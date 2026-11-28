import AppKit
import Foundation
import MotionInput
import Network

/// Exposes an open loopback WebSocket bridge (`ws://127.0.0.1:47892`) so web games
/// can receive 100 Hz binary 6-axis IMU frames, run swing-to-join binding, and handle buttons
/// after one-time user authorization per website Origin.
final class MotionWebBridge {
    static let defaultHTTPPort: UInt16 = 47891
    static let defaultWSPort: UInt16 = 47892
    static let authorizedOriginsDefaultsKey = "authorizedMotionOrigins"
    static let didRevokeOriginsNotification = Notification.Name("VoiceStickMotionWebBridgeDidRevokeOrigins")

    private let ble: BleCentral
    private let queue = DispatchQueue(label: "VoiceStickApp.MotionWebBridge")

    private var httpListener: NWListener?
    private var wsListener: NWListener?
    private var activeClient: NWConnection?
    private var activeClientOrigin: String?
    private var pendingOrigins: [String] = []
    private var sessionAuthorizedOrigins = Set<String>()
    private var isPromptingAuthorization = false
    private var inFlightBinaryFrames = 0

    // Motion session state (mirrors MotionGameSession, driven on main queue)
    private var heartbeat: Timer?
    private var join: SwingJoin?
    private var probing = Set<UUID>()
    private var probeDetectors: [UUID: SwingProbeDetector] = [:]
    private var bindings = MotionBindings()
    private var lastFrame: [UUID: TimeInterval] = [:]
    private var activeRoles: [UUID: MotionRole] = [:]
    private var streaming = Set<UUID>()
    private var unavailable = Set<UUID>()
    private var voiceBusy: (() -> Bool)?
    var onClientActivated: (() -> Void)?
    var onDeviceReleased: ((UUID) -> Void)?

    private(set) var isRunning = false
    let httpPort: UInt16
    let wsPort: UInt16

