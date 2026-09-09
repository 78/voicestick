#include "debug_audio_recorder.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace voicestick {

DebugAudioRecorder::DebugAudioRecorder(bool enabled, std::filesystem::path directory)
    : enabled_(enabled), directory_(std::move(directory)) {}

void DebugAudioRecorder::Start(std::optional<std::string> device_id, std::optional<std::uint32_t> session_id) {
    if (!enabled_) return;
    current_device_id_ = std::move(device_id);
    current_session_id_ = session_id;
    current_started_at_ = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    current_audio_.clear();
}

void DebugAudioRecorder::Append(std::span<const std::uint8_t> data) {
    if (!enabled_ || data.empty()) return;
    current_audio_.insert(current_audio_.end(), data.begin(), data.end());
}

void DebugAudioRecorder::Finish() {
    if (!enabled_ || current_audio_.empty()) {
        Reset();
        return;
    }
    std::filesystem::create_directories(directory_);
    std::ofstream output(FilePath(), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(current_audio_.data()), static_cast<std::streamsize>(current_audio_.size()));
    Reset();
}

void DebugAudioRecorder::Discard() {
    Reset();
}

void DebugAudioRecorder::Reset() {
    current_device_id_.reset();
    current_session_id_.reset();
    current_started_at_ = 0;
    current_audio_.clear();
}

std::filesystem::path DebugAudioRecorder::FilePath() const {
    std::tm local_time{};
    localtime_s(&local_time, &current_started_at_);
    std::ostringstream name;
    name << std::put_time(&local_time, "%Y%m%d-%H%M%S");
    name << "-VS-" << current_device_id_.value_or("unknown-device");
    name << "-session-" << (current_session_id_ ? std::to_string(*current_session_id_) : "unknown");
    name << ".ogg";
    return directory_ / name.str();
}

} // namespace voicestick
