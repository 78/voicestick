import Foundation

/// A file per match. Only the game process writes here, while a match is running.
final class MatchLog {
    static var directory: URL {
        FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/VoiceStick/Tennis", isDirectory: true)
    }

    private let output: FileHandle
    private let queue = DispatchQueue(label: "VoiceStickTennis.match-log", qos: .utility)
    private let started = ProcessInfo.processInfo.systemUptime
    private var failed = false

    init() throws {
        let fm = FileManager.default
        try fm.createDirectory(at: Self.directory, withIntermediateDirectories: true)
        let formatter = ISO8601DateFormatter()
        let date = formatter.string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let url = Self.directory.appendingPathComponent("match-\(date)-\(UUID().uuidString).jsonl")
        guard fm.createFile(atPath: url.path, contents: nil, attributes: [.posixPermissions: 0o600]) else {
            throw CocoaError(.fileWriteUnknown)
        }
        output = try FileHandle(forWritingTo: url)
        let logs = (try? fm.contentsOfDirectory(at: Self.directory, includingPropertiesForKeys: nil)) ?? []
        let older = logs.filter { $0 != url && $0.lastPathComponent.hasPrefix("match-") && $0.pathExtension == "jsonl" }
        for expired in older.sorted(by: { $0.lastPathComponent > $1.lastPathComponent }).dropFirst(11) {
            try? fm.removeItem(at: expired)
        }
    }

    func write(_ event: String, _ fields: [String: Any] = [:]) {
        let elapsed = ProcessInfo.processInfo.systemUptime - started
        queue.async {
            guard !self.failed else { return }
            var row = fields
            row["t"] = elapsed
            row["event"] = event
            do {
                var data = try JSONSerialization.data(withJSONObject: row)
                data.append(10)
                try self.output.write(contentsOf: data)
            } catch {
                self.failed = true
                NSLog("Tennis match log write failed: %@", error.localizedDescription)
            }
        }
    }

    func close() { queue.sync { try? output.close() } }
}
