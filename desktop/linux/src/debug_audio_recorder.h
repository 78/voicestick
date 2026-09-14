#pragma once

#include "byte_utils.h"

#include <filesystem>
#include <optional>
#include <string>

namespace voicestick {

class DebugAudioRecorder {
public:
    DebugAudioRecorder(bool enabled, std::filesystem::path directory);

    void Start(std::optional<std::string> device_id, std::optional<std::uint32_t> session_id);
    void Append(std::span<const std::uint8_t> data);
    void Finish();
    void Discard();

private:
    void Reset();
    std::filesystem::path FilePath() const;

    bool enabled_;
    std::filesystem::path directory_;
    std::optional<std::string> current_device_id_;
    std::optional<std::uint32_t> current_session_id_;
    std::time_t current_started_at_ = 0;
    ByteVector current_audio_;
};

} // namespace voicestick
