import Foundation
import XCTest
@testable import VoiceStickApp

final class InputProtocolTests: XCTestCase {
    func testControlSingleAndChunkedRoundTrip() throws {
        var msgID: UInt8 = 7
        let small = Data(#"{"id":1,"method":"device.get_info"}"#.utf8)
        let single = BleProtocol.encodeControlFrames(jsonData: small, maximumWriteLength: 182, messageID: &msgID)
        XCTAssertEqual(single.count, 1)
        XCTAssertEqual(single[0][0], 2)
        XCTAssertEqual(single[0][1], 0x10)
        var reassembler = ControlReassembler()
        XCTAssertEqual(reassembler.receive(single[0]), small)

        let large = Data(repeating: UInt8(ascii: "a"), count: 400)
        let chunks = BleProtocol.encodeControlFrames(jsonData: large, maximumWriteLength: 182, messageID: &msgID)
        XCTAssertEqual(chunks.count, 3)
        XCTAssertEqual(chunks[0][1], 0x1F)
        XCTAssertNil(reassembler.receive(chunks[0], now: 10.0))
        XCTAssertNil(reassembler.receive(chunks[1], now: 10.1))
        XCTAssertEqual(reassembler.receive(chunks[2], now: 10.2), large)
    }

    func testDeviceInfoV2DecodesCapabilitiesAndBattery() throws {
        let json = """
        {"id":1,"result":{"protocol_version":2,"hardware":"stick_s3","firmware_version":"0.5.0","device_uuid":"5a8f3b0e-2c71-4e6b-9d12-7f84b06d3ac1","max_message_bytes":1024,"buttons":["primary","secondary"],"streams":{"input":{},"audio_in":{"codecs":["opus"],"sample_rates":[16000],"channels":[1],"frame_ms":[60],"triggers":["button_hold","button_toggle","immediate"]},"imu":{"channels":["accel","gyro"],"rates_hz":[50,100],"accel_lsb_per_g":8192,"gyro_lsb_per_dps":16.384,"max_lease_ms":5000}},"display":{"scenes":["idle","recording","busy","error"]},"battery":{"percent":73,"charging":false,"usb_powered":true}}}
        """
        let data = Data(json.utf8)
        guard case .rpcResult(let id, _, let raw)? = BleProtocol.parseControlMessage(data) else {
            return XCTFail("Expected rpcResult")
        }
        XCTAssertEqual(id, 1)
        let info = try XCTUnwrap(BleProtocol.decodeDeviceInfoResult(from: raw))
        XCTAssertTrue(info.supportsProtocolV2)
        XCTAssertEqual(info.hardware, "stick_s3")
        XCTAssertEqual(info.buttons, ["primary", "secondary"])
        XCTAssertEqual(info.streams?.imu?.supportsMotionGame, true)
        XCTAssertEqual(info.streams?.imu?.supportsSwingProbe, true)
        XCTAssertEqual(info.battery, DeviceBatteryStatus(percent: 73, charging: false, usbPowered: true))
    }

    func testLegacyV1DeviceInfoStillParsesForUpgradePrompt() throws {
        let payload = Data(#"{"event":"device_info","hardware":"stick_s3","firmware_version":"0.4.6"}"#.utf8)
        let frame = Data([1, 0x10, UInt8(payload.count), 0]) + payload
        let legacy = try XCTUnwrap(BleProtocol.parseLegacyDeviceInfo(frame))
        XCTAssertEqual(legacy.hardware, "stick_s3")
        XCTAssertEqual(legacy.firmwareVersion, "0.4.6")
    }

    func testControlRpcErrorAndSystemEvents() throws {
        let errData = Data(#"{"id":4,"error":{"code":"busy","message":"ota active"}}"#.utf8)
        guard case .rpcError(let id, let err)? = BleProtocol.parseControlMessage(errData) else {
            return XCTFail("Expected rpcError")
        }
        XCTAssertEqual(id, 4)
        XCTAssertEqual(err, RpcError(code: "busy", message: "ota active"))

        let closedData = Data(#"{"event":"stream.closed","seq":2,"timestamp_ms":9000,"data":{"stream_id":3,"reason":"lease_expired"}}"#.utf8)
        guard case .streamClosed(let sid, let reason)? = BleProtocol.parseControlMessage(closedData) else {
            return XCTFail("Expected streamClosed")
        }
        XCTAssertEqual(sid, 3)
        XCTAssertEqual(reason, "lease_expired")
    }

    func testAudioStreamFrameV2SupportsExtendedHeaderAndTimestamp() throws {
        let header: [UInt8] = [
            2, 0x01, 20, 0,       // ver=2, type=audio_in, header_len=20 (2 ext bytes)
            2, 0,                 // stream_id=2
            0x01, 0,              // flags=START, reserved=0
            7, 0, 0, 0,           // seq=7
            0xE8, 0x03, 0, 0,     // timestamp_ms=1000
            2, 0,                 // payload_len=2
            0xAA, 0xBB,           // extension bytes
            10, 11                // payload
        ]
        let frame = try XCTUnwrap(BleProtocol.parseAudioStreamFrame(Data(header)))
        XCTAssertEqual(frame.streamID, 2)
        XCTAssertTrue(frame.isStart)
        XCTAssertEqual(frame.seq, 7)
        XCTAssertEqual(frame.timestampMs, 1000)
        XCTAssertEqual(frame.payload, Data([10, 11]))
        XCTAssertNil(BleProtocol.parseAudioStreamFrame(Data(header.dropLast())))
    }

    func testInputAndImuStreamFramesV2() throws {
        let inputBytes: [UInt8] = [
            2, 0x04, 12, 0,       // ver=2, type=input, payload_len=12
            1, 0,                 // stream_id=1
            5, 0,                 // seq=5
            0x10, 0x27, 0, 0,     // timestamp_ms=10000
            0x03, 0, 0, 0         // buttons_mask=0b11
        ]
        let input = try XCTUnwrap(BleProtocol.parseInputStreamFrame(Data(inputBytes)))
        XCTAssertEqual(input.streamID, 1)
        XCTAssertEqual(input.seq, 5)
        XCTAssertEqual(input.timestampMs, 10_000)
        XCTAssertEqual(input.buttonsMask, 0x03)

        let imuBytes: [UInt8] = [
            2, 0x03, 20, 0,       // ver=2, type=imu, payload_len=20 (6-axis)
            3, 0,                 // stream_id=3
            9, 0,                 // seq=9
            0xE8, 0x03, 0, 0,     // timestamp_ms=1000
            0x00, 0x20, 0, 0, 0, 0,
            0x10, 0x00, 0x20, 0x00, 0x30, 0x00
        ]
        let imu = try XCTUnwrap(BleProtocol.parseImuStreamFrame(Data(imuBytes)))
        XCTAssertEqual(imu.streamID, 3)
        XCTAssertEqual(imu.samples.count, 6)
        XCTAssertEqual(imu.samples[0], 8192)
        let legacy = try XCTUnwrap(imu.toLegacyMotionFrame())
        XCTAssertEqual(legacy.count, 20)
        XCTAssertEqual(legacy[0], 1)
        XCTAssertEqual(legacy[1], 0x11)
    }

    func testHostShakeAndSwingDetectors() {
        var shake = ShakeDetector()
        XCTAssertFalse(shake.sample(ax: 0, ay: 0, az: 8192, accelLsbPerG: 8192, timestampUs: 0))
        XCTAssertFalse(shake.sample(ax: 10000, ay: 0, az: 8192, accelLsbPerG: 8192, timestampUs: 60_000))
        XCTAssertFalse(shake.sample(ax: -10000, ay: 0, az: 8192, accelLsbPerG: 8192, timestampUs: 130_000))
        XCTAssertTrue(shake.sample(ax: 10000, ay: 0, az: 8192, accelLsbPerG: 8192, timestampUs: 200_000))

        var swing = SwingProbeDetector()
        for step in 0...6 {
            XCTAssertFalse(swing.sample(gx: 0, gy: 0, gz: 0, gyroLsbPerDps: 16.384, timestampUs: Int64(step) * 20_000))
        }
        XCTAssertFalse(swing.sample(gx: 3000, gy: 0, gz: 0, gyroLsbPerDps: 16.384, timestampUs: 140_000))
        XCTAssertFalse(swing.sample(gx: 3000, gy: 0, gz: 0, gyroLsbPerDps: 16.384, timestampUs: 160_000))
        XCTAssertTrue(swing.sample(gx: 3000, gy: 0, gz: 0, gyroLsbPerDps: 16.384, timestampUs: 180_000))
    }

    func testOtaChunkFitsTheNegotiatedWriteLength() throws {
        XCTAssertNil(BleProtocol.otaChunkSize(maximumWriteLength: 12))
        XCTAssertEqual(BleProtocol.otaChunkSize(maximumWriteLength: 512), 244)
        let chunkSize = try XCTUnwrap(BleProtocol.otaChunkSize(maximumWriteLength: 20))
        let packet = BleProtocol.otaDataPayload(transferID: 42, offset: 0, chunk: Data(count: chunkSize))
        XCTAssertEqual(packet.count, 20)
    }
}
