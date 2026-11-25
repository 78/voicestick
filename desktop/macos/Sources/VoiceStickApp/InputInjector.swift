import AppKit
import ApplicationServices
import Foundation

final class InputInjector {
    struct PasteTarget {
        fileprivate let application: NSRunningApplication
        fileprivate let element: AXUIElement?
        fileprivate let identity: FieldIdentity?
        fileprivate let windowNumber: CGWindowID?
        fileprivate let fieldOrigin: CGPoint?

        var windowTarget: PasteTarget {
            PasteTarget(application: application, element: nil, identity: nil,
                        windowNumber: windowNumber, fieldOrigin: nil)
        }
    }

    fileprivate struct FieldIdentity {
        let window: AXUIElement
        let role: String
        let identifier: String

        func matches(_ other: FieldIdentity) -> Bool {
            role == other.role && identifier == other.identifier && CFEqual(window, other.window)
        }
    }

    func frontmostPasteApplication() -> NSRunningApplication? {
        guard let app = NSWorkspace.shared.frontmostApplication,
              app.processIdentifier != ProcessInfo.processInfo.processIdentifier
        else { return nil }
        return app
    }

    func capturePasteTarget() -> PasteTarget? {
        guard let app = frontmostPasteApplication() else { return nil }
        let element = textInput(in: app)
        return makeTarget(application: app, element: element)
    }

    // The user explicitly selected a new field. Some editors expose only their application.
    func selectedPasteTarget(in app: NSRunningApplication) -> PasteTarget? {
        guard frontmostPasteApplication()?.processIdentifier == app.processIdentifier else { return nil }
        return makeTarget(application: app, element: textInput(in: app))
    }

    private func makeTarget(application: NSRunningApplication, element: AXUIElement?) -> PasteTarget {
        PasteTarget(
            application: application, element: element, identity: element.flatMap(fieldIdentity),
            windowNumber: frontWindowNumber(for: application), fieldOrigin: element.flatMap(fieldOrigin)
        )
    }

    func restoreFocus(to target: PasteTarget, completion: @escaping (PasteTarget?) -> Void) {
        guard !target.application.isTerminated else {
            completion(nil)
            return
        }
        if !target.application.isActive {
            _ = target.application.activate(options: [])
        }
        // Give AppKit time to relinquish the confirmation panel's key focus.
        waitForFocus(target, remainingAttempts: target.element == nil ? 2 : 8,
                     canRefocus: true, completion: completion)
    }

    private func waitForFocus(
        _ target: PasteTarget,
        remainingAttempts: Int,
        canRefocus: Bool,
        completion: @escaping (PasteTarget?) -> Void
    ) {
        if target.element != nil && isFocused(target) {
            completion(target)
        } else if remainingAttempts == 0 || target.application.isTerminated {
            let destination = sameWindowDestination(for: target)
            NSLog("VoiceStick confirmation focus: same-window fallback=%@, app=%@",
                  destination == nil ? "unavailable" : "available",
                  target.application.bundleIdentifier ?? "unknown")
            completion(destination)
        } else {
            let shouldRefocus = canRefocus && target.application.isActive
            if shouldRefocus, let element = target.element {
                AXUIElementSetAttributeValue(element, kAXFocusedAttribute as CFString, kCFBooleanTrue)
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                self.waitForFocus(
                    target,
                    remainingAttempts: remainingAttempts - 1,
                    canRefocus: canRefocus && !shouldRefocus,
                    completion: completion
                )
            }
        }
    }

    private func sameWindowDestination(for target: PasteTarget) -> PasteTarget? {
        let app = target.application
        guard frontmostPasteApplication()?.processIdentifier == app.processIdentifier,
              let window = target.windowNumber, frontWindowNumber(for: app) == window
        else { return nil }

        if let current = textInput(in: app) {
            guard let original = target.element else {
                return makeTarget(application: app, element: current)
            }
            // If both controls are exposed, keep different fields distinct. Editors
            // without a stable identifier can recreate a field in the same position.
            guard target.identity == nil, fieldIdentity(current) == nil,
                  let before = target.fieldOrigin, let after = fieldOrigin(current),
                  abs(before.x - after.x) <= 8, abs(before.y - after.y) <= 8
            else { return nil }
            // An original element that's still live indicates a distinct field.
            guard attribute(kAXRoleAttribute, of: original) == nil else { return nil }
            return makeTarget(application: app, element: current)
        }
        // No conflicting text field is visible to Accessibility.
        return target.windowTarget
    }

    private func frontWindowNumber(for app: NSRunningApplication) -> CGWindowID? {
        guard let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
            as? [[String: Any]]
        else { return nil }
        return windows.first {
            ($0[kCGWindowOwnerPID as String] as? Int) == Int(app.processIdentifier)
                && ($0[kCGWindowLayer as String] as? Int) == 0
        }.flatMap { $0[kCGWindowNumber as String] as? CGWindowID }
    }

    private func fieldOrigin(_ element: AXUIElement) -> CGPoint? {
        guard let value = attribute(kAXPositionAttribute, of: element), CFGetTypeID(value) == AXValueGetTypeID()
        else { return nil }
        var point = CGPoint.zero
        return AXValueGetValue(value as! AXValue, .cgPoint, &point) ? point : nil
    }

    private func textInput(in app: NSRunningApplication) -> AXUIElement? {
        focusedElement(in: app).flatMap { isTextInput($0) ? $0 : nil }
    }

