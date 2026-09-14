#pragma once

#include "byte_utils.h"

namespace voicestick {

class OggOpusMuxer {
public:
    OggOpusMuxer(int sample_rate, int channels);

    void Reset();
    ByteVector Append(std::span<const std::uint8_t> opus_payload, bool is_last);
    ByteVector Finish();

private:
    ByteVector OpusHead() const;
    static ByteVector OpusTags();
    ByteVector MakePage(std::span<const std::uint8_t> packet, std::uint64_t granule, std::uint8_t header_type);
    ByteVector MakeEmptyPage(std::uint64_t granule, std::uint8_t header_type);

    int sample_rate_;
    int channels_;
    bool wrote_headers_ = false;
    std::uint32_t sequence_ = 0;
    std::uint64_t granule_position_ = 0;
    static constexpr std::uint32_t serial_ = 0x5653544b;
};

} // namespace voicestick
