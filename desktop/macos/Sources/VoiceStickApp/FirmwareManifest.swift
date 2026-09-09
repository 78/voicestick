import CryptoKit
import Foundation

struct FirmwareManifest: Decodable {
    let hardware: String
    let version: String
    let otaURL: URL
    let otaSHA256: String
    let otaSize: Int
    let mergedURL: URL?
    let mergedSHA256: String?
    let mergedSize: Int?

    enum CodingKeys: String, CodingKey {
        case hardware
        case version
        case otaURL = "ota_url"
        case otaSHA256 = "ota_sha256"
        case otaSize = "ota_size"
        case mergedURL = "merged_url"
        case mergedSHA256 = "merged_sha256"
        case mergedSize = "merged_size"
    }
}

struct DeviceFirmwareInfo {
    var hardware: String?
    var currentVersion: String?
    var latestVersion: String?
    var updateAvailable = false
    var isChecking = false
    var errorMessage: String?
}

enum FirmwareVersion {
    static func isVersion(_ current: String, olderThan latest: String) -> Bool {
        guard let current = ParsedVersion(current), let latest = ParsedVersion(latest) else {
            return false
        }
        return current < latest
    }

    private struct ParsedVersion: Comparable {
        let numbers: [Int]
        let suffix: String?

        init?(_ text: String) {
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty else { return nil }
            let parts = trimmed.split(separator: "-", maxSplits: 1).map(String.init)
            let numberParts = parts[0].split(separator: ".").map(String.init)
            guard !numberParts.isEmpty else { return nil }
            var parsedNumbers: [Int] = []
            for part in numberParts {
                guard let value = Int(part) else { return nil }
                parsedNumbers.append(value)
            }
            numbers = parsedNumbers
            suffix = parts.count > 1 ? parts[1] : nil
        }

        static func < (lhs: ParsedVersion, rhs: ParsedVersion) -> Bool {
            let count = max(lhs.numbers.count, rhs.numbers.count)
            for index in 0..<count {
                let left = index < lhs.numbers.count ? lhs.numbers[index] : 0
                let right = index < rhs.numbers.count ? rhs.numbers[index] : 0
                if left != right {
                    return left < right
                }
            }

            switch (lhs.suffix, rhs.suffix) {
            case (.some, nil):
                return true
            case (nil, .some):
                return false
            case let (.some(left), .some(right)):
                return left.localizedStandardCompare(right) == .orderedAscending
            case (nil, nil):
                return false
            }
        }
    }
}

final class FirmwareManifestClient {
    enum FirmwareManifestError: LocalizedError {
        case invalidResponse
        case checksumMismatch
        case sizeMismatch

        var errorDescription: String? {
            switch self {
            case .invalidResponse:
                return "Firmware update server returned an invalid response."
            case .checksumMismatch:
                return "Firmware checksum did not match the manifest."
            case .sizeMismatch:
                return "Firmware size did not match the manifest."
            }
        }
    }

    private let manifestURL: URL

    init(manifestURL: URL = AppConfig.firmwareManifestURL) {
        self.manifestURL = manifestURL
    }

    func fetchManifest(completion: @escaping (Result<FirmwareManifest, Error>) -> Void) {
        var request = URLRequest(
            url: manifestURL,
            cachePolicy: .reloadIgnoringLocalAndRemoteCacheData
        )
        request.setValue("no-cache", forHTTPHeaderField: "Cache-Control")
        request.setValue("no-cache", forHTTPHeaderField: "Pragma")

        URLSession.shared.dataTask(with: request) { data, response, error in
            if let error {
                completion(.failure(error))
                return
            }
            guard let data,
                  let httpResponse = response as? HTTPURLResponse,
                  (200..<300).contains(httpResponse.statusCode) else {
                completion(.failure(FirmwareManifestError.invalidResponse))
                return
            }

            do {
                let manifest = try JSONDecoder().decode(FirmwareManifest.self, from: data)
                completion(.success(manifest))
            } catch {
                completion(.failure(error))
            }
        }.resume()
    }

    func downloadOTA(from manifest: FirmwareManifest, completion: @escaping (Result<Data, Error>) -> Void) {
        URLSession.shared.dataTask(with: manifest.otaURL) { data, response, error in
            if let error {
                completion(.failure(error))
                return
            }
            guard let data,
                  let httpResponse = response as? HTTPURLResponse,
                  (200..<300).contains(httpResponse.statusCode) else {
                completion(.failure(FirmwareManifestError.invalidResponse))
                return
            }
            guard data.count == manifest.otaSize else {
                completion(.failure(FirmwareManifestError.sizeMismatch))
                return
            }
            let digest = SHA256.hash(data: data)
                .map { String(format: "%02x", $0) }
                .joined()
            guard digest.lowercased() == manifest.otaSHA256.lowercased() else {
                completion(.failure(FirmwareManifestError.checksumMismatch))
                return
            }
            completion(.success(data))
        }.resume()
    }
}
