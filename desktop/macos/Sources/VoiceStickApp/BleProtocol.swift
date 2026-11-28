import Foundation

struct AudioStreamFrame: Equatable {
    let streamID: UInt16
    let seq: UInt32
    let timestampMs: UInt32
    let flags: UInt8
    let payload: Data

    var isStart: Bool { flags & BleProtocol.audioFlagStart != 0 }
    var isEnd: Bool { flags & BleProtocol.audioFlagEnd != 0 }
    var isFlush: Bool { flags & BleProtocol.audioFlagFlush != 0 }
}

struct ImuStreamFrame: Equatable {
    let streamID: UInt16
    let seq: UInt16
    let timestampMs: UInt32
    let samples: [Int16]

    var hasGyro: Bool { samples.count >= 6 }

    /// Converts a 6-axis v2 IMU frame into the 20-byte local MotionWire sample format (`01 11 ...`).
    func toLegacyMotionFrame() -> Data? {
        guard samples.count >= 6 else { return nil }
        var data = Data(capacity: 20)
        data.append(1)
        data.append(0x11)
        data.appendLittleEndian(seq)
        data.appendLittleEndian(timestampMs)
        for i in 0..<6 {
            data.appendLittleEndian(UInt16(bitPattern: samples[i]))
        }
        return data
    }
}

struct InputStreamFrame: Equatable {
    let streamID: UInt16
    let seq: UInt16
    let timestampMs: UInt32
    let buttonsMask: UInt32
}

struct DeviceBatteryStatus: Decodable, Equatable {
    let percent: Int
    let charging: Bool
    let usbPowered: Bool

    enum CodingKeys: String, CodingKey {
        case percent
        case charging
        case usbPowered = "usb_powered"
    }
}

struct ButtonInput: Equatable {
    enum Action: String {
        case down, up
    }

    let id: String
    let action: Action
    let timestampMs: UInt64
}

struct ImuCapabilities: Decodable, Equatable {
    let channels: [String]
    let ratesHz: [Int]
    let accelLsbPerG: Double
    let gyroLsbPerDps: Double?
    let maxLeaseMs: Int

    var hasAccel: Bool { channels.contains("accel") }
    var hasGyro: Bool { channels.contains("gyro") && gyroLsbPerDps != nil }
    var supportsMotionGame: Bool { hasAccel && hasGyro && ratesHz.contains(100) }
    var supportsSwingProbe: Bool { hasAccel && hasGyro && (ratesHz.contains(50) || ratesHz.contains(100)) }

    enum CodingKeys: String, CodingKey {
        case channels
        case ratesHz = "rates_hz"
        case accelLsbPerG = "accel_lsb_per_g"
        case gyroLsbPerDps = "gyro_lsb_per_dps"
        case maxLeaseMs = "max_lease_ms"
    }
}

struct AudioInCapabilities: Decodable, Equatable {
    let codecs: [String]
    let sampleRates: [Int]
    let channels: [Int]
    let frameMs: [Int]
    let triggers: [String]

    enum CodingKeys: String, CodingKey {
        case codecs
        case sampleRates = "sample_rates"
        case channels
        case frameMs = "frame_ms"
        case triggers
    }
}

struct StreamCapabilities: Decodable, Equatable {
    let audioIn: AudioInCapabilities?
    let imu: ImuCapabilities?

    enum CodingKeys: String, CodingKey {
        case audioIn = "audio_in"
        case imu
    }
}

struct DisplayCapabilities: Decodable, Equatable {
    let scenes: [String]
}

struct DeviceInfoResult: Decodable, Equatable {
    let protocolVersion: Int
    let hardware: String
    let firmwareVersion: String
    let deviceUUID: String?
    let maxMessageBytes: Int?
    let buttons: [String]
    let streams: StreamCapabilities?
    let display: DisplayCapabilities?
    let battery: DeviceBatteryStatus?

    var supportsProtocolV2: Bool {
        protocolVersion == Int(BleProtocol.protocolVersion)
    }

    enum CodingKeys: String, CodingKey {
        case protocolVersion = "protocol_version"
        case hardware
        case firmwareVersion = "firmware_version"
        case deviceUUID = "device_uuid"
        case maxMessageBytes = "max_message_bytes"
        case buttons
        case streams
        case display
        case battery
    }
}

