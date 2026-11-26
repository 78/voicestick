import AVFoundation
import Foundation

final class OpusPCMDecoder {
    static let sampleRate = 24_000

    private let converter: AVAudioConverter
    private let input: AVAudioCompressedBuffer
    private let output: AVAudioPCMBuffer

    init() throws {
        let framesPerPacket = 1_440
        var source = AudioStreamBasicDescription(
            mSampleRate: Double(Self.sampleRate),
            mFormatID: kAudioFormatOpus,
            mFormatFlags: 0,
            mBytesPerPacket: 0,
            mFramesPerPacket: UInt32(framesPerPacket),
            mBytesPerFrame: 0,
            mChannelsPerFrame: 1,
            mBitsPerChannel: 0,
            mReserved: 0
        )
        guard let opus = AVAudioFormat(streamDescription: &source),
              let pcm = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: Double(Self.sampleRate),
                                      channels: 1, interleaved: true),
              let converter = AVAudioConverter(from: opus, to: pcm),
              let output = AVAudioPCMBuffer(pcmFormat: pcm, frameCapacity: UInt32(framesPerPacket * 2))
        else { throw DecodeError.unavailable }
        self.converter = converter
        self.input = AVAudioCompressedBuffer(format: opus, packetCapacity: 1, maximumPacketSize: 255)
        self.output = output
    }

    func decode(_ chunk: Data) throws -> Data {
        // OggOpusMuxer emits whole pages, each with at most one packet.
        var pcm = Data()
        var index = chunk.startIndex
        while index < chunk.endIndex {
            let tableStart = index + 27
            guard tableStart <= chunk.endIndex, chunk[index..<(index + 4)].elementsEqual("OggS".utf8)
            else { throw DecodeError.invalidPage }
            let segmentCount = Int(chunk[index + 26])
            if segmentCount == 0 {
                index = tableStart
                continue
            }
            guard segmentCount == 1, tableStart < chunk.endIndex else { throw DecodeError.invalidPage }
            let packetStart = tableStart + 1
            let packetEnd = packetStart + Int(chunk[tableStart])
            guard packetStart < packetEnd, packetEnd <= chunk.endIndex else { throw DecodeError.invalidPage }
            let packet = chunk[packetStart..<packetEnd]
            if !packet.starts(with: "OpusHead".utf8), !packet.starts(with: "OpusTags".utf8) {
                pcm.append(try decodePacket(packet))
            }
            index = packetEnd
        }
        return pcm
    }

    private func decodePacket(_ packet: Data) throws -> Data {
        input.packetCount = 1
        input.byteLength = UInt32(packet.count)
        packet.copyBytes(to: input.data.assumingMemoryBound(to: UInt8.self), count: packet.count)
        input.packetDescriptions?.pointee = AudioStreamPacketDescription(
            mStartOffset: 0, mVariableFramesInPacket: 0, mDataByteSize: UInt32(packet.count)
        )

        output.frameLength = 0
        var providedInput = false
        var error: NSError?
        let status = converter.convert(to: output, error: &error) { [input] _, inputStatus in
            if providedInput {
                inputStatus.pointee = .noDataNow
                return nil
            }
            providedInput = true
            inputStatus.pointee = .haveData
            return input
        }
        if let error { throw error }
        guard status != .error, let samples = output.int16ChannelData?[0] else { throw DecodeError.failed }
        return Data(bytes: samples, count: Int(output.frameLength) * MemoryLayout<Int16>.size)
    }

    private enum DecodeError: LocalizedError {
        case unavailable
        case invalidPage
        case failed

        var errorDescription: String? {
            switch self {
            case .unavailable: return "The Mac could not initialize the Opus decoder."
            case .invalidPage: return "VoiceStick sent an invalid Ogg Opus page."
            case .failed: return "The Mac could not decode VoiceStick audio."
            }
        }
    }
}
