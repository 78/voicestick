import Foundation
import simd

struct Stroke {
    let aim: Double       // -1...1 across the court
    let strength: Double  // 0...1
}

struct SwingDetector {
    private var lastSample: UInt32?
    private var quietSince: UInt32?
    private var armed = false
    private var sweep = SIMD2<Float>.zero
    private var movingSince: UInt32?
    private var lastStrokeTime: UInt32?
    private var strokeDirection = SIMD2<Float>.zero
    private(set) var active: Stroke?

    mutating func consume() { active = nil }

    mutating func sample(_ sample: IMUSample) -> Stroke? {
        let now = sample.time
        let dt = lastSample.map { now &- $0 } ?? 0
        if lastSample != nil && (dt == 0 || dt >= 0x80000000) { return nil }
        if dt > 100 { self = SwingDetector() }
        lastSample = now
        let gyro = sample.rotationRate
        // Rotation around the handle (board X) turns the strings, without sweeping the head.
        let transverse = SIMD2(gyro.y, gyro.z)
        let speed = simd_length(transverse)
        if let lastStrokeTime, now &- lastStrokeTime > 500 { active = nil }
        if speed < 60 {
            active = nil
            movingSince = nil
            sweep = .zero
            if quietSince == nil { quietSince = now }
            if let quietSince, now &- quietSince >= 65 { armed = true }
            return nil
        }
        quietSince = nil
        // A backswing can reverse into a forehand without resting between them.
        // Require a sustained turn against the last confirmed stroke, even after a hit consumed it.
        let reversing = !armed && lastStrokeTime.map { now &- $0 >= 100 } == true
            && speed >= 125 && simd_dot(transverse, strokeDirection) < -0.45 * speed
        guard (armed || reversing) && speed >= 75 else {
            movingSince = nil
            sweep = .zero
            return nil
        }
        if reversing { active = nil }
        guard let movingSince else { self.movingSince = now; return nil }
        sweep += transverse * (Float(dt) / 1000)
        guard now &- movingSince >= 18, simd_length(sweep) >= 5 else { return nil }
        let lateral = abs(gyro.z) > abs(gyro.y) ? gyro.z : gyro.y
        let stroke = Stroke(aim: Double(lateral / speed), strength: min(1, Double(speed) / 550))
        strokeDirection = simd_normalize(sweep)
        lastStrokeTime = now
        self.movingSince = nil
        sweep = .zero
        armed = false
        active = stroke
        return stroke
    }
}

struct BallPosition {
    var center: SIMD3<Float> { SIMD3(Float(x), Float(y), Float(z)) }
    var x: Double
    var y: Double
    var z: Double
}

struct Rally {
    enum Phase { case ready, incoming, returning, missed, finished }
    private(set) var phase = Phase.ready
    private(set) var progress = 0.0
    private(set) var remaining = 120.0
    private(set) var streak = 0
    private(set) var best = 0
    private(set) var returns = 0
    private(set) var nearX = 0.0
    private(set) var farX = 0.0
    private var contact = BallPosition(x: 0, y: 1, z: 7.8)
    private var returnDuration = 1.3

    private(set) var receivingX = 0.0
    var incomingDuration: Double { max(1.65, 2.25 - Double(streak)*0.045) }
    static let bounceProgress = 0.60
    private static let bounceZ = 5.25
    private static let postBounceDistance = 3.2

    private static func reboundHeight(at v: Double) -> Double { 4.4*v*(1-v) + 0.3*v }
    static let receivingHeight = RacketFace.ballRadius + Float(reboundHeight(at: (RacketFace.contactZ - bounceZ) / postBounceDistance))
    var incomingBounce: BallPosition { incomingBall(at: Self.bounceProgress) }

    private func incomingBall(at t: Double) -> BallPosition {
        let u = min(1, t / Self.bounceProgress)
        let v = max(0, (t - Self.bounceProgress) / (1 - Self.bounceProgress))
        let height = t < Self.bounceProgress ? (1-u)*1.05 + 8.8*u*(1-u) : Self.reboundHeight(at: v)
        let z = t < Self.bounceProgress ? -8.4 + (Self.bounceZ + 8.4)*u : Self.bounceZ + Self.postBounceDistance*v
        return BallPosition(x: farX + (nearX-farX)*(z + 8.4)/16.85, y: Double(RacketFace.ballRadius) + height, z: z)
    }

    var ball: BallPosition {
        let t = min(1, progress)
        switch phase {
        case .returning:
            return BallPosition(x: contact.x + (farX-contact.x)*t,
                                y: contact.y*(1-t) + 1.1*t + 9*t*(1-t),
                                z: contact.z + (-8.4-contact.z)*t)
        case .incoming, .missed:
            return incomingBall(at: t)
        default:
            return BallPosition(x: 0, y: 1.1, z: -8.4)
        }
    }

    mutating func start() {
        self = Rally()
        feed()
    }

    func ballRelative(to face: RacketFace, delay: Double = 0) -> SIMD3<Float> {
        face.localBall(incomingBall(at: max(0, progress - delay / incomingDuration)))
    }

    func approaching(_ face: RacketFace) -> Bool {
        guard phase == .incoming else { return false }
        let ahead = min(1, progress + 0.18 / incomingDuration)
        return face.touches(from: face.localBall(ball), to: face.localBall(incomingBall(at: ahead)))
    }

    @discardableResult
    mutating func tryReturn(_ stroke: Stroke, face: RacketFace, from previous: SIMD3<Float>?, delay: Double = 0) -> Bool {
        guard phase == .incoming else { return false }
        let t = progress - delay / incomingDuration
        guard t >= 0, t <= 1 else { return false }
        let impact = incomingBall(at: t)
        let here = face.localBall(impact)
        guard face.touches(from: previous ?? here, to: here) else { return false }
        contact = impact
        farX = stroke.aim * 3.4
        returnDuration = 1.6 - 0.65*stroke.strength
        streak += 1
        returns += 1
        best = max(best, streak)
        progress = 0
        phase = .returning
        return true
    }

    mutating func tick(_ dt: Double) {
        guard phase != .ready && phase != .finished else { return }
        remaining = max(0, remaining - dt)
        if remaining == 0 { phase = .finished; return }
        switch phase {
        case .incoming:
            progress += dt / incomingDuration
            if progress > 1 { progress = 0; streak = 0; phase = .missed }
        case .returning:
            progress += dt / returnDuration
            if progress >= 1 { feed() }
        case .missed:
            progress += dt
            if progress > 0.85 { feed() }
        default: break
        }
    }

    private mutating func feed() {
        nearX = Double.random(in: -2.8...2.8)
        receivingX = farX + (nearX - farX) * (RacketFace.contactZ + 8.4) / 16.85
        progress = 0
        phase = .incoming
    }
}