struct LegacyDeviceInfo: Decodable, Equatable {
    let event: String
    let hardware: String?
    let firmwareVersion: String?
    let protocolVersion: Int?
    let buttons: [String]?

    enum CodingKeys: String, CodingKey {
        case event
        case hardware
        case firmwareVersion = "firmware_version"
        case protocolVersion = "protocol_version"
        case buttons
    }
}

struct RpcError: Error, LocalizedError, Equatable {
    let code: String
    let message: String

    var errorDescription: String? {
        "\(code): \(message)"
    }
}

enum ControlMessage {
    case rpcResult(id: Int, result: [String: Any], rawData: Data)
    case rpcError(id: Int, error: RpcError)
    case batteryChanged(DeviceBatteryStatus)
    case streamClosed(streamID: UInt16, reason: String)
    case unknownEvent(String)
}

struct ControlReassembler {
    private var active = false
    private var messageID: UInt8 = 0
    private var nextIndex: UInt8 = 0
    private var count: UInt8 = 0
    private var buffer = Data()
    private var startedAt: TimeInterval = 0
    private let maxBytes: Int
    private let timeout: TimeInterval

    init(maxBytes: Int = BleProtocol.maxControlMessageBytes, timeout: TimeInterval = 2.0) {
        self.maxBytes = maxBytes
        self.timeout = timeout
    }

    mutating func reset() {
        active = false
        buffer.removeAll(keepingCapacity: true)
    }

    /// Feeds one `control_tx` notification frame and returns complete UTF-8 JSON bytes when ready.
    mutating func receive(_ data: Data, now: TimeInterval = ProcessInfo.processInfo.systemUptime) -> Data? {
        guard data.count >= 4, data[0] == BleProtocol.protocolVersion else { return nil }
        let type = data[1]
        let payloadLen = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard data.count >= 4 + payloadLen else { return nil }

        if type == BleProtocol.typeControlJSON {
            guard payloadLen > 0, payloadLen <= maxBytes else { return nil }
            return data.subdata(in: 4..<(4 + payloadLen))
        }

        guard type == BleProtocol.typeControlChunk, payloadLen >= 3 else { return nil }
        let msgID = data[4]
        let index = data[5]
        let total = data[6]
        let slice = data.subdata(in: 7..<(4 + payloadLen))
        guard total >= 2, index < total else {
            reset()
            return nil
        }

        if index == 0 {
            guard slice.count <= maxBytes else { reset(); return nil }
            active = true
            messageID = msgID
            nextIndex = 1
            count = total
            startedAt = now
            buffer = slice
            return nil
        }

        guard active, msgID == messageID, index == nextIndex, total == count,
              now - startedAt <= timeout, buffer.count + slice.count <= maxBytes else {
            reset()
            return nil
        }

        buffer.append(slice)
        nextIndex += 1
        if nextIndex == count {
            let complete = buffer
            reset()
            return complete
        }
        return nil
    }
}

struct FirmwareOTAStateEvent: Decodable {
    let event: String
    let transferID: UInt32?
    let written: UInt32?
    let size: UInt32?
    let code: String?
    let espErr: Int?
    let rebootMs: Int?

    enum CodingKeys: String, CodingKey {
        case event
        case transferID = "transfer_id"
        case written
        case size
        case code
        case espErr = "esp_err"
        case rebootMs = "reboot_ms"
    }
}

/// Host-side shake detector matching the proven 3-alternating-peak algorithm.
struct ShakeDetector {
    private static let minTurnUs: Int64 = 55_000
    private static let maxShakeUs: Int64 = 600_000
    private static let minDeltaMg: Int32 = 640

    private var hasPrevious = false
    private var previous: (x: Int32, y: Int32, z: Int32) = (0, 0, 0)
    private var peak: (x: Int32, y: Int32, z: Int32) = (0, 0, 0)
    private var peaks = 0
    private var startedUs: Int64 = 0
    private var lastPeakUs: Int64 = 0

    mutating func reset() {
        self = ShakeDetector()
    }

