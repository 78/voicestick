import Foundation

final class OpenAIRealtimeASRClient: ASRClient {
    private static let endpoint = URL(string: "wss://api.openai.com/v1/realtime?intent=transcription")!
    private static let model = "gpt-live-transcribe"

    private final class Turn {
        let id: UUID
        let decoder: OpusPCMDecoder
        let keywords: [String]
        var queuedPCM: [Data] = []
        var pcmBytes = 0
        var finishRequested = false
        var committed = false
        var itemID: String?
        var partialText = ""

        init(id: UUID, decoder: OpusPCMDecoder, keywords: [String]) {
            self.id = id
            self.decoder = decoder
            self.keywords = keywords
        }
    }

    private struct Event: Decodable {
        struct APIError: Decodable {
            let message: String?
        }

        let type: String
        let item_id: String?
        let delta: String?
        let transcript: String?
        let error: APIError?
    }

    private let apiKey: String
    private let queue = DispatchQueue(label: "VoiceStick.OpenAIRealtimeASRClient")
    private var socket: URLSessionWebSocketTask?
    private var configuredKeywords: [String]?
    private var turn: Turn?
    private var timeout: DispatchWorkItem?
    private var lastCompletedItemID: String?
    // The coordinator calls the client and receives callbacks on the main queue.
    private var callbackTurnID: UUID?

    var onPartial: ((String) -> Void)?
    var onSegment: ((ASRSegment) -> Void)?
    var onFinal: ((ASRFinalResult) -> Void)?
    var onError: ((_ message: String, _ partialText: String) -> Void)?
    var onUpgradeURL: ((URL, String) -> Void)?
    private(set) var lastStartError: String?

