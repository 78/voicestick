#pragma once

#include "app_config.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <Winhttp.h>

#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace voicestick {

class OpenAITranscriptionClient : public AsrClient {
public:
    explicit OpenAITranscriptionClient(AppConfig config);
    ~OpenAITranscriptionClient() override;

    bool Start(AsrSessionOptions options = {}) override;
    void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) override;
    void Cancel() override;
    std::string LastStartError() const override;

private:
    void UploadTranscription(const std::vector<std::uint8_t>& audio, std::vector<std::string> hotwords);
    void FailSession(const std::string& message);
    void NotifyError(const std::string& message);
    std::string TranscriptionPathAndQuery(std::wstring* host, INTERNET_PORT* port, bool* secure, std::string* error) const;
    static std::string MakeMultipartBody(std::string_view boundary, std::span<const std::uint8_t> audio, const std::vector<std::string>& hotwords);
    static std::string ExtractTranscript(std::string_view json_response);
    static std::string ExtractErrorMessage(std::string_view json_response);
    static std::string LastErrorText();

    AppConfig config_;
    AsrSessionOptions session_options_;
    std::vector<std::uint8_t> buffered_audio_;
    bool started_ = false;
    std::atomic_bool cancelled_ = false;
    mutable std::mutex mutex_;
    std::string last_start_error_;
    std::thread worker_;
};

} // namespace voicestick
