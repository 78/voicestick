import Foundation

final class LLMTranslationClient {
    enum TranslationError: LocalizedError {
        case missingAPIKey
        case invalidURL
        case invalidResponse
        case serverError(Int, String)

        var errorDescription: String? {
            switch self {
            case .missingAPIKey:
                return "Missing LLM API key"
            case .invalidURL:
                return "Invalid LLM base URL"
            case .invalidResponse:
                return "Invalid LLM translation response"
            case .serverError(let statusCode, let message):
                return "LLM translation failed (\(statusCode)): \(message)"
            }
        }
    }

    private let config: AppConfig
    private let session: URLSession

    init(config: AppConfig, session: URLSession = .shared) {
        self.config = config
        self.session = session
    }

    func translate(
        _ text: String,
        targetLanguage: String,
        hotwords: [String],
        completion: @escaping (Result<String, Error>) -> Void
    ) {
        let apiKey = config.llmAPIKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !apiKey.isEmpty else {
            completion(.failure(TranslationError.missingAPIKey))
            return
        }

        guard let url = chatCompletionsURL() else {
            completion(.failure(TranslationError.invalidURL))
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 8
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")

        let systemPrompt = Self.systemPrompt(targetLanguage: targetLanguage, hotwords: hotwords)
        let payload: [String: Any] = [
            "model": config.llmModel,
            "temperature": 0,
            "messages": [
                ["role": "system", "content": systemPrompt],
                ["role": "user", "content": text]
            ]
        ]

        do {
            request.httpBody = try JSONSerialization.data(withJSONObject: payload)
        } catch {
            completion(.failure(error))
            return
        }

        session.dataTask(with: request) { data, response, error in
            if let error {
                completion(.failure(error))
                return
            }
            if let httpResponse = response as? HTTPURLResponse,
               !(200...299).contains(httpResponse.statusCode) {
                completion(.failure(TranslationError.serverError(
                    httpResponse.statusCode,
                    Self.extractErrorMessage(from: data) ?? HTTPURLResponse.localizedString(forStatusCode: httpResponse.statusCode)
                )))
                return
            }
            guard
                let data,
                let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                let choices = object["choices"] as? [[String: Any]],
                let message = choices.first?["message"] as? [String: Any],
                let content = message["content"] as? String
            else {
                completion(.failure(TranslationError.invalidResponse))
                return
            }
            completion(.success(content.trimmingCharacters(in: .whitespacesAndNewlines)))
        }.resume()
    }

    private func chatCompletionsURL() -> URL? {
        let base = config.llmBaseURL.trimmingCharacters(in: CharacterSet(charactersIn: "/ \n\r\t"))
        guard !base.isEmpty else { return nil }
        if base.hasSuffix("/chat/completions") {
            return URL(string: base)
        }
        return URL(string: "\(base)/chat/completions")
    }

    private static func systemPrompt(targetLanguage: String, hotwords: [String]) -> String {
        var prompt = """
        You are a real-time speech translator.
        Translate the user's text into \(targetLanguage).
        Detect the source language automatically.
        Return only the translated text, with no explanations, quotes, prefixes, alternatives, or markdown.
        The text may come from live speech recognition and may contain minor recognition errors; infer the intended meaning when it is clear.
        """

        let terms = hotwords
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        if !terms.isEmpty {
            prompt += "\n\nImportant terms that may appear:\n"
            prompt += terms.map { "- \($0)" }.joined(separator: "\n")
        }
        return prompt
    }

    private static func extractErrorMessage(from data: Data?) -> String? {
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
}
