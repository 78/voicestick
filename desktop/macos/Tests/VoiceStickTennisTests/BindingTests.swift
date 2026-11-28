import AppKit
import MotionInput
import XCTest

final class BindingTests: XCTestCase {
    let devices = ["VS-A001", "VS-A002", "VS-A003"].map { MotionDevice(id: UUID(), name: $0) }

    func testChooseTwoOfThreeAtomicallyAndReconnectInDifferentOrder() {
        var bindings = MotionBindings()
        let hands: [MotionRole] = [.leftHand, .rightHand]
        XCTAssertTrue(bindings.apply(InputRequest(roles: hands), devices: devices))
        XCTAssertTrue(bindings.assignments.isEmpty)
        let mapping = ["leftHand": devices[2].id, "rightHand": devices[0].id]
        XCTAssertTrue(bindings.apply(InputRequest(roles: hands, assignments: mapping), devices: devices))
        XCTAssertNil(bindings.role(for: devices[1].id))
        for invalid in [["leftHand": devices[1].id], ["leftHand": devices[0].id, "rightHand": devices[0].id],
                        ["leftHand": UUID(), "rightHand": devices[0].id]] {
            XCTAssertFalse(bindings.apply(InputRequest(roles: hands, assignments: invalid), devices: devices))
            XCTAssertEqual(bindings.assignments, mapping)
        }
        bindings.discover([devices[0], devices[1]])
        XCTAssertEqual(bindings.assignments, mapping)
        bindings.discover(devices.reversed())
        XCTAssertEqual(bindings.role(for: devices[2].id), .leftHand)
        XCTAssertEqual(bindings.role(for: devices[0].id), .rightHand)
        let swap = ["leftHand": devices[0].id, "rightHand": devices[2].id]
        XCTAssertTrue(bindings.apply(InputRequest(roles: hands, assignments: swap), devices: devices))
        XCTAssertEqual(bindings.assignments, swap)
    }

    func testOneRacketAndExplicitKeyboardChoice() {
        var bindings = MotionBindings()
        _ = bindings.apply(InputRequest(roles: [.racket]), devices: [])
        bindings.discover([devices[0]])
        XCTAssertEqual(bindings.role(for: devices[0].id), .racket)
        bindings.discover(devices)
        XCTAssertEqual(bindings.assignments.count, 1)
        _ = bindings.apply(InputRequest(roles: [.racket], assignments: [:]), devices: devices)
        bindings.discover([devices[0]])
        XCTAssertTrue(bindings.assignments.isEmpty)
        var crowded = MotionBindings()
        _ = crowded.apply(InputRequest(roles: [.racket]), devices: devices)
        crowded.discover([devices[2]])
        XCTAssertTrue(crowded.assignments.isEmpty)
    }

    @MainActor
    func testPhysicalIdentificationFillsEachRequestedHand() throws {
        _ = NSApplication.shared
        let picker = MotionPicker(roles: [.leftHand, .rightHand])
        var roster = InputRoster()
        roster.devices = devices
        picker.update(roster)
        var requests: [InputRequest] = []
        picker.onRequest = { requests.append($0) }
        func arm(_ index: Int) throws {
            let menu = try XCTUnwrap(picker.arrangedSubviews[index] as? NSPopUpButton)
            menu.selectItem(withTag: 1)
            menu.sendAction(menu.action, to: menu.target)
        }
        picker.identify(devices[1].id) // Ordinary presses cannot assign an unarmed picker.
        try arm(0)
        picker.identify(devices[2].id)
        XCTAssertTrue(requests.isEmpty)
        try arm(1)
        picker.identify(devices[0].id)
        XCTAssertEqual(requests.count, 1)
        XCTAssertEqual(requests.first?.assignments, ["leftHand": devices[2].id, "rightHand": devices[0].id])
    }

    func testMotionSourcesSurviveTransport() throws {
        var frame = Data(repeating: 0, count: 20)
        frame[0] = 1
        frame[1] = 0x11
        for device in devices {
            let (id, parsed) = try XCTUnwrap(MotionWire.decodeSample(MotionWire.sample(frame, from: device.id)))
            XCTAssertEqual(id, device.id)
            XCTAssertEqual(parsed, frame)
        }
    }
}
