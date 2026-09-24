import Foundation

final class OpenAITranscriptionClient: ASRClient {
    private enum SessionState {
        case idle
        case buffering
        case uploading
        case finished
    }

    private let config: AppConfig
    private let queue = DispatchQueue(label: "VoiceStick.OpenAITranscriptionClient")
    private var sessionState: SessionState = .idle
    private var uploadTask: URLSessionDataTask?
    private var bufferedAudio = Data()
    private var sessionOptions = ASRSessionOptions()
    private var cancelled = false

    var onPartial: ((String) -> Void)?
    var onSegment: ((ASRSegment) -> Void)?
    var onFinal: ((String) -> Void)?
    var onError: ((String) -> Void)?
    var onUpgradeURL: ((URL, String) -> Void)?

    init(config: AppConfig) {
        self.config = config
    }

    deinit {
        uploadTask?.cancel()
    }

    @discardableResult
    func start(options: ASRSessionOptions) -> Bool {
        guard !apiKey.isEmpty else {
            notifyError("Missing OpenAI API key")
            return false
        }
        guard transcriptionURL != nil else {
            notifyError("Invalid OpenAI base URL")
            return false
        }
        queue.sync { [weak self] in
            guard let self else { return }
            self.cancelled = false
            self.bufferedAudio.removeAll(keepingCapacity: true)
            self.sessionOptions = options
            self.sessionState = .buffering
        }
        return true
    }

    func sendOggOpusChunk(_ data: Data, isLast: Bool) {
        queue.async { [weak self] in
            guard let self, !self.cancelled else { return }
            if !data.isEmpty {
                self.bufferedAudio.append(data)
            }
            if isLast {
                self.uploadTranscriptionIfNeeded()
            }
        }
    }

    func finish() {
        queue.async { [weak self] in
            self?.uploadTranscriptionIfNeeded()
        }
    }

    func cancel() {
        queue.async { [weak self] in
            guard let self else { return }
            self.cancelled = true
            self.uploadTask?.cancel()
            self.uploadTask = nil
            self.bufferedAudio.removeAll(keepingCapacity: true)
            self.sessionState = .idle
        }
    }

    private var apiKey: String {
        config.llmAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private var transcriptionURL: URL? {
        let trimmed = config.llmBaseURL.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        let base = trimmed.hasSuffix("/") ? String(trimmed.dropLast()) : trimmed
        return URL(string: base + "/audio/transcriptions")
    }

    private func uploadTranscriptionIfNeeded() {
        guard !cancelled else { return }
        guard sessionState == .buffering else { return }
        guard uploadTask == nil else { return }
        guard !bufferedAudio.isEmpty else {
            failSession("No audio data to transcribe")
            return
        }
        guard bufferedAudio.count <= 25 * 1024 * 1024 else {
            failSession("OpenAI transcription audio is too large")
            return
        }
        guard let url = transcriptionURL else {
            failSession("Invalid OpenAI base URL")
            return
        }

        sessionState = .uploading
        let boundary = "VoiceStick-\(UUID().uuidString)"
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        request.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        request.setValue("application/json", forHTTPHeaderField: "Accept")

        let body = makeMultipartBody(boundary: boundary, audio: bufferedAudio)
        let task = URLSession.shared.uploadTask(with: request, from: body) { [weak self] data, response, error in
            self?.queue.async {
                self?.uploadTask = nil
                self?.handleUploadCompletion(data: data, response: response, error: error)
            }
        }
        uploadTask = task
        task.resume()
    }

    private func handleUploadCompletion(data: Data?, response: URLResponse?, error: Error?) {
        guard !cancelled else { return }

        if let error {
            failSession(error.localizedDescription)
            return
        }
        guard let httpResponse = response as? HTTPURLResponse else {
            failSession("Invalid OpenAI response")
            return
        }

        guard (200...299).contains(httpResponse.statusCode) else {
            let message = extractErrorMessage(from: data) ?? responseSummary(httpResponse, data: data)
            failSession(message)
            return
        }

        let transcript = extractTranscript(from: data)
        guard !transcript.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            failSession("OpenAI transcription returned empty text")
            return
        }

        sessionState = .finished
        bufferedAudio.removeAll(keepingCapacity: true)
        DispatchQueue.main.async { [weak self] in
            self?.onFinal?(transcript)
        }
    }

    private func failSession(_ message: String) {
        bufferedAudio.removeAll(keepingCapacity: true)
        sessionState = .idle
        DispatchQueue.main.async { [weak self] in
            self?.onError?(message)
        }
    }

    private func notifyError(_ message: String) {
        DispatchQueue.main.async { [weak self] in
            self?.onError?(message)
        }
    }

    private func makeMultipartBody(boundary: String, audio: Data) -> Data {
        var body = Data()
        let lineBreak = "\r\n"

        func append(_ string: String) {
            body.append(Data(string.utf8))
        }

        append("--\(boundary)\(lineBreak)")
        append("Content-Disposition: form-data; name=\"model\"\(lineBreak)\(lineBreak)")
        append("whisper-1\(lineBreak)")

        let prompt = sessionOptions.hotwords
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
            .joined(separator: ", ")
        if !prompt.isEmpty {
            append("--\(boundary)\(lineBreak)")
            append("Content-Disposition: form-data; name=\"prompt\"\(lineBreak)\(lineBreak)")
            append("\(prompt)\(lineBreak)")
        }

        append("--\(boundary)\(lineBreak)")
        append("Content-Disposition: form-data; name=\"file\"; filename=\"speech.ogg\"\(lineBreak)")
        append("Content-Type: audio/ogg\(lineBreak)\(lineBreak)")
        body.append(audio)
        append(lineBreak)

        append("--\(boundary)--\(lineBreak)")
        return body
    }

    private func extractTranscript(from data: Data?) -> String {
        guard let data, !data.isEmpty else { return "" }
        if let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            if let text = object["text"] as? String {
                return text
            }
            if let error = object["error"] as? [String: Any] {
                return (error["message"] as? String) ?? ""
            }
        }
        return String(data: data, encoding: .utf8) ?? ""
    }

    private func extractErrorMessage(from data: Data?) -> String? {
        guard let data, !data.isEmpty else { return nil }
        if let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            if let error = object["error"] as? [String: Any] {
                return (error["message"] as? String) ?? (error["type"] as? String)
            }
            if let message = object["message"] as? String {
                return message
            }
        }
        return String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func responseSummary(_ response: HTTPURLResponse, data: Data?) -> String {
        let body = extractErrorMessage(from: data)
        if let body, !body.isEmpty {
            return "OpenAI transcription failed (\(response.statusCode)): \(body)"
        }
        return "OpenAI transcription failed (\(response.statusCode))"
    }
}
