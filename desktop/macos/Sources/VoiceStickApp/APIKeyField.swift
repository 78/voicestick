import AppKit

final class APIKeyField: NSTextField {
    override class var cellClass: AnyClass? {
        get { APIKeyCell.self }
        set { super.cellClass = newValue }
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        usesSingleLineMode = false
        cell?.isScrollable = false
        cell?.wraps = true
        lineBreakMode = .byCharWrapping
        heightAnchor.constraint(equalToConstant: 72).isActive = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}

private final class APIKeyCell: NSTextFieldCell {
    private let keyEditor = APIKeyEditor(frame: .zero)

    override func fieldEditor(for controlView: NSView) -> NSTextView? {
        keyEditor.isFieldEditor = true
        return keyEditor
    }
}

private final class APIKeyEditor: NSTextView {
    override func readSelection(from pasteboard: NSPasteboard, type: NSPasteboard.PasteboardType) -> Bool {
        guard let pastedText = pasteboard.string(forType: .string) else { return false }

        // A key copied from a wrapped source is still one credential.
        let key = pastedText.filter { !$0.isWhitespace }
        let range = selectedRange()
        guard !key.isEmpty, shouldChangeText(in: range, replacementString: key) else { return false }

        replaceCharacters(in: range, with: key)
        setSelectedRange(NSRange(location: range.location + key.utf16.count, length: 0))
        didChangeText()
        return true
    }
}
