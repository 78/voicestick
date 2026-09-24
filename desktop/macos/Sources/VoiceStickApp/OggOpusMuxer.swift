import Foundation

final class OggOpusMuxer {
    private let sampleRate: Int
    private let channels: Int
    private var wroteHeaders = false
    private var sequence: UInt32 = 0
    private var granulePosition: UInt64 = 0
    private let serial: UInt32 = 0x5653544B

    init(sampleRate: Int, channels: Int) {
        self.sampleRate = sampleRate
        self.channels = channels
    }

    func reset() {
        wroteHeaders = false
        sequence = 0
        granulePosition = 0
    }

    func append(opusPayload: Data, isLast: Bool) -> Data {
        precondition(!opusPayload.isEmpty, "empty Opus payloads must be written with finish()")

        var out = Data()
        if !wroteHeaders {
            out.append(makePage(packet: opusHead(), granule: 0, headerType: 0x02))
            out.append(makePage(packet: opusTags(), granule: 0, headerType: 0x00))
            wroteHeaders = true
        }

        granulePosition += UInt64(960 * 48_000 / sampleRate)
        out.append(makePage(packet: opusPayload, granule: granulePosition, headerType: isLast ? 0x04 : 0x00))
        return out
    }

    func finish() -> Data {
        var out = Data()
        if !wroteHeaders {
            out.append(makePage(packet: opusHead(), granule: 0, headerType: 0x02))
            out.append(makePage(packet: opusTags(), granule: 0, headerType: 0x00))
            wroteHeaders = true
        }
        out.append(makeEmptyPage(granule: granulePosition, headerType: 0x04))
        return out
    }

    private func opusHead() -> Data {
        var data = Data("OpusHead".utf8)
        data.append(1)
        data.append(UInt8(channels))
        data.append(contentsOf: UInt16(312).littleEndianBytes)
        data.append(contentsOf: UInt32(sampleRate).littleEndianBytes)
        data.append(contentsOf: UInt16(0).littleEndianBytes)
        data.append(0)
        return data
    }

    private func opusTags() -> Data {
        let vendor = Data("VoiceStick".utf8)
        var data = Data("OpusTags".utf8)
        data.append(contentsOf: UInt32(vendor.count).littleEndianBytes)
        data.append(vendor)
        data.append(contentsOf: UInt32(0).littleEndianBytes)
        return data
    }

    private func makePage(packet: Data, granule: UInt64, headerType: UInt8) -> Data {
        precondition(packet.count <= 255, "v1 muxer expects one lacing segment per packet")

        var page = Data()
        page.append(Data("OggS".utf8))
        page.append(0)
        page.append(headerType)
        page.append(contentsOf: granule.littleEndianBytes)
        page.append(contentsOf: serial.littleEndianBytes)
        page.append(contentsOf: sequence.littleEndianBytes)
        page.append(contentsOf: UInt32(0).littleEndianBytes)
        page.append(1)
        page.append(UInt8(packet.count))
        page.append(packet)

        let crc = OggCRC.checksum(page)
        page.replaceSubrange(22..<26, with: crc.littleEndianBytes)
        sequence += 1
        return page
    }

    private func makeEmptyPage(granule: UInt64, headerType: UInt8) -> Data {
        var page = Data()
        page.append(Data("OggS".utf8))
        page.append(0)
        page.append(headerType)
        page.append(contentsOf: granule.littleEndianBytes)
        page.append(contentsOf: serial.littleEndianBytes)
        page.append(contentsOf: sequence.littleEndianBytes)
        page.append(contentsOf: UInt32(0).littleEndianBytes)
        page.append(0)

        let crc = OggCRC.checksum(page)
        page.replaceSubrange(22..<26, with: crc.littleEndianBytes)
        sequence += 1
        return page
    }
}

private extension FixedWidthInteger {
    var littleEndianBytes: [UInt8] {
        withUnsafeBytes(of: self.littleEndian) { Array($0) }
    }
}
