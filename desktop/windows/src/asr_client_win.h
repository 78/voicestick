#pragma once

#include "app_config.h"
#include "openai_realtime_protocol.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <Winhttp.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <vector>

namespace voicestick {

class AsrClientWin : public AsrClient {
public:
    AsrClientWin(AppConfig config, std::function<void(std::function<void()>)> post_to_ui);
    ~AsrClientWin() override;

    bool Start(AsrSessionOptions options = {}) override;
    void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) override;
    void Cancel() override;
    std::string LastStartError() const override;

private:
    enum class ConnectionState {
        kDisconnected,
        kConnecting,
        kReady,
    };

    enum class SessionState {
        kIdle,
        kStarting,
        kStreaming,
        kFinishing,
        kTransforming,
    };

    struct QueuedAudioChunk {
        ByteVector data;
        bool is_last = false;
    };

    bool StartReusableSession(AsrSessionOptions options);
    bool SendStartSession();
    void CancelReusableSession();
    void ShutdownReusableConnection();
    void RunReusableWebSocket();
    void FlushQueuedAudioChunks();
    void ReceiveOneReusable(HINTERNET websocket, ByteVector* message);
    void HandleCloudTextMessage(std::string_view message);
    void RunSessionTimeouts(std::stop_token stop);
    void SetSessionTimeout(std::chrono::seconds duration);
    void MarkSessionIdle();
    void FinishCloudText(std::string_view session_id, std::optional<std::string> text, std::string error);
    void HandleOpenAIEvent(std::string_view message);
    void SendOpenAIAudio(std::span<const std::uint8_t> data, bool is_last);
    std::string SendOpenAIAudioLocked(std::span<const std::uint8_t> data, bool is_last);
    static bool SendTextFrame(HINTERNET websocket, std::string_view json);
    bool UsesOpenAI() const { return config_.asr_provider == AsrProvider::kOpenAI; }
    void HandleReusableResponse(std::span<const std::uint8_t> data, HINTERNET websocket);
    void SendReusableAudio(std::span<const std::uint8_t> data, bool is_last);
    void FinishReusableSessionIfNeeded();
    void FailReusableSession(const std::string& message, std::optional<std::string> upgrade_url = {},
                             std::string_view expected_session_id = {},
                             std::optional<SessionState> expected_state = {});
    void SetLastStartError(std::string message);
    bool SendReusableFrameOrFail(const ByteVector& frame, const std::string& context);
    void PostSessionCallback(std::uint64_t generation, std::function<void()> callback) const;
    void PostSessionError(std::uint64_t generation, std::string message, std::string partial_text,
                          std::optional<std::string> upgrade_url = {}) const;

    static bool SendFrame(HINTERNET websocket, const ByteVector& frame);
    static void AddHeader(HINTERNET request, std::string_view name, std::string_view value);
    static std::string QueryStatusCode(HINTERNET request);
    static std::string LastErrorText();
    static std::string GenerateSessionId();
    static void CloseHandles(HINTERNET session, HINTERNET connect,
                             HINTERNET request, HINTERNET websocket);

    AppConfig config_;
    std::function<void(std::function<void()>)> post_to_ui_;
    // Queued callbacks may outlive the session or the client that posted them.
    std::shared_ptr<std::atomic_uint64_t> callback_generation_ =
        std::make_shared<std::atomic_uint64_t>(0);
    std::atomic_bool cancelled_ = false;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::vector<QueuedAudioChunk> queued_audio_chunks_;
    ConnectionState connection_state_ = ConnectionState::kDisconnected;
    SessionState session_state_ = SessionState::kIdle;
    std::string current_session_id_;
    std::string latest_session_transcript_;
    std::set<std::string> emitted_definite_segment_keys_;
    AsrSessionOptions session_options_;
    bool cloud_supports_text_ = false;
    bool cloud_text_requested_ = false;
    OpenAIOpusDecoder decoder_;
    std::size_t openai_pcm_samples_ = 0;
    std::string realtime_item_;
    std::string last_realtime_item_;
    std::condition_variable_any timeout_changed_;
    std::chrono::steady_clock::time_point timeout_at_{};
    std::jthread session_timeout_worker_;
    HINTERNET websocket_ = nullptr;
    std::string last_start_error_;
};

} // namespace voicestick
