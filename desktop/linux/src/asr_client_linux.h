#pragma once

#include "app_config.h"
#include "voice_stick_coordinator.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace voicestick {

class AsrClientLinux : public AsrClient {
public:
    explicit AsrClientLinux(AppConfig config);
    ~AsrClientLinux() override;

    bool Start(AsrSessionOptions options = {}) override;
    void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) override;
    void Cancel() override;
    std::string LastStartError() const override;

private:
    struct QueuedChunk {
        ByteVector data;
        bool is_last = false;
    };

    void RunLoop();
    void Fail(const std::string& message);
    void SetLastStartError(std::string message);
    static std::string GenerateSessionId();

    AppConfig config_;
    AsrSessionOptions session_options_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic_bool stop_{false};
    std::string last_start_error_;
    std::string current_session_id_;
    std::string latest_transcript_;
    std::set<std::string> emitted_segment_keys_;
    std::vector<QueuedChunk> queued_;
    bool session_active_ = false;
};

} // namespace voicestick
