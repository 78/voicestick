import Foundation

enum VoiceStickCloudApplyResult {
    case apiKey(String)
    case url(URL)
}

enum VoiceStickCloudAPIError: LocalizedError {
    case invalidCloudURL
    case invalidResponse
    case requestFailed(String)

    var errorDescription: String? {
        switch self {
        case .invalidCloudURL:
            return "Invalid VoiceStick Cloud URL."
        case .invalidResponse:
            return "VoiceStick Cloud returned an invalid response."
        case .requestFailed(let message):
            return message
        }
    }
}

enum VoiceStickCloudAPI {
    static func applyTrialAPIKey(
        cloudURL: String,
        deviceID: String?,
        completion: @escaping (Result<VoiceStickCloudApplyResult, Error>) -> Void
    ) {
        guard let url = applyURL(from: cloudURL) else {
            completion(.failure(VoiceStickCloudAPIError.invalidCloudURL))
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        if let deviceID, !deviceID.isEmpty {
            request.httpBody = try? JSONSerialization.data(withJSONObject: ["device_id": deviceID])
        } else {
            request.httpBody = Data("{}".utf8)
        }

        URLSession.shared.dataTask(with: request) { data, _, error in
            if let error {
                completion(.failure(VoiceStickCloudAPIError.requestFailed(error.localizedDescription)))
                return
            }
            guard
                let data,
                let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
            else {
                completion(.failure(VoiceStickCloudAPIError.invalidResponse))
                return
            }
            if let apiKey = object["api_key"] as? String, !apiKey.isEmpty {
                completion(.success(.apiKey(apiKey)))
                return
            }
            if let text = object["url"] as? String, let url = URL(string: text) {
                completion(.success(.url(url)))
                return
            }
            completion(.failure(VoiceStickCloudAPIError.invalidResponse))
        }.resume()
    }

    static func applyURL(from cloudURL: String) -> URL? {
        guard var components = URLComponents(string: cloudURL.trimmingCharacters(in: .whitespacesAndNewlines)) else {
            return nil
        }
        switch components.scheme?.lowercased() {
        case "ws":
            components.scheme = "http"
        case "wss":
            components.scheme = "https"
        case "http", "https":
            break
        default:
            return nil
        }
        components.path = "/voicestick/api-key/apply"
        components.query = nil
        components.fragment = nil
        return components.url
    }
}
