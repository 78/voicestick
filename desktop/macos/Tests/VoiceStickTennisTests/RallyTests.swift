import simd
import XCTest
@testable import VoiceStickTennis

final class RallyTests: XCTestCase {
    let shot = Stroke(aim: -0.9, strength: 0.7)
    let neutral = simd_quatf(real: 1, imag: .zero)

    func testTimedRallyAndReplay() {
        var game = Rally()
        game.start()
        for count in 1...3 {
            for _ in 0..<600 {
                let face = RacketFace(pose: neutral, lane: game.receivingX, practice: false)
                let before = game.ball
                if game.tryReturn(shot, face: face, from: nil) {
                    XCTAssertEqual(game.ball.z, before.z, accuracy: 0.001)
                    XCTAssertEqual(game.ball.y, before.y, accuracy: 0.001)
                    break
                }
                game.tick(1/120)
            }
            XCTAssertEqual(game.returns, count)
            XCTAssertLessThan(game.farX, 0)
        }
        XCTAssertEqual(game.best, 3)
        for _ in 0..<600 where game.phase != .missed { game.tick(1/120) }
        XCTAssertEqual(game.phase, .missed)
        XCTAssertEqual(game.best, 3)
        XCTAssertEqual(game.streak, 0)
        game.tick(121)
        XCTAssertEqual(game.phase, .finished)
        game.start()
        XCTAssertEqual(game.phase, .incoming)
        XCTAssertEqual(game.returns, 0)
        XCTAssertEqual(game.remaining, 120)
    }

    func testAssistedRacketCanReachThePassingBallWhenRotated() {
        var game = Rally()
        game.start()
        while game.ball.z < RacketFace.contactZ { game.tick(0.002) }
        for pitch in stride(from: -180, to: 180, by: 30) {
            for roll in stride(from: -180, to: 180, by: 30) {
                for yaw in stride(from: -180, to: 180, by: 45) {
                    let rotation = simd_quatf(angle: Float(pitch) * .pi / 180, axis: [1,0,0])
                        * simd_quatf(angle: Float(roll) * .pi / 180, axis: [0,0,1])
                        * simd_quatf(angle: Float(yaw) * .pi / 180, axis: [0,1,0])
                    let face = RacketFace(pose: rotation, lane: game.receivingX, practice: false)
                    let local = face.localBall(game.ball)
                    XCTAssertTrue(face.touches(from: local, to: local))
                }
            }
        }
    }

    func testNewFeedsAndAirSwingsDoNotScore() {
        var game = Rally()
        game.start()
        for _ in 0..<480 {
            let distant = RacketFace(pose: neutral, lane: game.receivingX + 4, practice: false)
            XCTAssertFalse(game.tryReturn(shot, face: distant, from: nil))
            game.tick(1/60)
        }
        XCTAssertEqual(game.returns, 0)
        XCTAssertEqual(game.best, 0)
    }

    func testSweptContactBetweenTwoSamplesAndNearMiss() {
        let face = RacketFace(pose: neutral, lane: 0, practice: false)
        XCTAssertFalse(face.touches(from: SIMD3(0, 0, 0.6), to: SIMD3(0, 0, 0.5)))
        XCTAssertTrue(face.touches(from: SIMD3(0, 0, 0.6), to: SIMD3(0, 0, -0.6)))
        XCTAssertFalse(face.touches(from: SIMD3(1, 0, 0.6), to: SIMD3(1, 0, -0.6)))
    }

    private func frame(_ time: UInt32, axial: Int16 = 0, swing: Int16 = 0) -> IMUSample {
        var data = Data(repeating: 0, count: 20)
        data[0] = 1
        data[1] = 0x11
        for i in 0..<4 { data[4+i] = UInt8(truncatingIfNeeded: time >> (8*i)) }
        for (index, value) in [(14, axial), (18, swing)] {
            data[index] = UInt8(truncatingIfNeeded: value)
            data[index+1] = UInt8(truncatingIfNeeded: UInt16(bitPattern: value) >> 8)
        }
        return IMUSample(data)!
    }

