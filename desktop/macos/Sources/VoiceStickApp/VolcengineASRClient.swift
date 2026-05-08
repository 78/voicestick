import Foundation
import CZlib

enum ASRResultType: String {
    case full
    case single
}

struct ASRSessionOptions {
    var hotwords: [String] = []
    var resultType: ASRResultType = .full
    var showUtterances: Bool = false
}

struct ASRSegment {
    let text: String
    let definite: Bool
    let startTime: Int?
    let endTime: Int?
}

protocol ASRClient: AnyObject {
    var onPartial: ((String) -> Void)? { get set }
    var onSegment: ((ASRSegment) -> Void)? { get set }
    var onFinal: ((String) -> Void)? { get set }
    var onError: ((String) -> Void)? { get set }
    var onUpgradeURL: ((URL) -> Void)? { get set }

    func start(options: ASRSessionOptions) -> Bool
    func sendOggOpusChunk(_ data: Data, isLast: Bool)
    func finish()
    func cancel()
}

final class VolcengineASRClient: ASRClient {
    private enum ConnectionState {
        case disconnected
        case connecting
        case ready
        case closing
    }

    private enum SessionState {
        case idle
        case starting
        case streaming
        case finishing
    }

    private enum ASREvent: UInt32 {
        case startConnection = 1
        case finishConnection = 2
        case connectionStarted = 50
        case connectionFailed = 51
        case connectionFinished = 52
        case startSession = 100
        case cancelSession = 101
        case finishSession = 102
        case sessionStarted = 150
        case sessionCanceled = 151
        case sessionFinished = 152
        case usageResponse = 154
        case taskRequest = 200
        case asrInfo = 450
        case asrResponse = 451
        case asrEnd = 459
    }

    private struct QueuedAudioChunk {
        let data: Data
        let isLast: Bool
    }

    private struct EventResponse {
        let event: ASREvent?
        let eventID: UInt32
        let sessionID: String?
        let payloadText: String?
    }

    private let config: AppConfig
    private let resultType: ASRResultType
    private let showUtterances: Bool
    private let queue = DispatchQueue(label: "VoiceStick.VolcengineASRClient")
    private var webSocket: URLSessionWebSocketTask?
    private var connectionState: ConnectionState = .disconnected
    private var sessionState: SessionState = .idle
    private var currentSessionID: String?
    private var queuedAudioChunks: [QueuedAudioChunk] = []
    private var latestSessionTranscript = ""
    private var emittedDefiniteSegmentKeys: Set<String> = []
    private var sessionOptions = ASRSessionOptions()
    private var sequence: Int32 = 0
    private var finished = false

    var onPartial: ((String) -> Void)?
    var onSegment: ((ASRSegment) -> Void)?
    var onFinal: ((String) -> Void)?
    var onError: ((String) -> Void)?
    var onUpgradeURL: ((URL) -> Void)?

    init(config: AppConfig, resultType: ASRResultType = .full, showUtterances: Bool = false) {
        self.config = config
        self.resultType = resultType
        self.showUtterances = showUtterances
    }

    deinit {
        webSocket?.cancel(with: .goingAway, reason: nil)
    }

    @discardableResult
    func start(options: ASRSessionOptions) -> Bool {
        if config.asrProvider == .volcengine {
            return startReusableSession(options: options)
        }
        return startLegacySession(options: options)
    }

    private func startLegacySession(options: ASRSessionOptions) -> Bool {
        let apiKey = activeAPIKey
        guard !apiKey.isEmpty else {
            let message = "Missing ASR API key"
            NSLog("ASR config error: \(message)")
            onError?(message)
            return false
        }

        guard let url = URL(string: activeWebSocketURL) else {
            onError?("Invalid ASR URL")
            return false
        }

        var request = URLRequest(url: url)
        let connectID = UUID().uuidString
        request.setValue(apiKey, forHTTPHeaderField: "X-Api-Key")
        if config.asrProvider == .volcengine {
            request.setValue(config.resourceID, forHTTPHeaderField: "X-Api-Resource-Id")
        }
        request.setValue(connectID, forHTTPHeaderField: "X-Api-Request-Id")
        request.setValue("-1", forHTTPHeaderField: "X-Api-Sequence")

        sequence = 0
        sessionOptions = options
        emittedDefiniteSegmentKeys.removeAll(keepingCapacity: true)
        finished = false
        NSLog("ASR start provider=\(config.asrProvider.rawValue) connect_id=\(connectID)")
        let task = URLSession.shared.webSocketTask(with: request)
        webSocket = task
        task.resume()
        receiveLoop()
        sendFullClientRequest()
        return true
    }

