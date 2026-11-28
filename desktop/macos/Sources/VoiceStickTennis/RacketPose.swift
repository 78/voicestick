import Foundation
import simd

struct IMUSample {
    let time: UInt32
    let acceleration: SIMD3<Float> // g
    let rotationRate: SIMD3<Float> // board axes, degrees/second

    init?(_ data: Data) {
        guard data.count == 20, data[0] == 1, data[1] == 0x11 else { return nil }
        func word(_ i: Int) -> UInt16 { UInt16(data[i]) | UInt16(data[i + 1]) << 8 }
        func axis(_ i: Int) -> Float { Float(Int16(bitPattern: word(i))) }
        time = UInt32(word(4)) | UInt32(word(6)) << 16
        acceleration = SIMD3(axis(8), axis(10), axis(12)) / 8192
        rotationRate = SIMD3(axis(14), axis(16), axis(18)) / 16.384
    }
}

/// Orientation relative to the grip at stream start or the last recenter.
/// The accelerometer constrains tilt; heading is relative to that grip.
struct RacketPose {
    private(set) var orientation = simd_quatf(real: 1, imag: .zero)
    private var previousTime: UInt32?
    private var referenceGravity: SIMD3<Float>?
    private var bias = SIMD3<Float>.zero
    private var quietTime: Float = 0

    mutating func update(_ sample: IMUSample) {
        // StickS3 +X points toward the handle and +Z out through the display.
        // The racket model's handle points down local Y.
        func racketAxes(_ v: SIMD3<Float>) -> SIMD3<Float> { SIMD3(v.y, -v.x, v.z) }
        let accel = racketAxes(sample.acceleration)
        let gyro = racketAxes(sample.rotationRate)
        let gravityLength = simd_length(accel)
        let atRest = abs(gravityLength - 1) < 0.06 && simd_length(gyro) < 1.2
        let elapsed = previousTime.map { sample.time &- $0 }
        if let elapsed, elapsed == 0 || elapsed >= 0x80000000 { return }
        previousTime = sample.time
        if referenceGravity == nil, abs(gravityLength - 1) < 0.08 && simd_length(gyro) < 30 {
            referenceGravity = orientation.act(accel / gravityLength)
        }
        guard let elapsed, elapsed <= 180 else {
            quietTime = 0
            return
        }
        let dt = Float(elapsed) / 1000
        quietTime = atRest ? quietTime + dt : 0
        if quietTime > 0.5 {
            bias += (gyro - bias) * min(1, dt / 2)
        }
        var omega = (gyro - bias) * (.pi / 180)
        if let referenceGravity, abs(gravityLength - 1) < 0.08, simd_length(gyro) < 45 {
            let expected = orientation.inverse.act(referenceGravity)
            omega += simd_cross(accel / gravityLength, expected) * 1.5
        }
        let speed = simd_length(omega)
        if speed > 0.0001 {
            let step = simd_quatf(angle: speed * dt, axis: omega / speed)
            orientation = simd_normalize(orientation * step)
        }
    }
}

/// Estimate sample age from device time so a queued BLE burst cannot hit a later ball.
struct SampleClock {
    private var lastTime: UInt32?
    private var estimatedTime = 0.0

    mutating func age(of time: UInt32, receivedAt now: Double) -> Double? {
        if let lastTime {
            let dt = time &- lastTime
            guard dt > 0, dt < 0x80000000 else { return nil }
            // Follow the lower envelope of host arrival times. A small slew allowance
            // absorbs independent clock rates without snapping forward on a queued burst.
            // The Stick clock in a captured match ran about 0.23% slower than the Mac.
            let deviceSeconds = Double(dt) / 1000
            estimatedTime = min(now, estimatedTime + deviceSeconds * 1.005)
        } else { estimatedTime = now }
        lastTime = time
        return now - estimatedTime
    }
}
