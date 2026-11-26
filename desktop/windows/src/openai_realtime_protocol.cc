#include "openai_realtime_protocol.h"

#include "cJSON.h"

#include <Windows.h>
#include <wincrypt.h>
#include <opus.h>

#include <array>
#include <string_view>

namespace voicestick {

OpenAIOpusDecoder::OpenAIOpusDecoder() : decoder_(nullptr, opus_decoder_destroy) {}
OpenAIOpusDecoder::~OpenAIOpusDecoder() = default;

bool OpenAIOpusDecoder::Reset() {
    if (!decoder_) decoder_.reset(opus_decoder_create(kOpenAIRealtimeSampleRate, 1, nullptr));
    return decoder_ && opus_decoder_ctl(decoder_.get(), OPUS_RESET_STATE) == OPUS_OK;
}

std::optional<std::vector<std::int16_t>> OpenAIOpusDecoder::Decode(std::span<const std::uint8_t> ogg) {
    if (!decoder_) return std::nullopt;
    std::vector<std::int16_t> pcm;
    // The muxer writes whole pages with at most one packet per page.
    constexpr std::size_t kPageHeaderBytes = 27;
    constexpr int kMaxOpusSamples = kOpenAIRealtimeSampleRate * 120 / 1000;
    while (!ogg.empty()) {
        if (ogg.size() < kPageHeaderBytes ||
            std::string_view(reinterpret_cast<const char*>(ogg.data()), 4) != "OggS") {
            return std::nullopt;
        }
        const auto segments = ogg[kPageHeaderBytes - 1];
        ogg = ogg.subspan(kPageHeaderBytes);
        if (segments == 0) continue;
        if (segments != 1 || ogg.empty() || ogg[0] == 0 || ogg[0] >= ogg.size()) {
            return std::nullopt;
        }
        const auto packet = ogg.subspan(1, ogg[0]);
        ogg = ogg.subspan(1 + packet.size());
        const std::string_view bytes(reinterpret_cast<const char*>(packet.data()), packet.size());
        if (bytes.starts_with("OpusHead") || bytes.starts_with("OpusTags")) continue;

        std::array<std::int16_t, kMaxOpusSamples> samples{};
        const int decoded = opus_decode(decoder_.get(), packet.data(), static_cast<opus_int32>(packet.size()),
                                        samples.data(), kMaxOpusSamples, 0);
        if (decoded < 0) return std::nullopt;
        pcm.insert(pcm.end(), samples.begin(), samples.begin() + decoded);
    }
    return pcm;
}

std::string OpenAITranscriptionSessionUpdate(const std::vector<std::string>& hotwords) {
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> message(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(message.get(), "type", "session.update");
    auto* session = cJSON_AddObjectToObject(message.get(), "session");
    cJSON_AddStringToObject(session, "type", "transcription");
    auto* input = cJSON_AddObjectToObject(cJSON_AddObjectToObject(session, "audio"), "input");
    auto* format = cJSON_AddObjectToObject(input, "format");
    cJSON_AddStringToObject(format, "type", "audio/pcm");
    cJSON_AddNumberToObject(format, "rate", kOpenAIRealtimeSampleRate);
    auto* transcription = cJSON_AddObjectToObject(input, "transcription");
    cJSON_AddStringToObject(transcription, "model", "gpt-live-transcribe");
    cJSON_AddNullToObject(input, "turn_detection");
    if (!hotwords.empty()) {
        auto* keywords = cJSON_AddArrayToObject(transcription, "keywords");
        for (const auto& word : hotwords) {
            if (!word.empty() && word.find_first_of("<>\r\n") == std::string::npos) {
                cJSON_AddItemToArray(keywords, cJSON_CreateString(word.c_str()));
            }
        }
    }
    const std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(message.get()), cJSON_free);
    return json ? json.get() : "";
}

std::string OpenAIAudioAppend(std::span<const std::int16_t> pcm) {
    const auto* data = reinterpret_cast<const BYTE*>(pcm.data());
    const auto size = static_cast<DWORD>(pcm.size_bytes());
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD encoded_size = 0;
    if (!CryptBinaryToStringA(data, size, flags, nullptr, &encoded_size)) return {};
    std::string encoded(encoded_size, '\0');
    if (!CryptBinaryToStringA(data, size, flags, encoded.data(), &encoded_size)) return {};
    encoded.resize(encoded_size);
    return "{\"type\":\"input_audio_buffer.append\",\"audio\":\"" + encoded + "\"}";
}

} // namespace voicestick
