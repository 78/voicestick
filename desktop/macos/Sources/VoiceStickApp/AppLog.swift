import Foundation

enum AppLog {
    static var isDebugEnabled: Bool {
        UserDefaults.standard.bool(forKey: "VoiceStickDebugLogging") ||
            ProcessInfo.processInfo.environment["VOICESTICK_DEBUG_LOGS"] == "1"
    }

    static func debug(_ message: @autoclosure () -> String) {
        guard isDebugEnabled else { return }
        NSLog("%@", message())
    }

    static func error(_ message: @autoclosure () -> String) {
        NSLog("%@", message())
    }
}
