import AppKit
import Darwin

let lockDirectory = AppConfig.configDirectory
do {
    try FileManager.default.createDirectory(at: lockDirectory, withIntermediateDirectories: true)
} catch {
    NSLog("Could not create the VoiceStick configuration directory: \(error.localizedDescription)")
    exit(EXIT_FAILURE)
}

let lockPath = lockDirectory.appendingPathComponent("instance.lock").path
let lockDescriptor = open(
    lockPath,
    O_WRONLY | O_CREAT | O_EXLOCK | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC,
    S_IRUSR | S_IWUSR
)
if lockDescriptor == -1 {
    let lockError = errno
    if lockError == EWOULDBLOCK {
        NSLog("Another VoiceStick instance is already running")
        exit(EXIT_SUCCESS)
    }
    NSLog("Could not acquire the VoiceStick instance lock: \(String(cString: strerror(lockError)))")
    exit(EXIT_FAILURE)
}
defer { close(lockDescriptor) }

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

let delegate = AppDelegate()
app.delegate = delegate
app.run()