    mutating func sample(ax: Int16, ay: Int16, az: Int16, accelLsbPerG: Double, timestampUs: Int64) -> Bool {
        guard accelLsbPerG > 0 else { return false }
        let scale = 1000.0 / accelLsbPerG
        let cur = (
            x: Int32(Double(ax) * scale),
            y: Int32(Double(ay) * scale),
            z: Int32(Double(az) * scale)
        )
        let prev = previous
        previous = cur
        if !hasPrevious {
            hasPrevious = true
            return false
        }
        if timestampUs - startedUs > Self.maxShakeUs {
            peaks = 0
        }
        let dx = cur.x - prev.x
        let dy = cur.y - prev.y
        let dz = cur.z - prev.z
        let threshold = Int64(Self.minDeltaMg) * Int64(Self.minDeltaMg)
        let magSq = Int64(dx) * Int64(dx) + Int64(dy) * Int64(dy) + Int64(dz) * Int64(dz)
        if magSq < threshold { return false }
        if peaks == 0 {
            startedUs = timestampUs
        } else {
            let dot = Int64(dx) * Int64(peak.x) + Int64(dy) * Int64(peak.y) + Int64(dz) * Int64(peak.z)
            if timestampUs - lastPeakUs < Self.minTurnUs || dot > -threshold / 2 {
                return false
            }
        }
        lastPeakUs = timestampUs
        peak = (dx, dy, dz)
        peaks += 1
        if peaks < 3 { return false }
        peaks = 0
        return true
    }
}

/// Host-side swing-to-join detector (quiet grip followed by sustained rotation).
struct SwingProbeDetector {
    private var quietSinceUs: Int64 = 0
    private var movingSinceUs: Int64 = 0
    private var lastSampleUs: Int64 = 0
    private var armed = false

    mutating func reset() {
        self = SwingProbeDetector()
    }

    mutating func sample(gx: Int16, gy: Int16, gz: Int16, gyroLsbPerDps: Double, timestampUs: Int64) -> Bool {
        guard gyroLsbPerDps > 0 else { return false }
        if lastSampleUs != 0 && timestampUs - lastSampleUs > 100_000 {
            reset()
        }
        lastSampleUs = timestampUs
        let vx = Double(gx) / gyroLsbPerDps
        let vy = Double(gy) / gyroLsbPerDps
        let vz = Double(gz) / gyroLsbPerDps
        let speedSq = vx * vx + vy * vy + vz * vz

        if speedSq < 50.0 * 50.0 {
            if quietSinceUs == 0 { quietSinceUs = timestampUs }
            if timestampUs - quietSinceUs >= 100_000 { armed = true }
        } else {
            quietSinceUs = 0
        }

        if !armed || speedSq < 135.0 * 135.0 {
            movingSinceUs = 0
            return false
        }
        if movingSinceUs == 0 { movingSinceUs = timestampUs }
        if timestampUs - movingSinceUs < 30_000 { return false }
        reset()
        return true
    }
}

enum BleProtocol {
    static let protocolVersion: UInt8 = 2
    static let maxControlMessageBytes = 1024

    static let serviceUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5100"
    static let streamTXUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5101"
    static let controlTXUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5102"
    static let controlRXUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5103"
    static let otaRXUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5104"
    static let otaStateUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5105"
    static let pairingAdvertisementUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5106"
    static let reconnectAdvertisementUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5107"
    static let authorizedUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5108"
    static let streamRXUUID = "8F2F0B84-6E6F-4B23-88F7-3A3CEAFC5109"

    static let typeNop: UInt8 = 0x00
    static let typeAudioIn: UInt8 = 0x01
    static let typeAudioOut: UInt8 = 0x02
    static let typeImu: UInt8 = 0x03
    static let typeInput: UInt8 = 0x04
    static let typeControlJSON: UInt8 = 0x10
    static let typeControlChunk: UInt8 = 0x1F

    static let audioFlagStart: UInt8 = 0x01
    static let audioFlagEnd: UInt8 = 0x02
    static let audioFlagFlush: UInt8 = 0x04

    static let otaTypeBegin: UInt8 = 0x20
    static let otaTypeData: UInt8 = 0x21
    static let otaTypeEnd: UInt8 = 0x22
    static let otaTypeAbort: UInt8 = 0x23
    static let otaTypeState: UInt8 = 0x30
    private static let otaDataHeaderLength: UInt8 = 12

