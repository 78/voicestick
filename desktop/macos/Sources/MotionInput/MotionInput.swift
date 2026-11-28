import Darwin
import Foundation

public enum MotionRole: String, Codable, CaseIterable {
    case racket, leftHand, rightHand

    public var title: String {
        switch self {
        case .racket: return "球拍"
        case .leftHand: return "左手"
        case .rightHand: return "右手"
        }
    }
}

public struct MotionDevice: Codable, Equatable {
    public let id: UUID
    public let name: String
    public init(id: UUID, name: String) { self.id = id; self.name = name }
}

public struct InputRequest: Codable {
    public let roles: [MotionRole]
    /// nil discovers devices. An empty map explicitly selects keyboard practice.
    public let swingToJoin: Bool?
    public let assignments: [String: UUID]?
    public init(roles: [MotionRole], assignments: [String: UUID]? = nil, swingToJoin: Bool? = nil) {
        self.swingToJoin = swingToJoin
        self.roles = roles
        self.assignments = assignments
    }
}

public struct InputRoster: Codable, Equatable {
    public var swingPrompt: SwingPrompt?
    public var devices: [MotionDevice] = []
    public var assignments: [String: UUID] = [:]
    public var streaming: Set<UUID> = []
    public var unavailable: Set<UUID> = []
    public var busy = false
    public init() {}
}

/// A session owns a one-to-one mapping. Retain a missing device's role until
/// the player changes it, so reconnects cannot silently swap hands.
public struct MotionBindings {
    public private(set) var roles: [MotionRole] = []
    public private(set) var assignments: [String: UUID] = [:]
    private var playerChose = false
    public init() {}

    @discardableResult
    public mutating func apply(_ request: InputRequest, devices: [MotionDevice]) -> Bool {
        guard !request.roles.isEmpty, Set(request.roles).count == request.roles.count,
              roles.isEmpty || request.roles == roles else { return false }
        if request.swingToJoin == true {
            guard request.assignments == nil else { return false }
            playerChose = true
            assignments = [:]
        }
        if let proposed = request.assignments {
            let ids = Set(devices.map(\.id))
            guard proposed.isEmpty || Set(proposed.keys) == Set(request.roles.map(\.rawValue)),
                  Set(proposed.values).count == proposed.count,
                  proposed.allSatisfy({ ids.contains($0.value) || assignments[$0.key] == $0.value })
            else { return false }
            assignments = proposed
            playerChose = true
        }
        roles = request.roles
        discover(devices)
        return true
    }

    public mutating func discover(_ devices: [MotionDevice]) {
        // Seeing several candidates always requires an explicit choice, even
        // if some of them subsequently disconnect while the picker is open.
        if devices.count > 1 { playerChose = true }
        if !playerChose, assignments.isEmpty, roles.count == 1, let only = devices.first {
            assignments[roles[0].rawValue] = only.id
        }
    }

    public func role(for id: UUID) -> MotionRole? {
        roles.first { assignments[$0.rawValue] == id }
    }
}

/// Local AF_UNIX datagrams preserve packet boundaries. JSON carries roster and
/// binding messages. A binary sample carries the BLE frame and source UUID.
public enum MotionWire {
    public static func sample(_ frame: Data, from id: UUID) -> Data {
        var result = frame
        var uuid = id.uuid
        withUnsafeBytes(of: &uuid) { result.append(contentsOf: $0) }
        return result
    }

    public static func sourceID(_ data: Data, offset: Int) -> UUID? {
        guard data.count == offset + 16 else { return nil }
        var id = UUID().uuid
        _ = withUnsafeMutableBytes(of: &id) { data.copyBytes(to: $0, from: offset..<data.count) }
        return UUID(uuid: id)
    }

    public static func decodeSample(_ data: Data) -> (UUID, Data)? {
        guard data.count == 36, data[0] == 1, data[1] == 0x11 else { return nil }
        guard let id = sourceID(data, offset: 20) else { return nil }
        return (id, Data(data.prefix(20)))
    }

    @discardableResult
    public static func send(_ data: Data, on fd: Int32) -> Bool {
        data.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, $0.count, MSG_DONTWAIT) == $0.count }
    }

    public static func receive(on fd: Int32) -> Data? {
        var bytes = [UInt8](repeating: 0, count: 8192)
        let n = Darwin.recv(fd, &bytes, bytes.count, MSG_DONTWAIT)
        return n > 0 ? Data(bytes.prefix(n)) : nil
    }
}
