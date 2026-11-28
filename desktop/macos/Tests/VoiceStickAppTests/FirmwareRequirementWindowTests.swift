import AppKit
import XCTest
@testable import VoiceStickApp

final class FirmwareRequirementWindowTests: XCTestCase {
    func testVisibleRequirementGainsAnUpdateActionAfterChecking() throws {
        _ = NSApplication.shared
        let controller = FirmwareRequirementWindowController(deviceID: "42AB")
        let content = try XCTUnwrap(controller.window?.contentView)
        var info = DeviceFirmwareInfo(currentVersion: "0.4.6", requiresInputFirmware: true, isChecking: true)

        controller.update(info: info, supportsFirmwareUpdate: true)
        XCTAssertEqual(controller.window?.title, "Firmware Update Required")
        XCTAssertTrue(visibleLabels(in: content).contains { $0.contains("voice input and games") })
        XCTAssertTrue(visibleLabels(in: content).contains { $0.hasPrefix("Checking") })
        XCTAssertEqual(visibleButtons(in: content).map(\.title), ["OK"])

        info.isChecking = false
        info.latestVersion = "0.5.0"
        info.updateAvailable = true
        controller.update(info: info, supportsFirmwareUpdate: true)
        let update = try XCTUnwrap(visibleButtons(in: content).first { $0.title == "Update Firmware" })
        XCTAssertTrue(update.isEnabled)
        XCTAssertTrue(visibleLabels(in: content).contains { $0.contains("0.5.0 is ready") })
        var requested = false
        controller.onUpdate = { requested = true }
        XCTAssertTrue(NSApp.sendAction(try XCTUnwrap(update.action), to: update.target, from: update))
        XCTAssertTrue(requested)
    }

    func testIncompatibleReleaseGivesNoUpdateAction() throws {
        _ = NSApplication.shared
        let controller = FirmwareRequirementWindowController(deviceID: "42AB")
        let content = try XCTUnwrap(controller.window?.contentView)
        let info = DeviceFirmwareInfo(currentVersion: "0.4.4", latestVersion: "0.4.6", requiresInputFirmware: true)

        controller.update(info: info, supportsFirmwareUpdate: true)
        XCTAssertTrue(visibleLabels(in: content).contains { $0.contains("No compatible firmware") })
        XCTAssertEqual(visibleButtons(in: content).map(\.title), ["OK"])
    }

    func testCompatibleReleaseCanOpenTheWebFlasherWithoutBLEUpdate() throws {
        _ = NSApplication.shared
        let controller = FirmwareRequirementWindowController(deviceID: "42AB")
        let content = try XCTUnwrap(controller.window?.contentView)
        let info = DeviceFirmwareInfo(currentVersion: "0.4.6", latestVersion: "0.5.0",
                                      requiresInputFirmware: true, updateAvailable: true)

        controller.update(info: info, supportsFirmwareUpdate: false)
        XCTAssertTrue(visibleButtons(in: content).contains { $0.title == "Open Web Flasher" && $0.isEnabled })
        XCTAssertTrue(visibleLabels(in: content).contains { $0.contains("VoiceStick web flasher") })
    }

    private func visibleLabels(in view: NSView) -> [String] {
        visibleViews(in: view).compactMap { ($0 as? NSTextField)?.stringValue }
    }

    private func visibleButtons(in view: NSView) -> [NSButton] {
        visibleViews(in: view).compactMap { $0 as? NSButton }
    }

    private func visibleViews(in view: NSView) -> [NSView] {
        guard !view.isHidden else { return [] }
        return [view] + view.subviews.flatMap(visibleViews(in:))
    }
}
