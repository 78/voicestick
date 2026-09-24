import Foundation

final class DebugAudioRecorder {
    private let enabled: Bool
    private let directory: URL
    private var currentDeviceID: String?
    private var currentSessionID: UInt32?
    private var currentStartedAt: Date?
    private var currentAudio = Data()

    init(enabled: Bool, directory: URL) {
        self.enabled = enabled
        self.directory = directory
    }

    func start(deviceID: String?, sessionID: UInt32?) {
        guard enabled else { return }
        currentDeviceID = deviceID
        currentSessionID = sessionID
        currentStartedAt = Date()
        currentAudio.removeAll(keepingCapacity: true)
    }

    func append(_ data: Data) {
        guard enabled, !data.isEmpty else { return }
        currentAudio.append(data)
    }

    func finish() {
        guard enabled, !currentAudio.isEmpty else {
            reset()
            return
        }

        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let fileURL = directory.appendingPathComponent(fileName(), isDirectory: false)
            try currentAudio.write(to: fileURL, options: .atomic)
            AppLog.debug("Debug audio saved: \(fileURL.path)")
        } catch {
            AppLog.error("Debug audio save failed: \(error.localizedDescription)")
        }

        reset()
    }

    func discard() {
        reset()
    }

    private func reset() {
        currentDeviceID = nil
        currentSessionID = nil
        currentStartedAt = nil
        currentAudio.removeAll(keepingCapacity: false)
    }

    private func fileName() -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let timestamp = formatter.string(from: currentStartedAt ?? Date())

        let device = currentDeviceID.map { "VS-\($0)" } ?? "unknown-device"
        let session = currentSessionID.map(String.init) ?? "unknown"
        return "\(timestamp)-\(device)-session-\(session).ogg"
    }
}