    private func startReusableSession(options: ASRSessionOptions) -> Bool {
        let apiKey = activeAPIKey
        guard !apiKey.isEmpty else {
            let message = "Missing ASR API key"
            NSLog("ASR config error: \(message)")
            onError?(message)
            return false
        }

        guard URL(string: activeWebSocketURL) != nil else {
            onError?("Invalid ASR URL")
            return false
        }

        queue.async { [weak self] in
            self?.beginReusableSession(options: options)
        }
        return true
    }

    private var activeAPIKey: String {
        switch config.asrProvider {
        case .voiceStickCloud:
            return config.voiceStickAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
        case .volcengine:
            return config.volcengineAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
        }
    }

    private var activeWebSocketURL: String {
        switch config.asrProvider {
        case .voiceStickCloud:
            return config.voiceStickCloudURL.trimmingCharacters(in: .whitespacesAndNewlines)
        case .volcengine:
            return AppConfig.volcengineWebSocketURL
        }
    }

    func sendOggOpusChunk(_ data: Data, isLast: Bool) {
        if config.asrProvider == .volcengine {
            queue.async { [weak self] in
                self?.sendReusableAudio(data, isLast: isLast)
            }
            return
        }

        if isLast {
            finished = true
        }
        sendAudio(data, isLast: isLast)
    }

    func finish() {
        if config.asrProvider == .volcengine {
            queue.async { [weak self] in
                self?.finishReusableSessionIfNeeded()
            }
            return
        }

        guard !finished else { return }
        finished = true
        sendAudio(Data(), isLast: true)
    }

    func cancel() {
        if config.asrProvider == .volcengine {
            queue.async { [weak self] in
                self?.cancelReusable()
            }
            return
        }

        finished = true
        webSocket?.cancel(with: .goingAway, reason: nil)
        webSocket = nil
    }

    private func beginReusableSession(options: ASRSessionOptions) {
        guard sessionState == .idle else {
            notifyError("ASR session already active")
            return
        }

        currentSessionID = UUID().uuidString
        sessionOptions = options
        latestSessionTranscript = ""
        emittedDefiniteSegmentKeys.removeAll(keepingCapacity: true)
        queuedAudioChunks.removeAll(keepingCapacity: true)
        sessionState = .starting

        switch connectionState {
        case .ready:
            sendStartSession()
        case .disconnected:
            connectReusableWebSocket()
        case .connecting:
            break
        case .closing:
            closeReusableWebSocket(sendFinishConnection: false)
            connectReusableWebSocket()
        }
    }

    private func connectReusableWebSocket() {
        guard let url = URL(string: activeWebSocketURL) else {
            failReusableSession("Invalid ASR URL")
            return
        }

        var request = URLRequest(url: url)
        let connectID = UUID().uuidString
        request.setValue(activeAPIKey, forHTTPHeaderField: "X-Api-Key")
        request.setValue(config.resourceID, forHTTPHeaderField: "X-Api-Resource-Id")
        request.setValue(connectID, forHTTPHeaderField: "X-Api-Request-Id")
        request.setValue("-1", forHTTPHeaderField: "X-Api-Sequence")

        connectionState = .connecting
        finished = false
        NSLog("ASR reusable connect provider=\(config.asrProvider.rawValue) request_id=\(connectID)")
        let task = URLSession.shared.webSocketTask(with: request)
        webSocket = task
        task.resume()
        receiveLoop()
        sendEvent(.startConnection, sessionID: nil, payload: connectionPayload())
    }

    private func sendStartSession() {
        guard let currentSessionID else {
            failReusableSession("Missing ASR session ID")
            return
        }

        NSLog("ASR reusable start_session session_id=\(currentSessionID)")
        sendEvent(.startSession, sessionID: currentSessionID, payload: sessionPayload())
    }

