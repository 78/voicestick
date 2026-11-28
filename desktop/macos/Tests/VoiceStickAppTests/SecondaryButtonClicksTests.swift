import Foundation
import XCTest
@testable import VoiceStickApp

final class SecondaryButtonClicksTests: XCTestCase {
    func testJapaneseTargetSurvivesSavingAndReloadingOriginalMode() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let configURL = directory.appendingPathComponent("config.toml")
        let deviceID = "42AB"
        var config = AppConfig.defaults
        config.pairedDeviceIDs = [deviceID]
        var profile = config.outputProfile(for: deviceID)
        profile.translationTarget = "ja"
        profile.toggleTranslation()
        config.setDeviceOutputProfile(profile, for: deviceID)
        try config.save(to: configURL)

        config = AppConfig.load(from: configURL)
        profile = config.outputProfile(for: deviceID)
        XCTAssertEqual(profile.transform, .translate)
        profile.toggleTranslation()
        config.setDeviceOutputProfile(profile, for: deviceID)
        try config.save(to: configURL)

        config = AppConfig.load(from: configURL)
        profile = config.outputProfile(for: deviceID)
        XCTAssertEqual(profile.transform, .original)
        XCTAssertEqual(profile.translationTarget, "ja")
        profile.toggleTranslation()
        XCTAssertEqual(profile.transform, .translate)
        XCTAssertEqual(profile.translationTarget, "ja")
        XCTAssertEqual(config.defaultOutputProfile.translationTarget, "en")
    }

    func testDoubleClickUsesFirstReleaseToSecondPress() {
        let clicks = SecondaryButtonClicks()
        let deviceID = UUID()
        var profile = OutputProfile(target: .subtitle, transform: .original, translationTarget: "ja")
        let single = expectation(description: "no single click for a double click")
        single.isInverted = true
        let release: (UInt64, UInt64) -> Void = { timestamp, duration in
            clicks.release(from: deviceID, at: timestamp, heldFor: duration, canDoubleClick: { true },
                           onSingleClick: { single.fulfill() },
                           onDoubleClick: { profile.toggleTranslation() })
        }

        for (offset, mode) in [(UInt64(0), TextTransform.translate), (1_000, .original)] {
            release(offset + 130, 30)
            clicks.press(from: deviceID, at: offset + 310, canDoubleClick: true)
            release(offset + 350, 40)
            XCTAssertEqual(profile.transform, mode)
            XCTAssertEqual(profile.translationTarget, "ja")
        }
        wait(for: [single], timeout: 0.25)
    }

    func testPressOutsideWindowDoesNotTriggerDoubleClick() {
        let clicks = SecondaryButtonClicks()
        let deviceID = UUID()
        let singles = expectation(description: "both single clicks fire")
        singles.expectedFulfillmentCount = 2
        let release: (UInt64) -> Void = { timestamp in
            clicks.release(from: deviceID, at: timestamp, heldFor: 40, canDoubleClick: { true },
                           onSingleClick: { singles.fulfill() },
                           onDoubleClick: { XCTFail("The gap exceeds 180 ms") })
        }

        release(100)
        clicks.press(from: deviceID, at: 281, canDoubleClick: true)
        release(321)
        wait(for: [singles], timeout: 1)
    }

    func testBusyClickActsImmediately() {
        let clicks = SecondaryButtonClicks()
        var cancelled = false
        clicks.release(from: UUID(), at: 130, heldFor: 30, canDoubleClick: { false },
                       onSingleClick: { cancelled = true },
                       onDoubleClick: { XCTFail("Busy clicks cannot form a double click") })
        XCTAssertTrue(cancelled)
    }

    func testDisconnectClearsBothClickPhases() {
        let clicks = SecondaryButtonClicks()
        let firstDevice = UUID()
        let secondDevice = UUID()
        let delayed = expectation(description: "disconnected clicks do not fire")
        delayed.isInverted = true
        func release(_ device: UUID, at timestamp: UInt64) {
            clicks.release(from: device, at: timestamp, heldFor: 40, canDoubleClick: { true },
                           onSingleClick: { delayed.fulfill() },
                           onDoubleClick: { XCTFail("Different connections cannot form a double click") })
        }
        release(firstDevice, at: 100)
        release(secondDevice, at: 100)
        clicks.press(from: secondDevice, at: 200, canDoubleClick: true)
        clicks.removeDisconnected { _ in false }

        wait(for: [delayed], timeout: 0.25)
    }
}
