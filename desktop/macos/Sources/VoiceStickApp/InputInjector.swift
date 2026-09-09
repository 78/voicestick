import AppKit
import ApplicationServices
import Foundation

final class InputInjector {
    func paste(text: String, pressEnter: Bool) -> Bool {
        guard !text.isEmpty else { return true }
        guard isAccessibilityTrusted(promptIfNeeded: true) else { return false }

        if insertWithAccessibility(text) {
            if pressEnter {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
                    self.sendReturn()
                }
            }
            return true
        }

        let pasteboard = NSPasteboard.general
        let previousItems = pasteboard.pasteboardItems?.map(PasteboardItemSnapshot.init)

        pasteboard.prepareForNewContents(with: .currentHostOnly)
        pasteboard.setString(text, forType: .string)
        let temporaryChangeCount = pasteboard.changeCount
        sendCommandV()

        if pressEnter {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
                self.releaseCommandKey()
                self.sendReturn()
            }
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
            guard pasteboard.changeCount == temporaryChangeCount else { return }

            pasteboard.prepareForNewContents(with: .currentHostOnly)
            let restoredItems = previousItems?.map(\.pasteboardItem) ?? []
            if !restoredItems.isEmpty {
                pasteboard.writeObjects(restoredItems)
            }
        }

        return true
    }

    private func isAccessibilityTrusted(promptIfNeeded: Bool) -> Bool {
        let options = [
            kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: promptIfNeeded
        ] as CFDictionary
        return AXIsProcessTrustedWithOptions(options)
    }

    private func insertWithAccessibility(_ text: String) -> Bool {
        let systemWideElement = AXUIElementCreateSystemWide()
        var focusedValue: CFTypeRef?
        let focusedResult = AXUIElementCopyAttributeValue(
            systemWideElement,
            kAXFocusedUIElementAttribute as CFString,
            &focusedValue
        )
        guard focusedResult == .success, let focusedElement = focusedValue else {
            return false
        }

        let element = unsafeBitCast(focusedElement, to: AXUIElement.self)
        if AXUIElementSetAttributeValue(element, kAXSelectedTextAttribute as CFString, text as CFTypeRef) == .success {
            return true
        }

        var valueRef: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, kAXValueAttribute as CFString, &valueRef) == .success,
              let currentText = valueRef as? String
        else {
            return false
        }

        var selectedRangeRef: CFTypeRef?
        guard AXUIElementCopyAttributeValue(
            element,
            kAXSelectedTextRangeAttribute as CFString,
            &selectedRangeRef
        ) == .success,
           let selectedRangeValue = selectedRangeRef,
           CFGetTypeID(selectedRangeValue) == AXValueGetTypeID()
        else {
            return false
        }

        var selectedRange = CFRange(location: 0, length: 0)
        let axValue = unsafeBitCast(selectedRangeValue, to: AXValue.self)
        AXValueGetValue(axValue, .cfRange, &selectedRange)

        let currentNSString = currentText as NSString
        guard selectedRange.location >= 0,
              selectedRange.length >= 0,
              selectedRange.location + selectedRange.length <= currentNSString.length
        else {
            return false
        }

        let updatedText = currentNSString.replacingCharacters(in: NSRange(location: selectedRange.location, length: selectedRange.length), with: text)
        guard AXUIElementSetAttributeValue(element, kAXValueAttribute as CFString, updatedText as CFTypeRef) == .success else {
            return false
        }

        var caretRange = CFRange(location: selectedRange.location + (text as NSString).length, length: 0)
        if let caretValue = AXValueCreate(.cfRange, &caretRange) {
            _ = AXUIElementSetAttributeValue(element, kAXSelectedTextRangeAttribute as CFString, caretValue)
        }
        return true
    }

    private func sendCommandV() {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let commandDown = CGEvent(keyboardEventSource: source, virtualKey: 0x37, keyDown: true)
        let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: true)
        let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: false)
        let commandUp = CGEvent(keyboardEventSource: source, virtualKey: 0x37, keyDown: false)
        commandDown?.flags = .maskCommand
        keyDown?.flags = .maskCommand
        keyUp?.flags = .maskCommand
        commandUp?.flags = []
        commandDown?.post(tap: .cghidEventTap)
        keyDown?.post(tap: .cghidEventTap)
        keyUp?.post(tap: .cghidEventTap)
        commandUp?.post(tap: .cghidEventTap)
    }

    private func releaseCommandKey() {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let commandUp = CGEvent(keyboardEventSource: source, virtualKey: 0x37, keyDown: false)
        commandUp?.flags = []
        commandUp?.post(tap: .cghidEventTap)
    }

    private func sendReturn() {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x24, keyDown: true)
        let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x24, keyDown: false)
        keyDown?.flags = []
        keyUp?.flags = []
        keyDown?.post(tap: .cghidEventTap)
        keyUp?.post(tap: .cghidEventTap)
    }
}

private struct PasteboardItemSnapshot {
    private let contents: [(type: NSPasteboard.PasteboardType, data: Data)]

    init(item: NSPasteboardItem) {
        contents = item.types.compactMap { type in
            guard let data = item.data(forType: type) else { return nil }
            return (type, data)
        }
    }

    var pasteboardItem: NSPasteboardItem {
        let item = NSPasteboardItem()
        for content in contents {
            item.setData(content.data, forType: content.type)
        }
        return item
    }
}