    private func sendReusableAudio(_ data: Data, isLast: Bool) {
        switch sessionState {
        case .starting:
            queuedAudioChunks.append(QueuedAudioChunk(data: data, isLast: isLast))
        case .streaming:
            sendTaskRequest(data)
            if isLast {
                finishReusableSessionIfNeeded()
            }
        case .finishing:
            break
        case .idle:
            queuedAudioChunks.append(QueuedAudioChunk(data: data, isLast: isLast))
        }
    }

    private func flushQueuedAudioChunks() {
        let chunks = queuedAudioChunks
        queuedAudioChunks.removeAll(keepingCapacity: true)
        for chunk in chunks {
            sendTaskRequest(chunk.data)
            if chunk.isLast {
                finishReusableSessionIfNeeded()
            }
        }
    }

    private func finishReusableSessionIfNeeded() {
        if sessionState == .starting {
            if !queuedAudioChunks.contains(where: \.isLast) {
                queuedAudioChunks.append(QueuedAudioChunk(data: Data(), isLast: true))
            }
            return
        }
        guard sessionState == .streaming else { return }
        guard let currentSessionID else { return }
        sessionState = .finishing
        NSLog("ASR reusable finish_session session_id=\(currentSessionID)")
        sendEvent(.finishSession, sessionID: currentSessionID, payload: connectionPayload())
    }

    private func cancelReusable() {
        if sessionState == .starting || sessionState == .streaming || sessionState == .finishing {
            if let currentSessionID {
                sendEvent(.cancelSession, sessionID: currentSessionID, payload: connectionPayload())
            }
        }

        queuedAudioChunks.removeAll(keepingCapacity: true)
        latestSessionTranscript = ""
        emittedDefiniteSegmentKeys.removeAll(keepingCapacity: true)
        currentSessionID = nil
        sessionState = .idle
    }

    private func closeReusableWebSocket(sendFinishConnection: Bool) {
        guard let task = webSocket else {
            connectionState = .disconnected
            return
        }

        let shouldSendFinishConnection = sendFinishConnection && connectionState == .ready
        finished = true
        connectionState = .closing
        if shouldSendFinishConnection {
            sendEvent(.finishConnection, sessionID: nil, payload: connectionPayload(), closeAfterSend: true)
            return
        }

        task.cancel(with: .goingAway, reason: nil)
        webSocket = nil
        connectionState = .disconnected
    }

    private func failReusableSession(_ message: String) {
        queuedAudioChunks.removeAll(keepingCapacity: true)
        latestSessionTranscript = ""
        emittedDefiniteSegmentKeys.removeAll(keepingCapacity: true)
        currentSessionID = nil
        sessionState = .idle
        closeReusableWebSocket(sendFinishConnection: false)
        notifyError(message)
    }

    private func notifyError(_ message: String) {
        DispatchQueue.main.async { [weak self] in
            self?.onError?(message)
        }
    }

    private func sendFullClientRequest() {
        var request: [String: Any] = [
            "model_name": "bigmodel",
            "enable_nonstream": true,
            "show_utterances": sessionOptions.showUtterances,
            "result_type": sessionOptions.resultType.rawValue,
            "enable_ddc": true
        ]
        addHotwordsIfNeeded(to: &request)

        let payload: [String: Any] = [
            "user": ["uid": "voice-stick-local"],
            "audio": ["format": "ogg", "codec": "opus", "rate": 16_000, "bits": 16, "channel": 1],
            "request": request
        ]

        guard let payloadData = try? JSONSerialization.data(withJSONObject: payload) else {
            onError?("Invalid ASR request JSON")
            return
        }
        sendBinaryFrame(
            messageType: 0x01,
            flags: 0x00,
            serialization: 0x01,
            compression: 0x01,
            payload: payloadData
        )
    }

    private func sendAudio(_ data: Data, isLast: Bool) {
        sendBinaryFrame(
            messageType: 0x02,
            flags: isLast ? 0x02 : 0x00,
            serialization: 0x00,
            compression: 0x01,
            payload: data
        )
    }