    static let legacyDeviceInfoRequest = Data(#"{"event":"get_device_info"}"#.utf8)
    static let legacyUIStateReady = Data(#"{"event":"ui_state","state":"ready","text":""}"#.utf8)

    static func encodeControlFrames(jsonData: Data, maximumWriteLength: Int, messageID: inout UInt8) -> [Data] {
        guard !jsonData.isEmpty, jsonData.count <= maxControlMessageBytes else { return [] }
        let maxWrite = max(maximumWriteLength, 20)
        if 4 + jsonData.count <= maxWrite {
            var frame = Data([protocolVersion, typeControlJSON])
            frame.appendLittleEndian(UInt16(jsonData.count))
            frame.append(jsonData)
            return [frame]
        }

        let maxSlice = max(maxWrite - 7, 13)
        let total = (jsonData.count + maxSlice - 1) / maxSlice
        guard total >= 2, total <= 255 else { return [] }
        let msgID = messageID
        messageID &+= 1

        var frames: [Data] = []
        frames.reserveCapacity(total)
        var offset = 0
        for index in 0..<total {
            let end = min(offset + maxSlice, jsonData.count)
            let slice = jsonData.subdata(in: offset..<end)
            let payloadLen = UInt16(3 + slice.count)
            var frame = Data([protocolVersion, typeControlChunk])
            frame.appendLittleEndian(payloadLen)
            frame.append(msgID)
            frame.append(UInt8(index))
            frame.append(UInt8(total))
            frame.append(slice)
            frames.append(frame)
            offset = end
        }
        return frames
    }

    static func parseControlMessage(_ jsonData: Data) -> ControlMessage? {
        guard let object = try? JSONSerialization.jsonObject(with: jsonData) as? [String: Any] else {
            return nil
        }

        if let id = object["id"] as? Int {
            if let errorObj = object["error"] as? [String: Any],
               let code = errorObj["code"] as? String {
                let message = errorObj["message"] as? String ?? ""
                return .rpcError(id: id, error: RpcError(code: code, message: message))
            }
            if let resultObj = object["result"] as? [String: Any] {
                return .rpcResult(id: id, result: resultObj, rawData: jsonData)
            }
            return nil
        }

        if let event = object["event"] as? String {
            let dataObj = object["data"] as? [String: Any]
            switch event {
            case "battery.changed":
                guard let dataObj,
                      let percent = dataObj["percent"] as? Int, (0...100).contains(percent),
                      let charging = dataObj["charging"] as? Bool,
                      let usbPowered = dataObj["usb_powered"] as? Bool else { return nil }
                return .batteryChanged(DeviceBatteryStatus(percent: percent, charging: charging, usbPowered: usbPowered))
            case "stream.closed":
                guard let dataObj,
                      let sid = dataObj["stream_id"] as? Int, sid > 0, sid <= Int(UInt16.max),
                      let reason = dataObj["reason"] as? String else { return nil }
                return .streamClosed(streamID: UInt16(sid), reason: reason)
            default:
                return .unknownEvent(event)
            }
        }
        return nil
    }

    static func decodeDeviceInfoResult(from responseData: Data) -> DeviceInfoResult? {
        struct Envelope: Decodable {
            let result: DeviceInfoResult
        }
        return try? JSONDecoder().decode(Envelope.self, from: responseData).result
    }

    static func parseLegacyDeviceInfo(_ data: Data) -> LegacyDeviceInfo? {
        guard data.count >= 4, data[0] == 1, data[1] == 0x10 else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard data.count >= 4 + payloadLength else { return nil }
        let payload = data.subdata(in: 4..<(4 + payloadLength))
        guard let info = try? JSONDecoder().decode(LegacyDeviceInfo.self, from: payload),
              info.event == "device_info" else { return nil }
        return info
    }

    static func parseAudioStreamFrame(_ data: Data) -> AudioStreamFrame? {
        guard data.count >= 18, data[0] == protocolVersion, data[1] == typeAudioIn else { return nil }
        let headerLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard headerLength >= 18, data.count >= headerLength else { return nil }
        let streamID = UInt16(littleEndianBytes: data[4..<6])
        let flags = data[6]
        let seq = UInt32(littleEndianBytes: data[8..<12])
        let timestampMs = UInt32(littleEndianBytes: data[12..<16])
        let payloadLength = Int(UInt16(littleEndianBytes: data[16..<18]))
        guard data.count >= headerLength + payloadLength else { return nil }
        return AudioStreamFrame(
            streamID: streamID,
            seq: seq,
            timestampMs: timestampMs,
            flags: flags,
            payload: data.subdata(in: headerLength..<(headerLength + payloadLength))
        )
    }

    static func parseImuStreamFrame(_ data: Data) -> ImuStreamFrame? {
        guard data.count >= 18, data[0] == protocolVersion, data[1] == typeImu else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard payloadLength >= 14, (payloadLength - 8) % 2 == 0, data.count >= 4 + payloadLength else {
            return nil
        }
        let streamID = UInt16(littleEndianBytes: data[4..<6])
        let seq = UInt16(littleEndianBytes: data[6..<8])
        let timestampMs = UInt32(littleEndianBytes: data[8..<12])
        let axisCount = (payloadLength - 8) / 2
        var samples: [Int16] = []
        samples.reserveCapacity(axisCount)
        for i in 0..<axisCount {
            let offset = 12 + i * 2
            samples.append(Int16(bitPattern: UInt16(littleEndianBytes: data[offset..<(offset + 2)])))
        }
        return ImuStreamFrame(streamID: streamID, seq: seq, timestampMs: timestampMs, samples: samples)
    }

    static func parseInputStreamFrame(_ data: Data) -> InputStreamFrame? {
        guard data.count >= 16, data[0] == protocolVersion, data[1] == typeInput else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard payloadLength >= 12, data.count >= 4 + payloadLength else { return nil }
        return InputStreamFrame(
            streamID: UInt16(littleEndianBytes: data[4..<6]),
            seq: UInt16(littleEndianBytes: data[6..<8]),
            timestampMs: UInt32(littleEndianBytes: data[8..<12]),
            buttonsMask: UInt32(littleEndianBytes: data[12..<16])
        )
    }

    static func parseFirmwareOTAStateEvent(_ data: Data) -> FirmwareOTAStateEvent? {
        guard data.count >= 4, data[0] == 1, data[1] == otaTypeState else { return nil }
        let payloadLength = Int(UInt16(littleEndianBytes: data[2..<4]))
        guard data.count >= 4 + payloadLength else { return nil }
        let payload = data.subdata(in: 4..<(4 + payloadLength))
        return try? JSONDecoder().decode(FirmwareOTAStateEvent.self, from: payload)
    }

    static func otaBeginPayload(imageSize: UInt32, transferID: UInt32) -> Data {
        var data = Data([1, otaTypeBegin, 12, 0])
        data.appendLittleEndian(imageSize)
        data.appendLittleEndian(transferID)
        return data
    }

    static func otaDataPayload(transferID: UInt32, offset: UInt32, chunk: Data) -> Data {
        var data = Data([1, otaTypeData, otaDataHeaderLength, 0])
        data.appendLittleEndian(transferID)
        data.appendLittleEndian(offset)
        data.append(chunk)
        return data
    }

    static func otaChunkSize(maximumWriteLength: Int) -> Int? {
        let headerLength = Int(otaDataHeaderLength)
        guard maximumWriteLength > headerLength else { return nil }
        return min(maximumWriteLength - headerLength, 244)
    }

    static func otaEndPayload(transferID: UInt32, imageSize: UInt32) -> Data {
        var data = Data([1, otaTypeEnd, 12, 0])
        data.appendLittleEndian(transferID)
        data.appendLittleEndian(imageSize)
        return data
    }

    static func otaAbortPayload(transferID: UInt32) -> Data {
        var data = Data([1, otaTypeAbort, 8, 0])
        data.appendLittleEndian(transferID)
        return data
    }
}

private extension UInt16 {
    init(littleEndianBytes bytes: Data.SubSequence) {
        self = bytes.enumerated().reduce(0) { $0 | UInt16($1.element) << UInt16($1.offset * 8) }
    }
}

private extension UInt32 {
    init(littleEndianBytes bytes: Data.SubSequence) {
        self = bytes.enumerated().reduce(0) { $0 | UInt32($1.element) << UInt32($1.offset * 8) }
    }
}

private extension Data {
    mutating func appendLittleEndian(_ value: UInt16) {
        append(UInt8(value & 0xff))
        append(UInt8((value >> 8) & 0xff))
    }

    mutating func appendLittleEndian(_ value: UInt32) {
        append(UInt8(value & 0xff))
        append(UInt8((value >> 8) & 0xff))
        append(UInt8((value >> 16) & 0xff))
        append(UInt8((value >> 24) & 0xff))
    }
}