    private func focusedElement(in app: NSRunningApplication) -> AXUIElement? {
        if let element = elementAttribute(kAXFocusedUIElementAttribute, of: AXUIElementCreateSystemWide()) {
            var pid: pid_t = 0
            if AXUIElementGetPid(element, &pid) == .success && pid == app.processIdentifier {
                return element
            }
        }
        let application = AXUIElementCreateApplication(app.processIdentifier)
        return elementAttribute(kAXFocusedUIElementAttribute, of: application)
    }

    private func attribute(_ name: String, of element: AXUIElement) -> CFTypeRef? {
        var value: CFTypeRef?
        return AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success ? value : nil
    }

    private func elementAttribute(_ name: String, of element: AXUIElement) -> AXUIElement? {
        guard let value = attribute(name, of: element), CFGetTypeID(value) == AXUIElementGetTypeID()
        else { return nil }
        return (value as! AXUIElement)
    }

    private func isTextInput(_ element: AXUIElement) -> Bool {
        let role = attribute(kAXRoleAttribute, of: element) as? String
        return role == kAXTextFieldRole || role == kAXTextAreaRole || role == kAXComboBoxRole
    }

    private func fieldIdentity(_ element: AXUIElement) -> FieldIdentity? {
        guard isTextInput(element),
              let role = attribute(kAXRoleAttribute, of: element) as? String,
              let window = elementAttribute(kAXWindowAttribute, of: element),
              let identifier = (attribute(kAXIdentifierAttribute, of: element) as? String)
                ?? (attribute("AXDOMIdentifier", of: element) as? String),
              !identifier.isEmpty
        else { return nil }
        return FieldIdentity(window: window, role: role, identifier: identifier)
    }

    private func isFocused(_ target: PasteTarget) -> Bool {
        guard frontmostPasteApplication()?.processIdentifier == target.application.processIdentifier else { return false }
        guard let element = target.element else {
            return target.windowNumber.map { frontWindowNumber(for: target.application) == $0 } ?? true
        }
        guard let focused = textInput(in: target.application) else { return false }
        if CFEqual(focused, element) { return true }
        guard let expected = target.identity, let current = fieldIdentity(focused) else { return false }
        return expected.matches(current)
    }

    @discardableResult
    func paste(text: String, pressEnter: Bool, target: PasteTarget? = nil) -> Bool {
        guard !text.isEmpty, target.map(isFocused) ?? true else {
            NSLog("VoiceStick confirmation paste: destination focus unavailable")
            return false
        }
        guard target == nil || CGPreflightPostEventAccess() else {
            NSLog("VoiceStick confirmation paste: keyboard event access unavailable")
            return false
        }

        let pasteboard = NSPasteboard.general
        let previousItems = pasteboard.pasteboardItems?.map(PasteboardItemSnapshot.init)

        pasteboard.prepareForNewContents(with: .currentHostOnly)
        pasteboard.setString(text, forType: .string)
        let temporaryChangeCount = pasteboard.changeCount
        defer {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
                guard pasteboard.changeCount == temporaryChangeCount else { return }

                pasteboard.prepareForNewContents(with: .currentHostOnly)
                let restoredItems = previousItems?.map(\.pasteboardItem) ?? []
                if !restoredItems.isEmpty {
                    pasteboard.writeObjects(restoredItems)
                }
            }
        }
        if let target, target.element == nil, !isFocused(target) {
            NSLog("VoiceStick confirmation paste: original window changed before dispatch")
            return false
        }
        // Window-only targets use the same foreground event route as the auto-send countdown.
        let eventPID = target?.element == nil ? nil : target?.application.processIdentifier
        sendCommandV(to: eventPID)
        if let target {
            NSLog("VoiceStick confirmation paste: dispatched to %@, field=%@",
                  target.application.bundleIdentifier ?? "unknown", target.element == nil ? "window" : "matched")
        }

        if pressEnter {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
                guard target.map(self.isFocused) ?? true else {
                    NSLog("InputInjector auto_enter skipped: destination focus changed")
                    return
                }
                self.releaseCommandKey(to: eventPID)
                self.sendReturn(to: eventPID)
                NSLog("InputInjector auto_enter")
            }
        }

        return true
    }

    private func sendCommandV(to pid: pid_t?) {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let commandDown = CGEvent(keyboardEventSource: source, virtualKey: 0x37, keyDown: true)
        let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: true)
        let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x09, keyDown: false)
        let commandUp = CGEvent(keyboardEventSource: source, virtualKey: 0x37, keyDown: false)
        commandDown?.flags = .maskCommand
        keyDown?.flags = .maskCommand
        keyUp?.flags = .maskCommand
        commandUp?.flags = []
        post(commandDown, to: pid)
        post(keyDown, to: pid)
        post(keyUp, to: pid)
        post(commandUp, to: pid)
    }

    private func releaseCommandKey(to pid: pid_t?) {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let commandUp = CGEvent(keyboardEventSource: source, virtualKey: 0x37, keyDown: false)
        commandUp?.flags = []
        post(commandUp, to: pid)
    }

    private func sendReturn(to pid: pid_t?) {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0x24, keyDown: true)
        let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0x24, keyDown: false)
        keyDown?.flags = []
        keyUp?.flags = []
        post(keyDown, to: pid)
        post(keyUp, to: pid)
    }

    private func post(_ event: CGEvent?, to pid: pid_t?) {
        if let pid {
            event?.postToPid(pid)
        } else {
            event?.post(tap: .cghidEventTap)
        }
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