    init(ble: BleCentral, httpPort: UInt16 = defaultHTTPPort, wsPort: UInt16 = defaultWSPort) {
        self.ble = ble
        self.httpPort = httpPort
        self.wsPort = wsPort
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleOriginsRevoked),
            name: Self.didRevokeOriginsNotification,
            object: nil
        )
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    static func authorizedOrigins() -> [String] {
        UserDefaults.standard.stringArray(forKey: authorizedOriginsDefaultsKey) ?? []
    }

    static func saveAuthorizedOrigin(_ origin: String) {
        var list = authorizedOrigins()
        if !list.contains(origin) {
            list.append(origin)
            list.sort()
            UserDefaults.standard.set(list, forKey: authorizedOriginsDefaultsKey)
        }
    }

    static func revokeAllAuthorizedOrigins() {
        UserDefaults.standard.removeObject(forKey: authorizedOriginsDefaultsKey)
        NotificationCenter.default.post(name: didRevokeOriginsNotification, object: nil)
    }

    @objc private func handleOriginsRevoked() {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.sessionAuthorizedOrigins.removeAll()
            if let origin = self.activeClientOrigin, !Self.isLoopbackOrigin(origin) {
                self.disconnectClient()
            }
        }
    }

    var hasActiveClient: Bool { activeClient != nil }

    func start(voiceBusy: @escaping () -> Bool) throws {
        self.voiceBusy = voiceBusy
        guard !isRunning else { return }
        try startHTTPListener()
        try startWSListener()
        isRunning = true
        NSLog("MotionWebBridge listening: HTTP http://127.0.0.1:%u, WS ws://127.0.0.1:%u", httpPort, wsPort)
    }

    func stop() {
        httpListener?.cancel()
        httpListener = nil
        wsListener?.cancel()
        wsListener = nil
        isRunning = false
        DispatchQueue.main.async { [weak self] in
            self?.disconnectClient()
        }
    }

    func releaseMotionClient() {
        DispatchQueue.main.async { [weak self] in
            self?.disconnectClient()
        }
    }

    func openInBrowser() {
        let url: URL
        if let custom = ProcessInfo.processInfo.environment["VOICESTICK_GAMES_URL"],
           !custom.isEmpty,
           let envURL = URL(string: custom) {
            url = envURL
        } else {
            url = AppConfig.gamesURL
        }
        NSWorkspace.shared.open(url)
    }

    // MARK: - BLE hooks (called on main queue from VoiceStickCoordinator)

    private static func roleLabel(_ role: MotionRole?) -> String {
        switch role {
        case .racket: return "Racket"
        case .leftHand: return "Left Hand"
        case .rightHand: return "Right Hand"
        case nil: return "Motion"
        }
    }

    func receive(_ frame: ImuStreamFrame, from id: UUID) {
        guard activeClient != nil else { return }
        if probing.contains(id), let join, voiceBusy?() == false, frame.hasGyro,
           let gyroScale = ble.deviceInfo(for: id)?.streams?.imu?.gyroLsbPerDps {
            var detector = probeDetectors[id] ?? SwingProbeDetector()
            let triggered = detector.sample(
                gx: frame.samples[3], gy: frame.samples[4], gz: frame.samples[5],
                gyroLsbPerDps: gyroScale, timestampUs: Int64(frame.timestampMs) * 1000
            )
            probeDetectors[id] = detector
            if triggered {
                probeSwing(from: id, token: join.token)
            }
        }
        guard lastFrame[id] != nil, bindings.role(for: id) != nil,
              let wireFrame = frame.toLegacyMotionFrame() else { return }
        lastFrame[id] = ProcessInfo.processInfo.systemUptime
        if streaming.insert(id).inserted { publishRoster() }
        sendBinarySample(MotionWire.sample(wireFrame, from: id))
    }

    func ownsDevice(_ id: UUID) -> Bool {
        activeClient != nil && ((lastFrame[id] != nil && bindings.role(for: id) != nil) || probing.contains(id))
    }

    func identify(_ id: UUID) {
        guard activeClient != nil, !bindings.roles.isEmpty,
              ble.motionInputs.contains(where: { $0.id == id }),
              lastFrame[id] != nil || voiceBusy?() == false else { return }
        let payload: [String: Any] = ["type": "identify", "deviceId": id.uuidString]
        sendJSONObject(payload)
    }

    func handleButton(_ button: ButtonInput, from id: UUID) -> Bool {
        guard activeClient != nil, lastFrame[id] != nil, bindings.role(for: id) != nil else { return false }
        let payload: [String: Any] = [
            "type": "button",
            "deviceId": id.uuidString,
            "button": button.id,
            "action": button.action.rawValue,
            "timestamp_ms": button.timestampMs
        ]
        sendJSONObject(payload)
        return true
    }

    func probeSwing(from id: UUID, token: Int) {
        guard activeClient != nil, voiceBusy?() == false,
              join?.report(id: id, token: token, now: ProcessInfo.processInfo.systemUptime) == true else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + SwingJoin.settleTime) { [weak self] in
            guard self?.join?.token == token else { return }
            self?.updateSession()
        }
    }

    // MARK: - WebSocket Listener

    private func startWSListener() throws {
        let wsOptions = NWProtocolWebSocket.Options()
        wsOptions.autoReplyPing = true
        wsOptions.setClientRequestHandler(queue) { [weak self] _, headers in
            let rawOrigin = headers.first(where: { $0.name.caseInsensitiveCompare("Origin") == .orderedSame })?.value ?? ""
            let normalized = Self.normalizeOrigin(rawOrigin)
            guard Self.isStructurallyValidOrigin(normalized) else {
                NSLog("MotionWebBridge rejected malformed WS origin: %@", rawOrigin)
                return .init(status: .reject, subprotocol: nil, additionalHeaders: nil)
            }
            self?.pendingOrigins.append(normalized)
            return .init(status: .accept, subprotocol: nil, additionalHeaders: nil)
        }
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        params.requiredLocalEndpoint = NWEndpoint.hostPort(
            host: .ipv4(.loopback),
            port: NWEndpoint.Port(rawValue: wsPort)!
        )
        params.defaultProtocolStack.applicationProtocols.insert(wsOptions, at: 0)

        let listener = try NWListener(using: params)
        listener.stateUpdateHandler = { state in
            if case .failed(let error) = state {
                NSLog("MotionWebBridge WS listener failed: %@", error.localizedDescription)
            }
        }
        listener.newConnectionHandler = { [weak self] connection in
            self?.acceptWSConnection(connection)
        }
        listener.start(queue: queue)
        wsListener = listener
    }

    private func acceptWSConnection(_ connection: NWConnection) {
        let origin = pendingOrigins.isEmpty ? "" : pendingOrigins.removeFirst()
        DispatchQueue.main.async { [weak self] in
            guard let self else { connection.cancel(); return }
            if self.isOriginAuthorized(origin) {
                self.activateAuthorizedConnection(connection, origin: origin)
                return
            }
            // If an authorization dialog is already open, do not stack another prompt.
            if self.isPromptingAuthorization {
                connection.cancel()
                return
            }
            self.requestAuthorization(for: connection, origin: origin)
        }
    }

    private func isOriginAuthorized(_ origin: String) -> Bool {
        if Self.isLoopbackOrigin(origin) { return true }
        if sessionAuthorizedOrigins.contains(origin) { return true }
        return Self.authorizedOrigins().contains(origin)
    }

    private func requestAuthorization(for connection: NWConnection, origin: String) {
        isPromptingAuthorization = true
        let displayOrigin = origin.isEmpty ? "Unknown local client" : origin

        var connectionClosed = false
        connection.stateUpdateHandler = { state in
            if case .failed = state { connectionClosed = true }
            if case .cancelled = state { connectionClosed = true }
            if case .ready = state {
                let payload: [String: Any] = ["type": "auth_required", "origin": displayOrigin]
                if let data = try? JSONSerialization.data(withJSONObject: payload) {
                    let meta = NWProtocolWebSocket.Metadata(opcode: .text)
                    let ctx = NWConnection.ContentContext(identifier: "auth_required", metadata: [meta])
                    connection.send(content: data, contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
                }
            }
        }
        connection.start(queue: queue)

        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = "Allow \"\(displayOrigin)\" to use VoiceStick?"
        alert.informativeText = "This website wants to connect to your VoiceStick for 6-axis motion and button input in games. Microphone audio and firmware settings are never shared."
        alert.addButton(withTitle: "Allow Always")
        alert.addButton(withTitle: "Allow Once")
        alert.addButton(withTitle: "Deny")

        let response = alert.runModal()
        isPromptingAuthorization = false

        guard !connectionClosed else {
            connection.cancel()
            return
        }

        switch response {
        case .alertFirstButtonReturn:
            if !origin.isEmpty { Self.saveAuthorizedOrigin(origin) }
            activateAuthorizedConnection(connection, origin: origin, alreadyStarted: true)
        case .alertSecondButtonReturn:
            if !origin.isEmpty { sessionAuthorizedOrigins.insert(origin) }
            activateAuthorizedConnection(connection, origin: origin, alreadyStarted: true)
        default:
            let payload: [String: Any] = ["type": "auth_denied", "origin": displayOrigin]
            if let data = try? JSONSerialization.data(withJSONObject: payload) {
                let meta = NWProtocolWebSocket.Metadata(opcode: .text)
                let ctx = NWConnection.ContentContext(identifier: "auth_denied", metadata: [meta])
                connection.send(content: data, contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in
                    connection.cancel()
                })
            } else {
                connection.cancel()
            }
        }
    }

    private func activateAuthorizedConnection(_ connection: NWConnection, origin: String, alreadyStarted: Bool = false) {
        onClientActivated?()
        disconnectClient(notifySuperseded: true)
        activeClient = connection
        activeClientOrigin = origin
        inFlightBinaryFrames = 0

        let onReady = { [weak self] in
            guard let self, self.activeClient === connection else { return }
            self.startHeartbeat()
            self.sendJSONObject([
                "type": "welcome",
                "protocol": 1,
                "sampleRateHz": 100,
                "origin": origin,
                "hostUptime": ProcessInfo.processInfo.systemUptime
            ])
            self.updateSession()
            self.receiveNextWSMessage(on: connection)
        }

        connection.stateUpdateHandler = { [weak self] state in
            DispatchQueue.main.async {
                guard let self, self.activeClient === connection else { return }
                switch state {
                case .ready:
                    onReady()
                case .failed, .cancelled:
                    self.disconnectClient()
                default:
                    break
                }
            }
        }

        if alreadyStarted {
            if connection.state == .ready {
                onReady()
            }
        } else {
            connection.start(queue: queue)
        }
    }

    private func receiveNextWSMessage(on connection: NWConnection) {
        connection.receiveMessage { [weak self] data, context, _, error in
            guard let self else { return }
            if let error {
                NSLog("MotionWebBridge WS receive error: %@", error.localizedDescription)
                DispatchQueue.main.async {
                    if self.activeClient === connection { self.disconnectClient() }
                }
                return
            }
            if let metadata = context?.protocolMetadata(definition: NWProtocolWebSocket.definition) as? NWProtocolWebSocket.Metadata,
               metadata.opcode == .close {
                DispatchQueue.main.async {
                    if self.activeClient === connection { self.disconnectClient() }
                }
                return
            }
            if let data, !data.isEmpty {
                DispatchQueue.main.async {
                    guard self.activeClient === connection else { return }
                    self.handleClientTextMessage(data)
                }
            }
            self.receiveNextWSMessage(on: connection)
        }
    }

    private func handleClientTextMessage(_ data: Data) {
        // First check for control ping / hello messages
        if let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let action = json["action"] as? String {
            switch action {
            case "ping":
                sendJSONObject([
                    "type": "pong",
                    "clientTime": json["clientTime"] ?? 0,
                    "hostUptime": ProcessInfo.processInfo.systemUptime
                ])
                return
            case "release":
                _ = bindings.apply(InputRequest(roles: bindings.roles.isEmpty ? [.racket] : bindings.roles, assignments: [:]),
                                   devices: ble.motionInputs)
                join = nil
                unavailable.removeAll()
                updateSession()
                return
            default:
                break
            }
        }

        // Standard InputRequest (same Codable format as MotionGameSession)
        if let request = try? JSONDecoder().decode(InputRequest.self, from: data),
           bindings.apply(request, devices: ble.motionInputs) {
            if request.swingToJoin == true {
                join = SwingJoin(roles: request.roles)
                unavailable.removeAll()
            } else if request.assignments != nil {
                join = nil
                unavailable.removeAll()
            }
            updateSession()
        } else {
            publishRoster()
        }
    }

    private func startHeartbeat() {
        heartbeat?.invalidate()
        let timer = Timer(timeInterval: 0.8, repeats: true) { [weak self] _ in
            self?.updateSession()
        }
        heartbeat = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func updateSession() {
        guard activeClient != nil else { return }
        let devices = ble.motionInputs
        bindings.discover(devices)
        let connected = Set(devices.map(\.id))
        unavailable.formIntersection(connected)
        let now = ProcessInfo.processInfo.systemUptime
        let busy = voiceBusy?() != false
        join?.update(available: connected.intersection(ble.probeDevices), busy: busy, now: now)
        if let join, join.prompt.phase == .joined, bindings.assignments.isEmpty {
            _ = bindings.apply(InputRequest(roles: bindings.roles, assignments: join.chosen), devices: devices)
        }
        let nextProbes = join?.candidates ?? []
        let wanted = Set(bindings.assignments.values).intersection(connected)
        for id in probing.subtracting(nextProbes).subtracting(wanted) {
            ble.stopImu(for: id)
            probeDetectors.removeValue(forKey: id)
            onDeviceReleased?(id)
        }
        let addedProbes = nextProbes.subtracting(probing)
        probing = nextProbes
        for id in addedProbes {
            probeDetectors[id] = SwingProbeDetector()
            ble.setDisplay(scene: "idle", text: "Swing to join", to: id)
            ble.startImu(includeGyro: true, rateHz: 50, leaseMs: 3000, to: id)
        }
        for id in probing where !addedProbes.contains(id) {
            ble.startImu(includeGyro: true, rateHz: 50, leaseMs: 3000, to: id)
        }
        for (id, seen) in lastFrame {
            if !wanted.contains(id) || activeRoles[id] != bindings.role(for: id) {
                releaseDevice(id)
            } else if now - seen >= 2.4 {
                releaseDevice(id)
                unavailable.insert(id)
            } else if streaming.contains(id) {
                ble.renewImu(for: id)
            } else if !busy {
                ble.startImu(includeGyro: true, rateHz: 100, leaseMs: 3000, to: id)
            }
        }
        if !busy {
            for id in wanted where lastFrame[id] == nil && !unavailable.contains(id) {
                let role = bindings.role(for: id)
                activeRoles[id] = role
                lastFrame[id] = now
                probeDetectors.removeValue(forKey: id)
                ble.stopAudioIn(for: id)
                ble.setDisplay(scene: "idle", text: Self.roleLabel(role), to: id)
                ble.startImu(includeGyro: true, rateHz: 100, leaseMs: 3000, to: id)
            }
        }
        publishRoster()
    }

    private func publishRoster() {
        guard activeClient != nil else { return }
        var roster = InputRoster()
        roster.swingPrompt = join?.prompt
        roster.devices = ble.motionInputs
        roster.assignments = bindings.assignments
        roster.streaming = streaming
        roster.unavailable = unavailable
        roster.busy = voiceBusy?() == true

        struct RosterEnvelope: Encodable {
            let type = "roster"
            let hostUptime: Double
            let roster: InputRoster
        }
        if let data = try? JSONEncoder().encode(RosterEnvelope(hostUptime: ProcessInfo.processInfo.systemUptime, roster: roster)) {
            sendTextFrame(data)
        }
    }

    private func sendJSONObject(_ object: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: object) else { return }
        sendTextFrame(data)
    }

    private func sendTextFrame(_ data: Data) {
        guard let client = activeClient else { return }
        let meta = NWProtocolWebSocket.Metadata(opcode: .text)
        let ctx = NWConnection.ContentContext(identifier: "json", metadata: [meta])
        client.send(content: data, contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
    }

    private func sendBinarySample(_ data: Data) {
        guard let client = activeClient else { return }
        // Protect against tab throttling / backpressure buildup.
        guard inFlightBinaryFrames < 12 else { return }
        inFlightBinaryFrames += 1
        let meta = NWProtocolWebSocket.Metadata(opcode: .binary)
        let ctx = NWConnection.ContentContext(identifier: "imu", metadata: [meta])
        client.send(content: data, contentContext: ctx, isComplete: true, completion: .contentProcessed { [weak self] _ in
            DispatchQueue.main.async {
                guard let self, self.activeClient === client else { return }
                self.inFlightBinaryFrames = max(0, self.inFlightBinaryFrames - 1)
            }
        })
    }

    private func releaseDevice(_ id: UUID) {
        ble.stopImu(for: id)
        activeRoles.removeValue(forKey: id)
        streaming.remove(id)
        lastFrame.removeValue(forKey: id)
        onDeviceReleased?(id)
    }

    private func disconnectClient(notifySuperseded: Bool = false) {
        heartbeat?.invalidate()
        heartbeat = nil
        let released = Set(lastFrame.keys).union(probing)
        for id in Array(lastFrame.keys) {
            ble.stopImu(for: id)
            activeRoles.removeValue(forKey: id)
            streaming.remove(id)
            lastFrame.removeValue(forKey: id)
        }
        for id in probing { ble.stopImu(for: id) }
        probing.removeAll()
        probeDetectors.removeAll()
        join = nil
        bindings = MotionBindings()
        unavailable.removeAll()
        inFlightBinaryFrames = 0
        let old = activeClient
        activeClient = nil
        activeClientOrigin = nil
        for id in released {
            onDeviceReleased?(id)
        }
        if let old {
            if notifySuperseded, let data = try? JSONSerialization.data(withJSONObject: ["type": "superseded"]) {
                let meta = NWProtocolWebSocket.Metadata(opcode: .text)
                let ctx = NWConnection.ContentContext(identifier: "superseded", metadata: [meta])
                old.send(content: data, contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in
                    old.cancel()
                })
            } else {
                old.cancel()
            }
        }
    }

    // MARK: - Minimal Local HTTP Server (Static Studio Files + Dataset Persistence)

    private func startHTTPListener() throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        params.requiredLocalEndpoint = NWEndpoint.hostPort(
            host: .ipv4(.loopback),
            port: NWEndpoint.Port(rawValue: httpPort)!
        )
        let listener = try NWListener(using: params)
        listener.stateUpdateHandler = { state in
            if case .failed(let error) = state {
                NSLog("MotionWebBridge HTTP listener failed: %@", error.localizedDescription)
            }
        }
        listener.newConnectionHandler = { [weak self] connection in
            self?.handleHTTPConnection(connection)
        }
        listener.start(queue: queue)
        httpListener = listener
    }

    private func handleHTTPConnection(_ connection: NWConnection) {
        connection.start(queue: queue)
        receiveHTTPRequest(on: connection, buffer: Data())
    }

    private func receiveHTTPRequest(on connection: NWConnection, buffer: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] chunk, _, isComplete, error in
            guard let self else { connection.cancel(); return }
            if error != nil { connection.cancel(); return }
            var acc = buffer
            if let chunk { acc.append(chunk) }
            // Cap request body at 32 MB for large dataset saves.
            if acc.count > 32 * 1024 * 1024 {
                self.sendHTTP(on: connection, status: "413 Payload Too Large", contentType: "text/plain", body: Data("Too large".utf8))
                return
            }
            if let headerRange = acc.range(of: Data("\r\n\r\n".utf8)) {
                let headerData = acc.subdata(in: 0..<headerRange.lowerBound)
                let bodyStart = headerRange.upperBound
                let headerText = String(decoding: headerData, as: UTF8.self)
                let contentLength = Self.parseContentLength(headerText)
                if acc.count - bodyStart >= contentLength || isComplete {
                    let body = acc.subdata(in: bodyStart..<min(acc.count, bodyStart + contentLength))
                    self.dispatchHTTP(headerText: headerText, body: body, on: connection)
                    return
                }
            }
            if isComplete {
                connection.cancel()
            } else {
                self.receiveHTTPRequest(on: connection, buffer: acc)
            }
        }
    }

    private func dispatchHTTP(headerText: String, body: Data, on connection: NWConnection) {
        let lines = headerText.components(separatedBy: "\r\n")
        guard let requestLine = lines.first else {
            sendHTTP(on: connection, status: "400 Bad Request", contentType: "text/plain", body: Data("Bad Request".utf8))
            return
        }
        let parts = requestLine.split(separator: " ")
        guard parts.count >= 2 else {
            sendHTTP(on: connection, status: "400 Bad Request", contentType: "text/plain", body: Data("Bad Request".utf8))
            return
        }
        let method = String(parts[0])
        let rawTarget = String(parts[1])
        let components = URLComponents(string: "http://127.0.0.1" + rawTarget)
        let path = components?.path ?? "/"

        if method == "OPTIONS" {
            sendHTTP(on: connection, status: "204 No Content", contentType: "text/plain", body: Data())
            return
        }

        if path == "/api/info" && method == "GET" {
            let info: [String: Any] = [
                "wsUrl": "ws://127.0.0.1:\(wsPort)",
                "httpPort": httpPort,
                "wsPort": wsPort,
                "studioRoot": resolveStudioRoot()?.path ?? "",
                "datasetsDir": resolveDatasetsDirectory().path
            ]
            let data = (try? JSONSerialization.data(withJSONObject: info)) ?? Data("{}".utf8)
            sendHTTP(on: connection, status: "200 OK", contentType: "application/json; charset=utf-8", body: data)
            return
        }

        if path == "/api/datasets" && method == "GET" {
            let dir = resolveDatasetsDirectory()
            let files = (try? FileManager.default.contentsOfDirectory(
                at: dir,
                includingPropertiesForKeys: [.fileSizeKey, .contentModificationDateKey],
                options: [.skipsHiddenFiles]
            )) ?? []
            let list: [[String: Any]] = files
                .filter { $0.pathExtension == "jsonl" || $0.pathExtension == "json" }
                .sorted { $0.lastPathComponent > $1.lastPathComponent }
                .map { url in
                    let values = try? url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
                    return [
                        "name": url.lastPathComponent,
                        "path": url.path,
                        "bytes": values?.fileSize ?? 0,
                        "modified": values?.contentModificationDate?.timeIntervalSince1970 ?? 0
                    ]
                }
            let payload: [String: Any] = ["dir": dir.path, "files": list]
            let data = (try? JSONSerialization.data(withJSONObject: payload)) ?? Data("{}".utf8)
            sendHTTP(on: connection, status: "200 OK", contentType: "application/json; charset=utf-8", body: data)
            return
        }

        if path == "/api/datasets" && method == "POST" {
            let dir = resolveDatasetsDirectory()
            let requestedName = components?.queryItems?.first(where: { $0.name == "name" })?.value ?? ""
            let safeName = Self.sanitizeDatasetFilename(requestedName)
            let target = dir.appendingPathComponent(safeName)
            do {
                try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                try body.write(to: target, options: .atomic)
                let res: [String: Any] = ["ok": true, "name": safeName, "path": target.path, "bytes": body.count]
                let data = try JSONSerialization.data(withJSONObject: res)
                sendHTTP(on: connection, status: "200 OK", contentType: "application/json; charset=utf-8", body: data)
            } catch {
                let res: [String: Any] = ["ok": false, "error": error.localizedDescription]
                let data = (try? JSONSerialization.data(withJSONObject: res)) ?? Data("{}".utf8)
                sendHTTP(on: connection, status: "500 Internal Server Error", contentType: "application/json; charset=utf-8", body: data)
            }
            return
        }

        guard method == "GET" else {
            sendHTTP(on: connection, status: "405 Method Not Allowed", contentType: "text/plain", body: Data("Method Not Allowed".utf8))
            return
        }

        guard let studioRoot = resolveStudioRoot() else {
            let msg = "Tennis Studio assets not found. Expected games/tennis-studio next to workspace or set UserDefaults tennisStudioRoot."
            sendHTTP(on: connection, status: "404 Not Found", contentType: "text/plain; charset=utf-8", body: Data(msg.utf8))
            return
        }

        let relative = path == "/" ? "index.html" : String(path.dropFirst())
        let candidate = studioRoot.appendingPathComponent(relative).standardizedFileURL
        let rootPath = studioRoot.standardizedFileURL.path
        guard candidate.path.hasPrefix(rootPath),
              let fileData = try? Data(contentsOf: candidate) else {
            sendHTTP(on: connection, status: "404 Not Found", contentType: "text/plain; charset=utf-8", body: Data("Not Found".utf8))
            return
        }
        sendHTTP(on: connection, status: "200 OK", contentType: Self.mimeType(for: candidate.pathExtension), body: fileData)
    }

    private func sendHTTP(on connection: NWConnection, status: String, contentType: String, body: Data) {
        let header =
            "HTTP/1.1 \(status)\r\n" +
            "Content-Type: \(contentType)\r\n" +
            "Content-Length: \(body.count)\r\n" +
            "Cache-Control: no-store\r\n" +
            "Access-Control-Allow-Origin: *\r\n" +
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n" +
            "Access-Control-Allow-Headers: Content-Type\r\n" +
            "Connection: close\r\n\r\n"
        var response = Data(header.utf8)
        response.append(body)
        connection.send(content: response, completion: .contentProcessed { _ in
            connection.cancel()
        })
    }

    // MARK: - Helpers

    private func resolveStudioRoot() -> URL? {
        let fm = FileManager.default
        if let custom = UserDefaults.standard.string(forKey: "tennisStudioRoot"), !custom.isEmpty {
            let url = URL(fileURLWithPath: custom)
            if fm.fileExists(atPath: url.appendingPathComponent("index.html").path) { return url }
        }
        if let env = ProcessInfo.processInfo.environment["VOICESTICK_TENNIS_STUDIO"], !env.isEmpty {
            let url = URL(fileURLWithPath: env)
            if fm.fileExists(atPath: url.appendingPathComponent("index.html").path) { return url }
        }
        if let res = Bundle.main.resourceURL?.appendingPathComponent("tennis-studio"),
           fm.fileExists(atPath: res.appendingPathComponent("index.html").path) {
            return res
        }
        // Walk upward from current executable and current working directory to locate repo games/tennis-studio.
        let roots = [
            URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent(),
            URL(fileURLWithPath: fm.currentDirectoryPath)
        ]
        for start in roots {
            var dir = start.standardizedFileURL
            for _ in 0..<8 {
                let candidate = dir.appendingPathComponent("games/tennis-studio")
                if fm.fileExists(atPath: candidate.appendingPathComponent("index.html").path) {
                    return candidate
                }
                let parent = dir.deletingLastPathComponent()
                if parent.path == dir.path { break }
                dir = parent
            }
        }
        return nil
    }

    private func resolveDatasetsDirectory() -> URL {
        if let studio = resolveStudioRoot() {
            return studio.appendingPathComponent("datasets", isDirectory: true)
        }
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VoiceStick/TennisDatasets", isDirectory: true)
    }

    private static func normalizeOrigin(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, trimmed.lowercased() != "null",
              let comps = URLComponents(string: trimmed),
              let scheme = comps.scheme?.lowercased(),
              let host = comps.host?.lowercased() else {
            return ""
        }
        if let port = comps.port {
            return "\(scheme)://\(host):\(port)"
        }
        return "\(scheme)://\(host)"
    }

    private static func isStructurallyValidOrigin(_ normalized: String) -> Bool {
        if normalized.isEmpty { return true }
        return normalized.hasPrefix("https://") || normalized.hasPrefix("http://")
    }

    private static func isLoopbackOrigin(_ normalized: String) -> Bool {
        guard !normalized.isEmpty, let url = URL(string: normalized), let host = url.host?.lowercased() else {
            return false
        }
        return host == "127.0.0.1" || host == "localhost" || host == "::1"
    }

    private static func parseContentLength(_ headerText: String) -> Int {
        for line in headerText.components(separatedBy: "\r\n") {
            if line.lowercased().hasPrefix("content-length:") {
                let value = line.dropFirst("content-length:".count).trimmingCharacters(in: .whitespaces)
                return Int(value) ?? 0
            }
        }
        return 0
    }

    private static func sanitizeDatasetFilename(_ raw: String) -> String {
        let base = URL(fileURLWithPath: raw).lastPathComponent
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_."))
        let cleaned = String(base.unicodeScalars.filter { allowed.contains($0) })
        if !cleaned.isEmpty && (cleaned.hasSuffix(".jsonl") || cleaned.hasSuffix(".json")) {
            return cleaned
        }
        let fmt = ISO8601DateFormatter()
        let stamp = fmt.string(from: Date()).replacingOccurrences(of: ":", with: "-")
        return "tennis-session-\(stamp).jsonl"
    }

    private static func mimeType(for ext: String) -> String {
        switch ext.lowercased() {
        case "html", "htm": return "text/html; charset=utf-8"
        case "js", "mjs": return "application/javascript; charset=utf-8"
        case "css": return "text/css; charset=utf-8"
        case "json", "jsonl": return "application/json; charset=utf-8"
        case "svg": return "image/svg+xml"
        case "png": return "image/png"
        case "jpg", "jpeg": return "image/jpeg"
        case "glb": return "model/gltf-binary"
        case "gltf": return "model/gltf+json"
        case "wasm": return "application/wasm"
        default: return "application/octet-stream"
        }
    }
}
