#include "ogg_opus_muxer.h"
#include "ogg_crc.h"

#include <algorithm>
#include <stdexcept>

namespace voicestick {

OggOpusMuxer::OggOpusMuxer(int sample_rate, int channels)
    : sample_rate_(sample_rate), channels_(channels) {}

void OggOpusMuxer::Reset() {
    wrote_headers_ = false;
    sequence_ = 0;
    granule_position_ = 0;
}

ByteVector OggOpusMuxer::Append(std::span<const std::uint8_t> opus_payload, bool is_last) {
    if (opus_payload.empty()) {
        throw std::invalid_argument("empty Opus payloads must use Finish()");
    }
    ByteVector out;
    if (!wrote_headers_) {
        auto head = MakePage(OpusHead(), 0, 0x02);
        auto tags = MakePage(OpusTags(), 0, 0x00);
        out.insert(out.end(), head.begin(), head.end());
        out.insert(out.end(), tags.begin(), tags.end());
        wrote_headers_ = true;
    }
    granule_position_ += static_cast<std::uint64_t>(960 * 48000 / sample_rate_);
    auto page = MakePage(opus_payload, granule_position_, is_last ? 0x04 : 0x00);
    out.insert(out.end(), page.begin(), page.end());
    return out;
}

ByteVector OggOpusMuxer::Finish() {
    ByteVector out;
    if (!wrote_headers_) {
        auto head = MakePage(OpusHead(), 0, 0x02);
        auto tags = MakePage(OpusTags(), 0, 0x00);
        out.insert(out.end(), head.begin(), head.end());
        out.insert(out.end(), tags.begin(), tags.end());
        wrote_headers_ = true;
    }
    auto page = MakeEmptyPage(granule_position_, 0x04);
    out.insert(out.end(), page.begin(), page.end());
    return out;
}

ByteVector OggOpusMuxer::OpusHead() const {
    ByteVector data;
    AppendAscii(data, "OpusHead");
    data.push_back(1);
    data.push_back(static_cast<std::uint8_t>(channels_));
    AppendLe16(data, 312);
    AppendLe32(data, static_cast<std::uint32_t>(sample_rate_));
    AppendLe16(data, 0);
    data.push_back(0);
    return data;
}

ByteVector OggOpusMuxer::OpusTags() {
    ByteVector data;
    AppendAscii(data, "OpusTags");
    AppendLe32(data, 10);
    AppendAscii(data, "VoiceStick");
    AppendLe32(data, 0);
    return data;
}

ByteVector OggOpusMuxer::MakePage(std::span<const std::uint8_t> packet, std::uint64_t granule, std::uint8_t header_type) {
    if (packet.size() > 255) {
        throw std::invalid_argument("v1 muxer expects one lacing segment per packet");
    }
    ByteVector page;
    AppendAscii(page, "OggS");
    page.push_back(0);
    page.push_back(header_type);
    AppendLe64(page, granule);
    AppendLe32(page, serial_);
    AppendLe32(page, sequence_);
    AppendLe32(page, 0);
    page.push_back(1);
    page.push_back(static_cast<std::uint8_t>(packet.size()));
    page.insert(page.end(), packet.begin(), packet.end());
    const auto crc = OggCRC::Checksum(page);
    page[22] = static_cast<std::uint8_t>(crc & 0xff);
    page[23] = static_cast<std::uint8_t>((crc >> 8) & 0xff);
    page[24] = static_cast<std::uint8_t>((crc >> 16) & 0xff);
    page[25] = static_cast<std::uint8_t>((crc >> 24) & 0xff);
    ++sequence_;
    return page;
}

ByteVector OggOpusMuxer::MakeEmptyPage(std::uint64_t granule, std::uint8_t header_type) {
    ByteVector page;
    AppendAscii(page, "OggS");
    page.push_back(0);
    page.push_back(header_type);
    AppendLe64(page, granule);
    AppendLe32(page, serial_);
    AppendLe32(page, sequence_);
    AppendLe32(page, 0);
    page.push_back(0);
    const auto crc = OggCRC::Checksum(page);
    page[22] = static_cast<std::uint8_t>(crc & 0xff);
    page[23] = static_cast<std::uint8_t>((crc >> 8) & 0xff);
    page[24] = static_cast<std::uint8_t>((crc >> 16) & 0xff);
    page[25] = static_cast<std::uint8_t>((crc >> 24) & 0xff);
    ++sequence_;
    return page;
}

} // namespace voicestick
