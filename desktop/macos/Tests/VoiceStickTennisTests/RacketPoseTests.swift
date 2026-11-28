import simd
import XCTest
@testable import VoiceStickTennis

final class RacketPoseTests: XCTestCase {
    private func sample(_ ms: UInt32, acceleration: SIMD3<Double> = .zero,
                        gyro: SIMD3<Double> = .zero) -> IMUSample {
        var data = Data([1, 0x11, 0, 0])
        for i in 0..<4 { data.append(UInt8(truncatingIfNeeded: ms >> (8*i))) }
        for v in [acceleration * 8192, gyro * 16.384] {
            for value in [v.x, v.y, v.z] {
                let bits = UInt16(bitPattern: Int16(value.rounded()))
                data.append(UInt8(truncatingIfNeeded: bits))
                data.append(UInt8(bits >> 8))
            }
        }
        return IMUSample(data)!
    }

    func testSlowTwistMovesFaceWithoutStrokeThenHolds() {
        var pose = RacketPose()
        var detector = SwingDetector()
        for tick in 0...100 {
            let reading = sample(UInt32(tick * 10), acceleration: SIMD3(1, 0, 0), gyro: SIMD3(30, 0, 0))
            XCTAssertNil(detector.sample(reading))
            pose.update(reading)
        }
        let normal = pose.orientation.act(SIMD3(0, 0, 1))
        XCTAssertEqual(normal.x, -0.5, accuracy: 0.003)
        XCTAssertEqual(normal.z, 0.866, accuracy: 0.003)
        for tick in 101...160 {
            pose.update(sample(UInt32(tick * 10), acceleration: SIMD3(1, 0, 0)))
        }
        XCTAssertLessThan(simd_length(pose.orientation.act(SIMD3(0, 0, 1)) - normal), 0.002)
    }

    func testGapReorderingAndTimeWrap() {
        var pose = RacketPose()
        let rate = SIMD3<Double>(0, 0, 90)
        for time: UInt32 in [.max - 9, 0, 10, 10, 5, 2010] {
            pose.update(sample(time, gyro: rate))
        }
        XCTAssertEqual(pose.orientation.angle, Float.pi / 100, accuracy: 0.0001)
    }

    func testInvertedAndSidewaysRacketStayAboveTheCourt() {
        for practice in [true, false] {
            for axis: SIMD3<Float> in [[1, 0, 0], [0, 0, 1], simd_normalize([1, 1, 1])] {
                for angle in stride(from: Float.zero, through: 2 * .pi, by: .pi / 12) {
                    let face = RacketFace(pose: simd_quatf(angle: angle, axis: axis), lane: 2.8, practice: practice)
                    for point: SIMD3<Float> in [[0, 0.685, 0], [0, -0.685, 0], [0.535, 0, 0],
                                                [-0.535, 0, 0], [0.053, -1.125, 0.053]] {
                        XCTAssertGreaterThan((face.center + face.rotation.act(point * face.scale)).y, 0.03)
                    }
                }
            }
        }
    }

    func testGravityCorrectsTiltWithoutGyroscopeAndRecenterClearsIt() {
        var pose = RacketPose()
        pose.update(sample(0, acceleration: SIMD3(0, 0, 1)))
        for tick in 1...300 {
            pose.update(sample(UInt32(tick*10), acceleration: SIMD3(0, 0.5, sqrt(0.75))))
        }
        XCTAssertEqual(pose.orientation.angle, .pi / 6, accuracy: 0.012)
        pose = RacketPose()
        pose.update(sample(3020, acceleration: SIMD3(0, 0.5, sqrt(0.75))))
        XCTAssertEqual(pose.orientation.angle, 0, accuracy: 0.0001)
    }
}
