import Foundation

public struct SwingPrompt: Codable, Equatable {
    public enum Phase: String, Codable { case waiting, listening, ambiguous, timedOut, joined }
    public var phase: Phase
    public var role: MotionRole
    public let attempt: UUID

    enum CodingKeys: String, CodingKey { case phase, role, attempt }
    public init(role: MotionRole) {
        attempt = UUID()
        phase = .waiting
        self.role = role
    }
}

/// The host compares event arrival times on one clock. Device sampling clocks
/// are independent. A fresh token fences delayed events between roles/retries.
public struct SwingJoin {
    public static let settleTime = 0.35
    public private(set) var prompt: SwingPrompt
    public private(set) var token = 0
    public private(set) var candidates = Set<UUID>()
    public private(set) var chosen: [String: UUID] = [:]
    private let roles: [MotionRole]
    private var expires: TimeInterval?
    private var settleAt: TimeInterval?
    private var retryAt: TimeInterval = 0
    private var moved = Set<UUID>()

    public init(roles: [MotionRole]) {
        precondition(!roles.isEmpty)
        self.roles = roles
        prompt = SwingPrompt(role: roles[0])
    }

    public mutating func update(available: Set<UUID>, busy: Bool, now: TimeInterval) {
        guard prompt.phase != .timedOut, prompt.phase != .joined else { return }
        if !Set(chosen.values).isSubset(of: available) {
            chosen.removeAll()
            prompt.role = roles[0]
            stopRound(.waiting)
            expires = nil
        }
        if let expires, now >= expires {
            stopRound(.timedOut)
            return
        }
        let eligible = available.subtracting(chosen.values)
        guard !busy, !eligible.isEmpty else {
            stopRound(.waiting)
            return
        }
        if prompt.phase == .ambiguous && now < retryAt { return }
        if token == 0 {
            token = Int.random(in: 1...Int(Int32.max))
            prompt.phase = .listening
            if expires == nil { expires = now + 12 }
        }
        candidates = eligible
        if let settleAt, now >= settleAt {
            if moved.count == 1, let id = moved.first, eligible.contains(id) {
                chosen[prompt.role.rawValue] = id
                if chosen.count == roles.count {
                    stopRound(.joined)
                } else {
                    prompt.role = roles[chosen.count]
                    stopRound(.waiting)
                    expires = nil
                    update(available: available, busy: busy, now: now)
                }
            } else {
                stopRound(.ambiguous)
                retryAt = now + 0.6
            }
        }
    }

    /// Returns true when the host should schedule a decision after settleTime.
    public mutating func report(id: UUID, token: Int, now: TimeInterval) -> Bool {
        guard token != 0, token == self.token, candidates.contains(id),
              prompt.phase == .listening, now < (expires ?? 0),
              now <= (settleAt ?? .infinity) else { return false }
        moved.insert(id)
        guard settleAt == nil else { return false }
        settleAt = now + Self.settleTime
        return true
    }

    private mutating func stopRound(_ phase: SwingPrompt.Phase) {
        token = 0
        candidates.removeAll()
        moved.removeAll()
        settleAt = nil
        prompt.phase = phase
    }
}
