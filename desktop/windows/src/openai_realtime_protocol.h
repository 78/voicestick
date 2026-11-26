#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct OpusDecoder;

namespace voicestick {

inline constexpr int kOpenAIRealtimeSampleRate = 24000;

class OpenAIOpusDecoder {
public:
    OpenAIOpusDecoder();
    ~OpenAIOpusDecoder();
    bool Reset();
    std::optional<std::vector<std::int16_t>> Decode(std::span<const std::uint8_t> ogg);

private:
    std::unique_ptr<OpusDecoder, void (*)(OpusDecoder*)> decoder_;
};

std::string OpenAITranscriptionSessionUpdate(const std::vector<std::string>& hotwords);
std::string OpenAIAudioAppend(std::span<const std::int16_t> pcm);

} // namespace voicestick