    private func sendEvent(
        _ event: ASREvent,
        sessionID: String?,
        payload: [String: Any],
        closeAfterSend: Bool = false
    ) {
        let requestPayload = event == .startSession ? sessionPayload() : payload
        guard let payloadData = try? JSONSerialization.data(withJSONObject: requestPayload) else {
            failReusableSession("Invalid ASR request JSON")
            return
        }

        sendEventFrame(
            messageType: 0x01,
            event: event,
            sessionID: sessionID,
            serialization: 0x01,
            payload: payloadData,
            closeAfterSend: closeAfterSend
        )
    }

    private func sendTaskRequest(_ data: Data) {
        guard let currentSessionID else { return }
        sendEventFrame(
            messageType: 0x02,
            event: .taskRequest,
            sessionID: currentSessionID,
            serialization: 0x00,
            payload: data
        )
    }

    private func sendEventFrame(
        messageType: UInt8,
        event: ASREvent,
        sessionID: String?,
        serialization: UInt8,
        payload: Data,
        closeAfterSend: Bool = false
    ) {
        guard let sendingTask = webSocket else {
            failReusableSession("ASR WebSocket is not connected")
            return
        }

        do {
            let compressed = try payload.gzipCompressed()
            var frame = Data()
            frame.append(0x11)
            frame.append((messageType << 4) | 0x04)
            frame.append((serialization << 4) | 0x01)
            frame.append(0x00)
            frame.append(contentsOf: event.rawValue.bigEndianBytes)
            if let sessionID {
                let sessionData = Data(sessionID.utf8)
                frame.append(contentsOf: UInt32(sessionData.count).bigEndianBytes)
                frame.append(sessionData)
            }
            frame.append(contentsOf: UInt32(compressed.count).bigEndianBytes)
            frame.append(compressed)
            sendingTask.send(.data(frame)) { [weak self] error in
                if let error {
                    NSLog("ASR send event error: \(error.localizedDescription)")
                    self?.queue.async {
                        self?.failReusableSession(error.localizedDescription)
                    }
                    return
                }

                guard closeAfterSend else { return }
                self?.queue.async {
                    guard let self, self.webSocket === sendingTask else { return }
                    sendingTask.cancel(with: .goingAway, reason: nil)
                    self.webSocket = nil
                    self.connectionState = .disconnected
                }
            }
        } catch {
            NSLog("ASR gzip error: \(error.localizedDescription)")
            failReusableSession(error.localizedDescription)
        }
    }

    private func sendBinaryFrame(
        messageType: UInt8,
        flags: UInt8,
        serialization: UInt8,
        compression: UInt8,
        payload: Data
    ) {
        do {
            let compressed = try payload.gzipCompressed()
            var frame = Data()
            frame.append(0x11)
            frame.append((messageType << 4) | flags)
            frame.append((serialization << 4) | compression)
            frame.append(0x00)
            frame.append(contentsOf: UInt32(compressed.count).bigEndianBytes)
            frame.append(compressed)
            webSocket?.send(.data(frame)) { [weak self] error in
                if let error {
                    NSLog("ASR send error: \(error.localizedDescription)")
                    if self?.finished != true {
                        self?.onError?(error.localizedDescription)
                    }
                }
            }
        } catch {
            NSLog("ASR gzip error: \(error.localizedDescription)")
            onError?(error.localizedDescription)
        }
    }

