import MotionInput
import XCTest

final class SwingJoinTests: XCTestCase {
    let a = UUID(), b = UUID(), c = UUID()
    var all: Set<UUID> { [a, b, c] }

    func testAllCandidatesThenOnlyTheOneThatSwung() {
        var join = SwingJoin(roles: [.racket])
        join.update(available: all, busy: false, now: 0)
        XCTAssertEqual(join.candidates, all)
        XCTAssertTrue(join.report(id: c, token: join.token, now: 1))
        XCTAssertFalse(join.report(id: c, token: join.token, now: 1.05))
        join.update(available: all, busy: false, now: 1.2)
        XCTAssertTrue(join.chosen.isEmpty)
        join.update(available: all, busy: false, now: 1.36)
        XCTAssertEqual(join.chosen, ["racket": c])
        XCTAssertEqual(join.prompt.phase, .joined)
        XCTAssertTrue(join.candidates.isEmpty)
    }

    func testSimultaneousDevicesAndStaleEventsDoNotBind() {
        var join = SwingJoin(roles: [.leftHand, .rightHand])
        join.update(available: all, busy: false, now: 0)
        let old = join.token
        XCTAssertTrue(join.report(id: a, token: old, now: 0.2))
        XCTAssertFalse(join.report(id: c, token: old, now: 0.4))
        join.update(available: all, busy: false, now: 0.56)
        XCTAssertEqual(join.prompt.phase, .ambiguous)
        XCTAssertTrue(join.chosen.isEmpty)
        join.update(available: all, busy: false, now: 1.3)
        XCTAssertFalse(join.report(id: b, token: old, now: 1.4))
        XCTAssertTrue(join.report(id: c, token: join.token, now: 1.5))
        join.update(available: all, busy: false, now: 1.9)
        XCTAssertEqual(join.prompt.role, .rightHand)
        XCTAssertEqual(join.candidates, [a, b])
        XCTAssertFalse(join.report(id: c, token: join.token, now: 2))
        XCTAssertTrue(join.report(id: a, token: join.token, now: 2))
        join.update(available: all, busy: false, now: 2.4)
        XCTAssertEqual(join.chosen, ["leftHand": c, "rightHand": a])
    }

    func testOfficePreemptionTimeoutAndDisconnect() {
        var join = SwingJoin(roles: [.racket])
        join.update(available: all, busy: true, now: 0)
        XCTAssertTrue(join.candidates.isEmpty)
        join.update(available: all, busy: false, now: 4)
        XCTAssertTrue(join.report(id: c, token: join.token, now: 4.1))
        join.update(available: [a, b], busy: false, now: 4.5)
        XCTAssertTrue(join.chosen.isEmpty)
        join.update(available: all, busy: false, now: 5.5)
        XCTAssertTrue(join.report(id: b, token: join.token, now: 6))
        join.update(available: all, busy: true, now: 6.1)
        XCTAssertTrue(join.candidates.isEmpty)
        join.update(available: all, busy: false, now: 17)
        XCTAssertTrue(join.chosen.isEmpty)
        XCTAssertEqual(join.prompt.phase, .timedOut)
    }

    func testSwingRequestDoesNotAutoAssignSoleDevice() {
        var bindings = MotionBindings()
        let devices = [MotionDevice(id: a, name: "Stick")]
        XCTAssertTrue(bindings.apply(InputRequest(roles: [.racket], swingToJoin: true), devices: devices))
        bindings.discover(devices)
        XCTAssertTrue(bindings.assignments.isEmpty)
    }
}
