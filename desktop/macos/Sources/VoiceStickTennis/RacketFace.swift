import simd

/// Court coordinates shared by the renderer and the rally, with the origin at the strings.
struct RacketFace {
    static let ballRadius: Float = 0.14
    static let ringRadius: Float = 0.49
    static let rim: Float = 0.045
    static let stretch: Float = 1.28
    static let contactZ = 7.15
    static let matchScale: Float = 0.86
    let center: SIMD3<Float>
    let rotation: simd_quatf
    let scale: Float

    init(pose: simd_quatf, lane: Double, practice: Bool) {
        scale = practice ? 1 : Self.matchScale
        let neutral = practice ? simd_quatf(real: 1, imag: .zero)
            : simd_quatf(angle: -0.44, axis: SIMD3(0, 0, 1)) * simd_quatf(angle: -0.16, axis: SIMD3(1, 0, 0))
        rotation = neutral * pose
        // In a match the auto-positioned arm helps bring the face to the passing ball.
        let lever = SIMD3<Float>(0, practice ? 0.87 : 0.19, 0)
        var position = practice ? SIMD3<Float>(0, 1.95, 7.5) + rotation.act(lever)
            : SIMD3<Float>(Float(lane), Rally.receivingHeight, Float(Self.contactZ)) + rotation.act(lever) - neutral.act(lever)
        let up = rotation.inverse.act(SIMD3<Float>(0, 1, 0))
        let headBottom = Self.ringRadius * simd_length(SIMD2(up.x, up.y * Self.stretch)) + Self.rim * Self.stretch
        let gripBottom = 0.87 * up.y + 0.255 * abs(up.y) + 0.053 * (abs(up.x) + abs(up.z))
        position.y = max(position.y, 0.08 + scale * max(headBottom, gripBottom))
        center = position
    }

    func localBall(_ ball: BallPosition) -> SIMD3<Float> {
        rotation.inverse.act(ball.center - center)
    }

    /// Closest approach of the moving ball in the moving face's local frame.
    /// Padding allows for the ball radius, the rim and a little hand tracking error.
    func touches(from start: SIMD3<Float>, to end: SIMD3<Float>) -> Bool {
        let padding = Self.ballRadius + 0.16
        let radius = (Self.ringRadius + Self.rim) * scale
        let extents = SIMD3(radius + padding, radius * Self.stretch + padding, padding + Self.rim * scale)
        let a = start / extents
        let d = end / extents - a
        let t = max(0, min(1, -simd_dot(a, d) / max(0.000001, simd_length_squared(d))))
        return simd_length_squared(a + t * d) <= 1
    }
}