    private func receiveLoop() {
        let receivingTask = webSocket
        receivingTask?.receive { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let message):
                self.queue.async {
                    guard self.webSocket === receivingTask else { return }
                    self.handle(message)
                    self.receiveLoop()
                }
            case .failure(let error):
                self.queue.async {
                    guard self.webSocket === receivingTask else { return }
                    if !self.finished {
                        NSLog("ASR receive error: \(error.localizedDescription)")
                        if self.config.asrProvider == .volcengine {
                            if self.sessionState == .idle {
                                self.webSocket = nil
                                self.connectionState = .disconnected
                            } else {
                                self.failReusableSession(error.localizedDescription)
                            }
                        } else {
                            self.onError?(error.localizedDescription)
                        }
                    }
                }
            }
        }
    }

    private func handle(_ message: URLSessionWebSocketTask.Message) {
        switch message {
        case .string(let text):
            handleASRText(text, isFinalResponse: false)
        case .data(let data):
            if config.asrProvider == .volcengine {
                handleReusableBinaryResponse(data)
            } else {
                handleBinaryResponse(data)
            }
        @unknown default:
            break
        }
    }

    private func handleBinaryResponse(_ data: Data) {
        guard data.count >= 4 else {
            onError?("Short ASR response")
            return
        }

        let messageType = data[1] >> 4
        let flags = data[1] & 0x0f
        let compression = data[2] & 0x0f
        var offset = Int(data[0] & 0x0f) * 4

        switch messageType {
        case 0x09:
            if flags == 0x01 || flags == 0x03 {
                guard data.count >= offset + 4 else { return }
                sequence = Int32(bigEndianBytes: data[offset..<(offset + 4)])
                offset += 4
            }
            guard data.count >= offset + 4 else { return }
            let payloadSize = Int(UInt32(bigEndianBytes: data[offset..<(offset + 4)]))
            offset += 4
            guard data.count >= offset + payloadSize else { return }
            let payload = data.subdata(in: offset..<(offset + payloadSize))
            do {
                let body = compression == 0x01 ? try payload.gzipDecompressed() : payload
                if let text = String(data: body, encoding: .utf8) {
                    logASRResponse(text, isFinalResponse: flags == 0x03)
                    handleASRText(text, isFinalResponse: flags == 0x03)
                }
            } catch {
                NSLog("ASR decompress error: \(error.localizedDescription)")
                onError?(error.localizedDescription)
            }

        case 0x0f:
            guard data.count >= offset + 8 else {
                onError?("Malformed ASR error response")
                return
            }
            let code = UInt32(bigEndianBytes: data[offset..<(offset + 4)])
            offset += 4
            let messageSize = Int(UInt32(bigEndianBytes: data[offset..<(offset + 4)]))
            offset += 4
            guard data.count >= offset + messageSize else {
                onError?("Malformed ASR error response")
                return
            }
            let message = String(data: data.subdata(in: offset..<(offset + messageSize)), encoding: .utf8) ?? "Unknown ASR error"
            NSLog("ASR server error code=\(code): \(message)")
            let parsedError = parsedErrorMessage(code: code, message: message)
            onError?(parsedError.message)
            if let upgradeURL = parsedError.upgradeURL {
                onUpgradeURL?(upgradeURL)
            }

        default:
            NSLog("ASR unhandled response type=\(messageType) bytes=\(data.count)")
        }
    }

    private func handleReusableBinaryResponse(_ data: Data) {
        guard data.count >= 4 else {
            failReusableSession("Short ASR response")
            return
        }

        let messageType = data[1] >> 4
        let flags = data[1] & 0x0f
        let compression = data[2] & 0x0f
        var offset = Int(data[0] & 0x0f) * 4

        if messageType == 0x0f {
            handleReusableErrorResponse(data, offset: offset, compression: compression)
            return
        }

        guard messageType == 0x09 || messageType == 0x0b else {
            NSLog("ASR reusable unhandled response type=\(messageType) bytes=\(data.count)")
            return
        }
        guard flags == 0x04 else {
            handleBinaryResponse(data)
            return
        }
        guard let response = parseEventResponse(data, offset: &offset, compression: compression) else {
            return
        }

        switch response.event {
        case .connectionStarted:
            connectionState = .ready
            NSLog("ASR reusable connection_started connect_id=\(response.sessionID ?? "")")
            if sessionState == .starting {
                sendStartSession()
            }

        case .connectionFinished:
            connectionState = .disconnected
            webSocket = nil
            NSLog("ASR reusable connection_finished")

        case .connectionFailed:
            failReusableSession(response.payloadText ?? "ASR connection failed")

        case .sessionStarted:
            guard response.sessionID == currentSessionID else { return }
            sessionState = .streaming
            NSLog("ASR reusable session_started session_id=\(response.sessionID ?? "")")
            flushQueuedAudioChunks()

        case .asrResponse, .asrInfo:
            guard response.sessionID == currentSessionID, let text = response.payloadText else { return }
            logASRResponse(text, isFinalResponse: false)
            let transcript = extractTranscript(from: text)
            let definiteSegments = extractNewDefiniteSegments(from: text)
            if !transcript.isEmpty {
                latestSessionTranscript = transcript
                DispatchQueue.main.async { [weak self] in
                    self?.onPartial?(transcript)
                }
            }
            if !definiteSegments.isEmpty {
                DispatchQueue.main.async { [weak self] in
                    for segment in definiteSegments {
                        self?.onSegment?(segment)
                    }
                }
            }

        case .asrEnd:
            break

        case .sessionFinished:
            guard response.sessionID == currentSessionID else { return }
            let finalText = response.payloadText.flatMap { text in
                let transcript = extractTranscript(from: text)
                return transcript.isEmpty ? nil : transcript
            } ?? latestSessionTranscript
            let definiteSegments = response.payloadText.map {
                extractNewDefiniteSegments(from: $0)
            } ?? []
            NSLog("ASR reusable session_finished session_id=\(response.sessionID ?? "") text_len=\(finalText.count)")
            currentSessionID = nil
            latestSessionTranscript = ""
            emittedDefiniteSegmentKeys.removeAll(keepingCapacity: true)
            queuedAudioChunks.removeAll(keepingCapacity: true)
            sessionState = .idle
            DispatchQueue.main.async { [weak self] in
                for segment in definiteSegments {
                    self?.onSegment?(segment)
                }
                self?.onFinal?(finalText)
            }

        case .sessionCanceled:
            if response.sessionID == currentSessionID {
                currentSessionID = nil
                latestSessionTranscript = ""
                emittedDefiniteSegmentKeys.removeAll(keepingCapacity: true)
                queuedAudioChunks.removeAll(keepingCapacity: true)
                sessionState = .idle
            }

        case .usageResponse:
            break

        case nil:
            NSLog("ASR reusable unknown event=\(response.eventID) bytes=\(data.count)")

        default:
            NSLog("ASR reusable ignored event=\(response.eventID)")
        }
    }

    private func parseEventResponse(_ data: Data, offset: inout Int, compression: UInt8) -> EventResponse? {
        guard data.count >= offset + 4 else { return nil }
        let eventID = UInt32(bigEndianBytes: data[offset..<(offset + 4)])
        offset += 4

        var sessionID: String?
        if data.count >= offset + 4 {
            let sessionIDSize = Int(UInt32(bigEndianBytes: data[offset..<(offset + 4)]))
            offset += 4
            guard data.count >= offset + sessionIDSize else { return nil }
            sessionID = String(data: data.subdata(in: offset..<(offset + sessionIDSize)), encoding: .utf8)
            offset += sessionIDSize
        }

        guard data.count >= offset + 4 else {
            return EventResponse(event: ASREvent(rawValue: eventID), eventID: eventID, sessionID: sessionID, payloadText: nil)
        }
        let payloadSize = Int(UInt32(bigEndianBytes: data[offset..<(offset + 4)]))
        offset += 4
        guard data.count >= offset + payloadSize else { return nil }
        let payload = data.subdata(in: offset..<(offset + payloadSize))
        let body: Data
        do {
            body = compression == 0x01 ? try payload.gzipDecompressed() : payload
        } catch {
            failReusableSession(error.localizedDescription)
            return nil
        }
        let payloadText = String(data: body, encoding: .utf8)
        return EventResponse(event: ASREvent(rawValue: eventID), eventID: eventID, sessionID: sessionID, payloadText: payloadText)
    }

    private func handleReusableErrorResponse(_ data: Data, offset: Int, compression: UInt8) {
        var offset = offset
        guard data.count >= offset + 8 else {
            failReusableSession("Malformed ASR error response")
            return
        }
        let code = UInt32(bigEndianBytes: data[offset..<(offset + 4)])
        offset += 4
        let messageSize = Int(UInt32(bigEndianBytes: data[offset..<(offset + 4)]))
        offset += 4
        guard data.count >= offset + messageSize else {
            failReusableSession("Malformed ASR error response")
            return
        }
        let payload = data.subdata(in: offset..<(offset + messageSize))
        let body: Data
        do {
            body = compression == 0x01 ? try payload.gzipDecompressed() : payload
        } catch {
            failReusableSession(error.localizedDescription)
            return
        }
        let message = String(data: body, encoding: .utf8) ?? "Unknown ASR error"
        NSLog("ASR server error code=\(code): \(message)")
        let parsedError = parsedErrorMessage(code: code, message: message)
        failReusableSession(parsedError.message)
        if let upgradeURL = parsedError.upgradeURL {
            DispatchQueue.main.async { [weak self] in
                self?.onUpgradeURL?(upgradeURL)
            }
        }
    }

    private func connectionPayload() -> [String: Any] {
        [
            "namespace": "BidirectionalASR",
            "event": 0,
            "req_params": sessionPayload()
        ]
    }

    private func sessionPayload() -> [String: Any] {
        var request: [String: Any] = [
            "model_name": "bigmodel",
            "enable_nonstream": true,
            "show_utterances": sessionOptions.showUtterances,
            "result_type": sessionOptions.resultType.rawValue,
            "enable_ddc": true
        ]
        addHotwordsIfNeeded(to: &request)

        return [
            "user": ["uid": "voice-stick-local"],
            "audio": ["format": "ogg", "codec": "opus", "rate": 16_000, "bits": 16, "channel": 1],
            "request": request
        ]
    }

    private func addHotwordsIfNeeded(to request: inout [String: Any]) {
        let hotwords = sessionOptions.hotwords
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        guard !hotwords.isEmpty else { return }

        let context: [String: Any] = [
            "hotwords": hotwords.map { ["word": $0] }
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: context),
              let contextString = String(data: data, encoding: .utf8) else {
            NSLog("ASR hotwords context JSON serialization failed")
            return
        }

        request["corpus"] = ["context": contextString]
    }

    private func parsedErrorMessage(code: UInt32, message: String) -> (message: String, upgradeURL: URL?) {
        guard
            let data = message.data(using: .utf8),
            let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else {
            return ("ASR \(code): \(message)", nil)
        }

        let detail = (object["message"] as? String) ?? (object["error"] as? String) ?? message
        if let upgradeURL = object["upgrade_url"] as? String, !upgradeURL.isEmpty {
            return ("ASR \(code): \(detail)", URL(string: upgradeURL))
        }
        return ("ASR \(code): \(detail)", nil)
    }

    private func handleASRText(_ text: String, isFinalResponse: Bool) {
        let transcript = extractTranscript(from: text)
        let definiteSegments = extractNewDefiniteSegments(from: text)

        if isFinalResponse {
            onFinal?(transcript)
        } else if !transcript.isEmpty {
            onPartial?(transcript)
        }
        for segment in definiteSegments {
            onSegment?(segment)
        }
    }

    private func logASRResponse(_: String, isFinalResponse _: Bool) {
    }

    private func extractTranscript(from text: String) -> String {
        guard let object = parseJSONPayload(text) else {
            return text
        }

        return extractTranscript(from: object)
    }

    private func extractTranscript(from object: [String: Any]) -> String {
        if let result = object["result"] as? [String: Any] {
            if let value = result["text"] as? String {
                return value
            }
            if let utterances = result["utterances"] as? [[String: Any]] {
                let parts = utterances.compactMap { $0["text"] as? String }
                if !parts.isEmpty {
                    return parts.joined()
                }
            }
        }

        if let results = object["result"] as? [[String: Any]] {
            let parts = results.compactMap { $0["text"] as? String }
            if !parts.isEmpty {
                return parts.joined()
            }
        }

        return ""
    }

    private func extractNewDefiniteSegments(from text: String) -> [ASRSegment] {
        guard let object = parseJSONPayload(text) else { return [] }
        return extractSegments(from: object)
            .filter(\.definite)
            .filter { segment in
                let key = segmentKey(segment)
                if emittedDefiniteSegmentKeys.contains(key) {
                    return false
                }
                emittedDefiniteSegmentKeys.insert(key)
                return true
            }
    }

    private func extractSegments(from object: [String: Any]) -> [ASRSegment] {
        if let result = object["result"] as? [String: Any],
           let utterances = result["utterances"] as? [[String: Any]] {
            return utterances.compactMap(segment)
        }

        if let results = object["result"] as? [[String: Any]] {
            return results.flatMap { result in
                (result["utterances"] as? [[String: Any]])?.compactMap(segment) ?? []
            }
        }

        return []
    }

    private func segment(from utterance: [String: Any]) -> ASRSegment? {
        guard let text = utterance["text"] as? String, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return nil
        }
        return ASRSegment(
            text: text,
            definite: utterance["definite"] as? Bool ?? false,
            startTime: utterance["start_time"] as? Int,
            endTime: utterance["end_time"] as? Int
        )
    }

    private func segmentKey(_ segment: ASRSegment) -> String {
        "\(segment.startTime ?? -1):\(segment.endTime ?? -1):\(segment.text)"
    }

    private func parseJSONPayload(_ text: String) -> [String: Any]? {
        guard let data = text.data(using: .utf8) else { return nil }
        return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    }

    private func responseKind(_ isFinalResponse: Bool) -> String {
        isFinalResponse ? "final" : "partial"
    }
}

