#pragma once

#include "app_config.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <Winhttp.h>

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <vector>

namespace voicestick {

class AsrClientWin : public AsrClient {
public:
    explicit AsrClientWin(AppConfig config);
    ~AsrClientWin() override;

    bool Start(AsrSessionOptions options = {}) override;
    void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) override;
    void Cancel() override;

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
    };

    struct QueuedAudioChunk {
        ByteVector data;
        bool is_last = false;
    };

    bool StartReusableSession();
    void CancelReusableSession();
    void ShutdownReusableConnection();
    void RunReusableWebSocket();
    void FlushQueuedAudioChunks(HINTERNET websocket);
    void ReceiveOneReusable(HINTERNET websocket);
    void HandleReusableResponse(std::span<const std::uint8_t> data, HINTERNET websocket);
    void SendReusableAudio(std::span<const std::uint8_t> data, bool is_last);
    void FinishReusableSessionIfNeeded(HINTERNET websocket);
    void FailReusableSession(const std::string& message);
    bool SendReusableFrameOrFail(HINTERNET websocket, const ByteVector& frame,
                                 const std::string& context);

    static bool SendFrame(HINTERNET websocket, const ByteVector& frame);
    static void AddHeader(HINTERNET request, std::string_view name, std::string_view value);
    static std::string QueryStatusCode(HINTERNET request);
    static std::string LastErrorText();
    static std::string GenerateSessionId();
    static void CloseHandles(HINTERNET session, HINTERNET connect,
                             HINTERNET request, HINTERNET websocket);

    AppConfig config_;
    std::atomic_bool cancelled_ = false;
    std::thread worker_;
    std::mutex mutex_;
    std::vector<QueuedAudioChunk> queued_audio_chunks_;
    ConnectionState connection_state_ = ConnectionState::kDisconnected;
    SessionState session_state_ = SessionState::kIdle;
    std::string current_session_id_;
    std::string latest_session_transcript_;
    std::set<std::string> emitted_definite_segment_keys_;
    AsrSessionOptions session_options_;
    HINTERNET websocket_ = nullptr;
};

} // namespace voicestick
