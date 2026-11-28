import AppKit

final class FirmwareRequirementWindowController: NSWindowController {
    let deviceID: String
    var onUpdate: (() -> Void)?

    private let message = NSTextField(wrappingLabelWithString: "")
    private let availability = NSTextField(wrappingLabelWithString: "")
    private let updateButton = NSButton(title: "Update Firmware", target: nil, action: nil)
    private let dismissButton = NSButton(title: "OK", target: nil, action: nil)
    private var usesWebFlasher = false

    init(deviceID: String) {
        self.deviceID = deviceID
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 420, height: 205),
                              styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "Firmware Update Required"
        window.isReleasedWhenClosed = false
        super.init(window: window)
        buildContent()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func show() {
        window?.center()
        showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func update(info: DeviceFirmwareInfo, supportsFirmwareUpdate: Bool) {
        let current = info.currentVersion.map { " is running firmware \($0)" } ?? " needs a firmware update"
        message.stringValue = "VS-\(deviceID)\(current). Update its firmware to use voice input and games."

        let latest = info.updateAvailable ? info.latestVersion : nil
        if let latest {
            availability.stringValue = supportsFirmwareUpdate
                ? "Firmware \(latest) is ready to install."
                : "Firmware \(latest) is available. Use the VoiceStick web flasher to install it."
        } else if info.isChecking {
            availability.stringValue = "Checking for compatible firmware…"
        } else if let errorMessage = info.errorMessage {
            availability.stringValue = errorMessage
        } else {
            availability.stringValue = "No compatible firmware is available from the update server yet."
        }

        let hasUpdate = latest != nil
        usesWebFlasher = hasUpdate && !supportsFirmwareUpdate
        updateButton.title = usesWebFlasher ? "Open Web Flasher" : "Update Firmware"
        updateButton.isHidden = !hasUpdate
        updateButton.isEnabled = hasUpdate
        updateButton.keyEquivalent = hasUpdate ? "\r" : ""
        dismissButton.title = hasUpdate ? "Later" : "OK"
        dismissButton.keyEquivalent = hasUpdate ? "\u{1b}" : "\r"
    }

    private func buildContent() {
        guard let content = window?.contentView else { return }
        availability.textColor = .secondaryLabelColor
        updateButton.target = self
        updateButton.action = #selector(installUpdate)
        dismissButton.target = self
        dismissButton.action = #selector(dismiss)

        let buttons = NSStackView(views: [dismissButton, updateButton])
        buttons.spacing = 8
        buttons.detachesHiddenViews = true
        let stack = NSStackView(views: [message, availability, buttons])
        stack.orientation = .vertical
        stack.alignment = .trailing
        stack.spacing = 12
        stack.setCustomSpacing(20, after: availability)
        stack.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(stack)

        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: content.topAnchor, constant: 22),
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 24),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -24),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: content.bottomAnchor, constant: -20),
            message.widthAnchor.constraint(equalTo: stack.widthAnchor),
            availability.widthAnchor.constraint(equalTo: stack.widthAnchor),
        ])
    }

    @objc private func installUpdate() {
        if usesWebFlasher, let url = URL(string: "#flash", relativeTo: AppConfig.websiteURL)?.absoluteURL {
            NSWorkspace.shared.open(url)
            close()
        } else {
            onUpdate?()
        }
    }

    @objc private func dismiss() {
        close()
    }
}