private extension String {
    var asrLogPreview: String {
        let normalized = split(whereSeparator: \.isNewline).joined(separator: " ")
        let limit = 48
        guard normalized.count > limit else { return normalized }
        return String(normalized.suffix(limit))
    }
}

private extension FixedWidthInteger {
    var bigEndianBytes: [UInt8] {
        withUnsafeBytes(of: self.bigEndian) { Array($0) }
    }
}

private extension UInt32 {
    init(bigEndianBytes bytes: Data.SubSequence) {
        self = bytes.reduce(0) { ($0 << 8) | UInt32($1) }
    }
}

private extension Int32 {
    init(bigEndianBytes bytes: Data.SubSequence) {
        self = Int32(bitPattern: UInt32(bigEndianBytes: bytes))
    }
}

private extension Data {
    func gzipCompressed() throws -> Data {
        try withZStream(input: self, operation: Z_DEFLATED, windowBits: MAX_WBITS + 16)
    }

    func gzipDecompressed() throws -> Data {
        try withZStream(input: self, operation: Z_DEFLATED, windowBits: MAX_WBITS + 16, decompress: true)
    }

    private func withZStream(input: Data, operation: Int32, windowBits: Int32, decompress: Bool = false) throws -> Data {
        var stream = z_stream()
        let initResult: Int32
        if decompress {
            initResult = inflateInit2_(&stream, windowBits, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size))
        } else {
            initResult = deflateInit2_(&stream, Z_DEFAULT_COMPRESSION, operation, windowBits, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size))
        }
        guard initResult == Z_OK else {
            throw ASRCompressionError.initFailed(initResult)
        }
        defer {
            if decompress {
                inflateEnd(&stream)
            } else {
                deflateEnd(&stream)
            }
        }

        let chunkSize = 16 * 1024
        var output = Data()
        var buffer = [UInt8](repeating: 0, count: chunkSize)

        return try input.withUnsafeBytes { rawBuffer in
            stream.next_in = UnsafeMutablePointer<Bytef>(mutating: rawBuffer.bindMemory(to: Bytef.self).baseAddress)
            stream.avail_in = uInt(input.count)

            repeat {
                let result = buffer.withUnsafeMutableBytes { outBuffer -> Int32 in
                    stream.next_out = outBuffer.bindMemory(to: Bytef.self).baseAddress
                    stream.avail_out = uInt(chunkSize)
                    return decompress ? inflate(&stream, Z_NO_FLUSH) : deflate(&stream, Z_FINISH)
                }

                guard result == Z_OK || result == Z_STREAM_END else {
                    throw ASRCompressionError.streamFailed(result)
                }

                output.append(buffer, count: chunkSize - Int(stream.avail_out))

                if result == Z_STREAM_END {
                    break
                }
            } while stream.avail_out == 0 || stream.avail_in > 0

            return output
        }
    }
}

private enum ASRCompressionError: LocalizedError {
    case initFailed(Int32)
    case streamFailed(Int32)

    var errorDescription: String? {
        switch self {
        case .initFailed(let code):
            return "zlib init failed: \(code)"
        case .streamFailed(let code):
            return "zlib stream failed: \(code)"
        }
    }
}
