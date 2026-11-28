import simd
import XCTest
@testable import VoiceStickTennis

final class RacketDrillTests: XCTestCase {
    private let neutral = simd_quatf(angle: 0, axis: SIMD3(0, 1, 0))

    private func observe(_ drill: inout RacketDrill, _ pose: simd_quatf,
                         at time: UInt32, stroke: Bool = false) {
        var data = Data(repeating: 0, count: 20)
        data[0] = 1
        data[1] = 0x11
        for index in 0..<4 { data[4 + index] = UInt8(truncatingIfNeeded: time >> (8 * index)) }
        if stroke { data[15] = 16 } // 250 degrees per second on board X
        drill.observe(pose: pose, sample: IMUSample(data)!, stroke: stroke)
    }

    func testFourPromptsAndAutomaticRearming() {
        var drill = RacketDrill()
        XCTAssertFalse(drill.advance())
        XCTAssertTrue(drill.advance())
        var time: UInt32 = 0
        for (index, pose) in [
            simd_quatf(angle: .pi / 4, axis: SIMD3(0, 1, 0)),
            simd_quatf(angle: -.pi / 4, axis: SIMD3(0, 1, 0)),
            simd_quatf(angle: -.pi / 4, axis: SIMD3(0, 0, 1))
        ].enumerated() {
            for _ in 0..<11 { observe(&drill, pose, at: time); time += 50 }
            XCTAssertEqual(drill.results.count, index + 1)
            XCTAssertTrue(drill.prompt.contains("方向吻合"))
            XCTAssertFalse(drill.advance())
            XCTAssertEqual(drill.phase, .center)
            for _ in 0..<10 { observe(&drill, neutral, at: time); time += 50 }
            XCTAssertEqual(drill.phase, .measure)
        }
        observe(&drill, neutral, at: time, stroke: true)
        for _ in 0..<19 { time += 50; observe(&drill, neutral, at: time) }
        XCTAssertTrue(drill.prompt.contains("识别 1 次"))
        XCTAssertFalse(drill.advance())
        XCTAssertEqual(drill.phase, .complete)
        XCTAssertEqual(drill.results.count, 4)
    }

    func testOppositeAndInsufficientAnglesAndInterruptedHold() {
        var drill = RacketDrill()
        _ = drill.advance()
        _ = drill.advance()
        let left = simd_quatf(angle: -.pi / 4, axis: SIMD3(0, 1, 0))
        for t: UInt32 in [50, 100, 100, 25, 900, 1100] { observe(&drill, left, at: t) }
        XCTAssertEqual(drill.phase, .measure)
        for t: UInt32 in [1250, 1400] { observe(&drill, left, at: t) }
        XCTAssertTrue(drill.prompt.contains("方向相反"))
        _ = drill.advance()
        XCTAssertTrue(drill.advance())
        _ = drill.advance()
        XCTAssertTrue(drill.prompt.contains("转角不足"))
    }
}
