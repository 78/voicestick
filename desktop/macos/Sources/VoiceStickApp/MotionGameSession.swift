import AppKit
import Darwin
import MotionInput

/// The host owns the BLE links. The game declares roles and requests assignments.
final class MotionGameSession {
    private let ble: BleCentral
    private var game: Process?
    private var endpoint: FileHandle?
    private var reader: DispatchSourceRead?
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
    var onDeviceReleased: ((UUID) -> Void)?

    init(ble: BleCentral) { self.ble = ble }

    private static func roleLabel(_ role: MotionRole?) -> String {
        switch role {
        case .racket: return "Racket"
        case .leftHand: return "Left Hand"
        case .rightHand: return "Right Hand"
        case nil: return "Motion"
        }
    }

    func open(executable: String, voiceBusy: @escaping () -> Bool) throws {
        if let game, game.isRunning {
            NSRunningApplication(processIdentifier: game.processIdentifier)?.activate(options: [.activateIgnoringOtherApps])
            return
        }
        finish()
        var fds: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_DGRAM, 0, &fds) == 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }
        let host = FileHandle(fileDescriptor: fds[0], closeOnDealloc: true)
        let peer = FileHandle(fileDescriptor: fds[1], closeOnDealloc: true)
        for fd in fds {
            guard fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 else {
                throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
            }
        }
        let child = Process()
        let bin = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
        child.executableURL = bin.appendingPathComponent(executable)
        child.arguments = ["--motion-stdin"]
        child.standardInput = peer
        child.terminationHandler = { [weak self] ended in
            DispatchQueue.main.async { if self?.game === ended { self?.finish() } }
        }
        try child.run()
        peer.closeFile()
        game = child
        endpoint = host
        self.voiceBusy = voiceBusy
        let source = DispatchSource.makeReadSource(fileDescriptor: host.fileDescriptor, queue: .main)
        source.setEventHandler { [weak self] in
            guard let self, self.game === child else { return }
            while let data = MotionWire.receive(on: host.fileDescriptor) {
                if let request = try? JSONDecoder().decode(InputRequest.self, from: data),
                   self.bindings.apply(request, devices: self.ble.motionInputs) {
                    if request.swingToJoin == true {
                        self.join = SwingJoin(roles: request.roles)
                        self.unavailable.removeAll()
                    } else if request.assignments != nil {
                        self.join = nil
                        self.unavailable.removeAll()
                    }
                    self.updateSession()
                } else {
                    self.publish()
                }
            }
        }
        source.setCancelHandler { host.closeFile() }
        reader = source
        source.resume()
        let timer = Timer(timeInterval: 0.8, repeats: true) { [weak self] _ in self?.updateSession() }
        heartbeat = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    func stop() {
        game?.terminate()
        finish()
    }

    func receive(_ frame: ImuStreamFrame, from id: UUID) {
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

        guard lastFrame[id] != nil, bindings.role(for: id) != nil, let endpoint,
              let wireFrame = frame.toLegacyMotionFrame() else { return }
        lastFrame[id] = ProcessInfo.processInfo.systemUptime
        if streaming.insert(id).inserted { publish() }
        MotionWire.send(MotionWire.sample(wireFrame, from: id), on: endpoint.fileDescriptor)
    }

    func ownsButtons(from id: UUID) -> Bool {
        lastFrame[id] != nil && bindings.role(for: id) != nil
    }

    func ownsDevice(_ id: UUID) -> Bool {
        ownsButtons(from: id) || probing.contains(id)
    }

    func identify(_ id: UUID) {
        guard !bindings.roles.isEmpty, let endpoint,
              ble.motionInputs.contains(where: { $0.id == id }),
              lastFrame[id] != nil || voiceBusy?() == false else { return }
        MotionWire.send(MotionWire.sample(Data([1, 0x13]), from: id), on: endpoint.fileDescriptor)
    }

    private func probeSwing(from id: UUID, token: Int) {
        guard voiceBusy?() == false,
              join?.report(id: id, token: token, now: ProcessInfo.processInfo.systemUptime) == true else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + SwingJoin.settleTime) { [weak self] in
            guard self?.join?.token == token else { return }
            self?.updateSession()
        }
    }

    private func updateSession() {
        guard game?.isRunning == true else { finish(); return }
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
                release(id)
            } else if now - seen >= 2.4 {
                release(id)
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
        publish()
    }

    private func publish() {
        guard let endpoint else { return }
        var roster = InputRoster()
        roster.swingPrompt = join?.prompt
        roster.devices = ble.motionInputs
        roster.assignments = bindings.assignments
        roster.streaming = streaming
        roster.unavailable = unavailable
        roster.busy = voiceBusy?() == true
        if let data = try? JSONEncoder().encode(roster) {
            MotionWire.send(data, on: endpoint.fileDescriptor)
        }
    }

    private func release(_ id: UUID) {
        ble.stopImu(for: id)
        activeRoles.removeValue(forKey: id)
        streaming.remove(id)
        lastFrame.removeValue(forKey: id)
        onDeviceReleased?(id)
    }

    private func finish() {
        heartbeat?.invalidate()
        heartbeat = nil
        let released = Set(lastFrame.keys).union(probing)
        for id in Array(lastFrame.keys) {
            ble.stopImu(for: id)
            activeRoles.removeValue(forKey: id)
            streaming.remove(id)
            lastFrame.removeValue(forKey: id)
        }
        for id in probing {
            ble.stopImu(for: id)
        }
        probing.removeAll()
        probeDetectors.removeAll()
        join = nil
        bindings = MotionBindings()
        unavailable.removeAll()
        reader?.cancel()
        reader = nil
        endpoint = nil
        voiceBusy = nil
        game = nil
        for id in released {
            onDeviceReleased?(id)
        }
    }
}
