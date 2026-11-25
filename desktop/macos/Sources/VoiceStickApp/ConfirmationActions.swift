import AppKit

struct ConfirmationShortcuts: Equatable {
    static let none = Self()

    let cancel: String?
    let send: String?

    init(hardware: String? = nil, buttons: [String]? = nil) {
        switch hardware {
        case "magiclick_2p5", "stick_s3":
            cancel = "Shake"
        default:
            cancel = buttons?.contains("secondary") == true ? "Side" : nil
        }
        send = buttons?.contains("primary") == true ? "Main" : nil
    }
}

final class ConfirmationActions: NSView {
    static let height: CGFloat = 50
    static let sendFailureHeight: CGFloat = 68

    private let message = NSTextField(wrappingLabelWithString: "")
    private let cancelButton = NSButton(title: "Cancel", target: nil, action: nil)
    private let sendButton = NSButton(title: "Send", target: nil, action: nil)
    private lazy var cancelWidth = cancelButton.widthAnchor.constraint(equalToConstant: 96)
    private lazy var sendWidth = sendButton.widthAnchor.constraint(equalToConstant: 96)
    private var shortcuts = ConfirmationShortcuts.none
    private var shouldUseCurrentField = false
    var onSend: ((Bool) -> Void)?
    var onCancel: (() -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        message.font = .systemFont(ofSize: 14, weight: .medium)
        message.textColor = VoiceOverlayStyle.textColor
        message.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        message.maximumNumberOfLines = 1
        message.alignment = .right
        message.isHidden = true

        cancelButton.target = self
        cancelButton.action = #selector(cancel)
        cancelButton.keyEquivalent = "\u{1b}"
        cancelButton.keyEquivalentModifierMask = []
        sendButton.target = self
        sendButton.action = #selector(send)
        for button in [cancelButton, sendButton] {
            button.bezelStyle = .rounded
            button.controlSize = .large
            button.font = .systemFont(ofSize: 20, weight: .medium)
            button.heightAnchor.constraint(greaterThanOrEqualToConstant: 38).isActive = true
        }
        cancelWidth.isActive = true
        sendWidth.isActive = true

        let buttons = NSStackView(views: [cancelButton, sendButton])
        buttons.alignment = .centerY
        buttons.distribution = .fill
        buttons.spacing = 10
        buttons.translatesAutoresizingMaskIntoConstraints = false
        message.translatesAutoresizingMaskIntoConstraints = false
        addSubview(message)
        addSubview(buttons)
        NSLayoutConstraint.activate([
            buttons.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -24),
            buttons.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8),
            message.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 24),
            message.trailingAnchor.constraint(equalTo: buttons.trailingAnchor),
            message.bottomAnchor.constraint(equalTo: buttons.topAnchor, constant: -3),
        ])
    }

    required init?(coder: NSCoder) { nil }

    func updateDraft(_ text: String) {
        sendButton.isEnabled = !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    func reset(shortcuts: ConfirmationShortcuts) {
        message.stringValue = ""
        message.isHidden = true
        shouldUseCurrentField = false
        self.shortcuts = shortcuts
        updateButtonTitles()
    }

    func updateShortcuts(_ shortcuts: ConfirmationShortcuts) {
        guard self.shortcuts != shortcuts else { return }
        self.shortcuts = shortcuts
        updateButtonTitles()
    }

    func showSendFailure() {
        message.stringValue = "Click an input field, then Paste Here."
        message.isHidden = false
        shouldUseCurrentField = true
        updateButtonTitles()
    }

    private func updateButtonTitles() {
        setTitle("Cancel", shortcut: shortcuts.cancel, for: cancelButton)
        setTitle(shouldUseCurrentField ? "Paste Here" : "Send",
                 shortcut: shouldUseCurrentField ? nil : shortcuts.send, for: sendButton)
        cancelWidth.constant = buttonWidth(cancelButton)
        sendWidth.constant = buttonWidth(sendButton)
    }

    private func buttonWidth(_ button: NSButton) -> CGFloat {
        max(96, ceil(button.attributedTitle.size().width) + 20)
    }

    private func setTitle(_ title: String, shortcut: String?, for button: NSButton) {
        guard let shortcut else {
            button.title = title
            return
        }
        let label = NSMutableAttributedString(
            string: title, attributes: [.font: NSFont.systemFont(ofSize: 20, weight: .medium)]
        )
        label.append(NSAttributedString(string: " [\(shortcut)]", attributes: [
            .font: NSFont.systemFont(ofSize: 14, weight: .medium),
            .foregroundColor: NSColor.secondaryLabelColor,
        ]))
        button.attributedTitle = label
    }

    @objc private func send() { onSend?(shouldUseCurrentField) }
    @objc private func cancel() { onCancel?() }
}
