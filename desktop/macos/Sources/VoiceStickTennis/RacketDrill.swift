import Foundation
import simd

/// Measurements are relative to one grip, established explicitly at the start.
/// Readings from keyboard controls never enter this check.
struct RacketDrill {
    enum Phase { case idle, grip, center, measure, result, complete }
    private(set) var phase = Phase.idle
    private(set) var step = 0
    private(set) var results: [String] = []
    private var heldSince: UInt32?
    private var direction = 0
    private var extreme = Float.zero
    private var peak = Float.zero
    private var strokes = 0
    private var firstStroke: UInt32?
    private var previous: UInt32?

    var button: String {
        switch phase {
        case .idle: return "动作测试"
        case .grip: return "就位，回正"
        case .center: return "以此握姿回正"
        case .measure: return "我做完了"
        case .result: return step == 3 ? "看结果" : "下一题"
        case .complete: return "重新测试"
        }
    }
    var prompt: String {
        switch phase {
        case .idle: return ""
        case .grip: return "准备　USB 口朝下，屏幕朝自己。握稳后点「就位，回正」。"
        case .center: return "回到起始握姿：USB 口朝下、屏幕朝自己，停半秒。"
        case .measure:
            switch step {
            case 0: return "1 / 4　保持拍柄竖直，把屏幕慢慢转向你的右手边约 45°，停一秒。"
            case 1: return "2 / 4　保持拍柄竖直，把屏幕慢慢转向你的左手边约 45°，停一秒。"
            case 2: return "3 / 4　屏幕朝自己，把拍头向右肩倾斜，像钟针从 12 点转向 2 点，停一秒。"
            default: return "4 / 4　先停一下，再快速挥一拍，然后停住。"
            }
        case .result: return results.last ?? ""
        case .complete: return results.joined(separator: "　·　")
        }
    }
    /// Returns true when the player explicitly asks to establish a new zero.
    mutating func advance() -> Bool {
        switch phase {
        case .idle, .complete:
            self = RacketDrill()
            phase = .grip
        case .grip, .center:
            heldSince = nil
            previous = nil
            phase = .measure
            return true
        case .measure:
            finishStep()
        case .result:
            if step == 3 { phase = .complete }
            else { step += 1; phase = .center }
        }
        return false
    }

    mutating func observe(pose: simd_quatf, sample: IMUSample, stroke: Bool) {
        guard phase == .center || phase == .measure else { return }
        let t = sample.time
        if let previous {
            let dt = t &- previous
            guard dt > 0 && dt < 0x80000000 else { return }
            if dt > 250 { heldSince = nil }
        }
        previous = t
        let speed = simd_length(sample.rotationRate)
        if phase == .center {
            let neutral = abs(simd_dot(pose.vector, SIMD4(0, 0, 0, 1))) > cos(Float.pi/24)
            if neutral && speed < 35 {
                if heldSince == nil { heldSince = t }
                if let heldSince, t &- heldSince >= 400 {
                    self.heldSince = nil
                    phase = .measure
                }
            } else { heldSince = nil }
            return
        }
        peak = max(peak, speed)
        if step == 3 {
            if stroke {
                strokes += 1
                if firstStroke == nil { firstStroke = t }
            }
            if let firstStroke, t &- firstStroke >= 900 { finishStep() }
            return
        }
        let normal = pose.act(SIMD3(0, 0, 1))
        let tip = pose.act(SIMD3(0, 1, 0))
        let angle = (step == 2 ? atan2(tip.x, tip.y) : atan2(normal.x, normal.z)) * 180 / .pi
        if abs(angle) > abs(extreme) { extreme = angle }
        let sign = angle > 0 ? 1 : -1
        if abs(angle) < 25 || speed > 45 || direction != sign { heldSince = nil }
        direction = sign
        if abs(angle) >= 25 && speed <= 45 {
            if heldSince == nil { heldSince = t }
            if let heldSince, t &- heldSince >= 450 {
                extreme = angle
                finishStep()
            }
        }
    }

    private mutating func finishStep() {
        if step == 3 {
            results.append("快挥：识别 \(strokes) 次，峰值 \(Int(peak.rounded()))°/s")
        } else {
            let expected = step == 1 ? -extreme : extreme
            let verdict = expected >= 25 ? "方向吻合" : (expected <= -25 ? "方向相反" : "转角不足")
            results.append(["右翻", "左翻", "右倾"][step] + "：\(Int(extreme.rounded()))°，" + verdict)
        }
        phase = .result
        heldSince = nil
        previous = nil
        firstStroke = nil
        extreme = 0
        direction = 0
        peak = 0
        strokes = 0
    }
}