    init(config: AppConfig) {
        self.apiKey = config.openAIAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    deinit {
        socket?.cancel(with: .goingAway, reason: nil)
    }

    func start(options: ASRSessionOptions) -> Bool {
        guard !apiKey.isEmpty else {
            lastStartError = "Missing OpenAI ASR API key"
            return false
        }
        lastStartError = nil
        let id = UUID()
        callbackTurnID = id
        queue.async { [weak self] in
            self?.beginTurn(id: id, keywords: options.hotwords)
        }
        return true
    }

    func sendOggOpusChunk(_ data: Data, isLast: Bool) {
        queue.async { [weak self] in
            self?.sendAudio(data, isLast: isLast)
        }
    }

    func finish() {
        sendOggOpusChunk(Data(), isLast: true)
    }

    func cancel() {
        callbackTurnID = nil
        queue.async { [weak self] in
            guard let self, self.turn != nil else { return }
            self.resetTurn()
            self.closeSocket()
        }
    }

    private func beginTurn(id: UUID, keywords: [String]) {
        if turn != nil {
            resetTurn()
            closeSocket()
        }
        let active: Turn
        do {
            active = Turn(id: id, decoder: try OpusPCMDecoder(), keywords: keywords)
            turn = active
        } catch {
            callback(for: id) { $0.onError?(error.localizedDescription, "") }
            return
        }

        if socket != nil, configuredKeywords == active.keywords { return }
        closeSocket()
        var request = URLRequest(url: Self.endpoint)
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        let task = URLSession.shared.webSocketTask(with: request)
        socket = task
        task.resume()
        receive(from: task)
        startTimeout(for: active, seconds: 15, message: "OpenAI realtime session did not start in time.")
        var transcription: [String: Any] = ["model": Self.model]
        if !active.keywords.isEmpty {
            transcription["keywords"] = active.keywords
        }
        send([
            "type": "session.update",
            "session": [
                "type": "transcription",
                "audio": ["input": [
                    "format": ["type": "audio/pcm", "rate": OpusPCMDecoder.sampleRate],
                    "transcription": transcription,
                    "turn_detection": NSNull(),
                ]],
            ],
        ])
    }

    private func sendAudio(_ data: Data, isLast: Bool) {
        guard let active = turn, !active.finishRequested else { return }
        do {
            let pcm = try active.decoder.decode(data)
            if !pcm.isEmpty {
                active.pcmBytes += pcm.count
                if configuredKeywords != nil {
                    append(pcm)
                } else {
                    active.queuedPCM.append(pcm)
                }
            }
        } catch {
            fail(error.localizedDescription)
            return
        }
        if isLast {
            active.finishRequested = true
            commitIfReady(active)
        }
    }

    private func append(_ pcm: Data) {
        send(["type": "input_audio_buffer.append", "audio": pcm.base64EncodedString()])
    }

    private func commitIfReady(_ active: Turn) {
        guard turn === active, configuredKeywords != nil, active.finishRequested, !active.committed else { return }
        // OpenAI requires at least 100 ms of PCM in a manually committed turn.
        guard active.pcmBytes >= OpusPCMDecoder.sampleRate * MemoryLayout<Int16>.size / 10 else {
            closeSocket()
            complete(active, text: "")
            return
        }
        active.committed = true
        startTimeout(for: active, seconds: 20, message: "OpenAI did not return a final transcript in time.")
        send(["type": "input_audio_buffer.commit"])
    }

    private func send(_ event: [String: Any]) {
        guard let socket else { return }
        guard let data = try? JSONSerialization.data(withJSONObject: event) else {
            fail("Could not encode the OpenAI realtime request.")
            return
        }
        socket.send(.string(String(decoding: data, as: UTF8.self))) { [weak self, weak socket] error in
            guard let self, let socket, let error else { return }
            self.queue.async {
                guard self.socket === socket else { return }
                self.fail(error.localizedDescription)
            }
        }
    }

    private func receive(from socket: URLSessionWebSocketTask) {
        socket.receive { [weak self, weak socket] result in
            guard let self, let socket else { return }
            self.queue.async {
                guard self.socket === socket else { return }
                switch result {
                case .success(let message):
                    self.handle(message)
                    if self.socket === socket { self.receive(from: socket) }
                case .failure(let error):
                    self.fail(error.localizedDescription)
                }
            }
        }
    }

    private func handle(_ message: URLSessionWebSocketTask.Message) {
        let data: Data
        switch message {
        case .string(let text): data = Data(text.utf8)
        case .data(let bytes): data = bytes
        @unknown default: return
        }
        guard let event = try? JSONDecoder().decode(Event.self, from: data), let active = turn else { return }
        if let id = event.item_id, id == lastCompletedItemID || (active.itemID != nil && active.itemID != id) {
            return
        }
        switch event.type {
        case "session.updated":
            guard configuredKeywords == nil else { return }
            timeout?.cancel()
            configuredKeywords = active.keywords
            for pcm in active.queuedPCM {
                append(pcm)
            }
            active.queuedPCM.removeAll()
            commitIfReady(active)
        case "input_audio_buffer.committed":
            if active.committed { active.itemID = event.item_id }
        case "conversation.item.input_audio_transcription.delta":
            guard let delta = event.delta, !delta.isEmpty else { return }
            active.itemID = event.item_id ?? active.itemID
            active.partialText.append(delta)
            let text = active.partialText
            callback(for: active.id) { $0.onPartial?(text) }
        case "conversation.item.input_audio_transcription.completed":
            guard active.committed else { return }
            lastCompletedItemID = event.item_id
            complete(active, text: (event.transcript ?? active.partialText).trimmingCharacters(in: .whitespacesAndNewlines))
        case "error", "conversation.item.input_audio_transcription.failed":
            fail(event.error?.message ?? "OpenAI realtime transcription failed.")
        default:
            break
        }
    }

    private func complete(_ active: Turn, text: String) {
        resetTurn()
        callback(for: active.id) { $0.onFinal?(ASRFinalResult(text: text)) }
    }

    private func fail(_ message: String) {
        closeSocket()
        guard let active = turn else { return }
        resetTurn()
        let safeMessage = message.replacingOccurrences(of: apiKey, with: "[redacted]")
        let partialText = active.partialText
        callback(for: active.id) { $0.onError?(safeMessage, partialText) }
    }

    private func startTimeout(for active: Turn, seconds: TimeInterval, message: String) {
        timeout?.cancel()
        let id = active.id
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.turn?.id == id else { return }
            self.fail(message)
        }
        timeout = work
        queue.asyncAfter(deadline: .now() + seconds, execute: work)
    }

    private func resetTurn() {
        timeout?.cancel()
        timeout = nil
        turn = nil
    }

    private func closeSocket() {
        socket?.cancel(with: .goingAway, reason: nil)
        socket = nil
        configuredKeywords = nil
        lastCompletedItemID = nil
    }

    private func callback(for id: UUID, _ action: @escaping (OpenAIRealtimeASRClient) -> Void) {
        DispatchQueue.main.async { [weak self] in
            guard let self, self.callbackTurnID == id else { return }
            action(self)
        }
    }
}