    func testRestTwistSpikeAndContinuousStroke() {
        var detector = SwingDetector()
        for t in stride(from: 0, through: 150, by: 10) {
            XCTAssertNil(detector.sample(frame(UInt32(t), axial: 9000)))
            XCTAssertNil(detector.active)
        }
        XCTAssertNil(detector.sample(frame(160, swing: 6000)))
        XCTAssertNil(detector.sample(frame(170)))
        for t: UInt32 in [180, 190, 200] { XCTAssertNil(detector.sample(frame(t))) }
        XCTAssertNil(detector.sample(frame(210, swing: -6000)))
        XCTAssertNil(detector.sample(frame(220, swing: -6000)))
        XCTAssertNotNil(detector.sample(frame(230, swing: -6000)))
        XCTAssertNil(detector.sample(frame(240, swing: -6000)))
        XCTAssertLessThan(detector.active?.aim ?? 0, 0)
        detector.consume()
        XCTAssertNil(detector.sample(frame(250, swing: -6000)))
        XCTAssertNil(detector.active)
        for t: UInt32 in [260, 330, 500, 510, 520] { XCTAssertNil(detector.sample(frame(t, swing: 6000))) }
        XCTAssertNil(detector.active)
    }

    func testBackswingCanReverseIntoContactWithoutAQuietInterval() {
        var detector = SwingDetector()
        for t: UInt32 in [0, 70] { _ = detector.sample(frame(t)) }
        for t: UInt32 in [80, 90, 100] { _ = detector.sample(frame(t, swing: -8000)) }
        XCTAssertLessThan(detector.active?.aim ?? 0, 0)
        for t in stride(from: 110, through: 230, by: 10) {
            XCTAssertNil(detector.sample(frame(UInt32(t), swing: -8000)))
        }
        XCTAssertNil(detector.sample(frame(240, swing: 14000)))
        XCTAssertNil(detector.sample(frame(250, swing: 14000)))
        XCTAssertGreaterThan(detector.sample(frame(260, swing: 14000))?.aim ?? 0, 0)
        _ = detector.sample(frame(270))
        XCTAssertNil(detector.active)
    }

    func testClockSkewOverFullMatchDoesNotHideSwingsButABacklogIsStale() {
        var clock = SampleClock()
        for ms in stride(from: 0, through: 120_000, by: 10) {
            let host = Double(ms) * 1.0024 / 1000
            let jitter = Double(20 - ms % 30) / 1000
            let age = clock.age(of: UInt32(ms), receivedAt: host + jitter)!
            XCTAssertLessThan(age, 0.025)
        }
        let delayed = clock.age(of: 120_010, receivedAt: 120.010 * 1.0024 + 0.22)!
        XCTAssertGreaterThan(delayed, 0.19)
    }

    func testStationarySampleEndsStrokeAndClockRejectsBacklog() {
        var detector = SwingDetector()
        for t: UInt32 in [.max - 95, .max - 25] { _ = detector.sample(frame(t)) }
        for t: UInt32 in [.max - 15, .max - 5, 5] { _ = detector.sample(frame(t, swing: 8000)) }
        XCTAssertNotNil(detector.active)
        _ = detector.sample(frame(15))
        XCTAssertNil(detector.active)
        var clock = SampleClock()
        XCTAssertEqual(clock.age(of: .max - 10, receivedAt: 40), 0)
        XCTAssertGreaterThan(clock.age(of: 9, receivedAt: 40.3)!, 0.25)
        XCTAssertNil(clock.age(of: 9, receivedAt: 40.3))
        XCTAssertNil(clock.age(of: 8, receivedAt: 40.3))
        XCTAssertLessThan(clock.age(of: 285, receivedAt: 40.3)!, 0.01)
    }
}
