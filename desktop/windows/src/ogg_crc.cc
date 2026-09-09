#include "ogg_crc.h"

#include <array>

namespace voicestick {

std::uint32_t OggCRC::Checksum(std::span<const std::uint8_t> data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> out{};
        for (std::uint32_t i = 0; i < out.size(); ++i) {
            std::uint32_t r = i << 24;
            for (int bit = 0; bit < 8; ++bit) {
                r = (r & 0x80000000u) ? ((r << 1) ^ 0x04c11db7u) : (r << 1);
            }
            out[i] = r;
        }
        return out;
    }();

    std::uint32_t crc = 0;
    for (auto byte : data) {
        crc = (crc << 8) ^ table[((crc >> 24) & 0xffu) ^ byte];
    }
    return crc;
}

} // namespace voicestick
