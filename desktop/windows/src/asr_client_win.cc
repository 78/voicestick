#include "asr_client_win.h"

#include "asr_protocol.h"
#include "cJSON.h"

#include <bcrypt.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace voicestick {

namespace {

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

bool StartsWithScheme(std::string_view text, std::string_view scheme) {
    return text.size() >= scheme.size() &&
           std::equal(scheme.begin(), scheme.end(), text.begin(), [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs)) ==
                      std::tolower(static_cast<unsigned char>(rhs));
           });
}

constexpr int kAsrResolveTimeoutMs = 5000;
constexpr int kAsrConnectTimeoutMs = 5000;
constexpr int kAsrSendTimeoutMs = 5000;
constexpr int kAsrReceiveTimeoutMs = 15000;
constexpr auto kCloudIdleTimeout = std::chrono::minutes(30);

void SetAsrWinHttpTimeouts(HINTERNET handle) {
    if (!handle) return;
    WinHttpSetTimeouts(handle,
                       kAsrResolveTimeoutMs,
                       kAsrConnectTimeoutMs,
                       kAsrSendTimeoutMs,
                       kAsrReceiveTimeoutMs);
}

std::optional<std::wstring> WinHttpUrlFromWebSocketUrl(std::string_view websocket_url) {
    std::string http_url;
    if (StartsWithScheme(websocket_url, "wss://")) {
        http_url = "https://";
        http_url.append(websocket_url.substr(6));
    } else if (StartsWithScheme(websocket_url, "ws://")) {
        http_url = "http://";
        http_url.append(websocket_url.substr(5));
    } else {
        http_url = std::string(websocket_url);
    }

    auto wide = Utf16FromUtf8(http_url);
    if (wide.empty()) return std::nullopt;
    return wide;
}

} // namespace

AsrClientWin::AsrClientWin(AppConfig config, std::function<void(std::function<void()>)> post_to_ui)
    : config_(std::move(config)), post_to_ui_(std::move(post_to_ui)) {
    session_timeout_worker_ = std::jthread([this](std::stop_token stop) { RunSessionTimeouts(stop); });
}

AsrClientWin::~AsrClientWin() {
    session_timeout_worker_.request_stop();
    if (session_timeout_worker_.joinable()) session_timeout_worker_.join();
    ShutdownReusableConnection();
    callback_generation_->fetch_add(1);
}

bool AsrClientWin::Start(AsrSessionOptions options) {
    SetLastStartError({});
    if (config_.ActiveApiKey().empty()) {
        SetLastStartError(UsesOpenAI() ? "Missing OpenAI API key" : "Missing ASR API key");
        return false;
    }
    if (options.hotwords.empty()) options.hotwords = config_.asr_hotwords;
    return StartReusableSession(std::move(options));
}

bool AsrClientWin::StartReusableSession(AsrSessionOptions options) {
    bool has_ready_websocket = false;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::kIdle) {
            last_start_error_ = "ASR session already active";
            return false;
        }
        if (UsesOpenAI() && !decoder_.Reset()) {
            last_start_error_ = "Could not initialize the Opus decoder.";
            return false;
        }
        callback_generation_->fetch_add(1);
        session_options_ = std::move(options);
        openai_pcm_samples_ = 0;
        realtime_item_.clear();
        emitted_definite_segment_keys_.clear();
        cloud_text_requested_ = false;
        current_session_id_ = GenerateSessionId();
        latest_session_transcript_.clear();
        queued_audio_chunks_.clear();
        session_state_ = SessionState::kStarting;
        SetSessionTimeout(std::chrono::seconds(0));
        if (connection_state_ == ConnectionState::kReady && websocket_) {
            has_ready_websocket = true;
        }
        if (connection_state_ == ConnectionState::kConnecting && worker_.joinable()) {
            return true;
        }
    }
    if (has_ready_websocket) return SendStartSession();

    ShutdownReusableConnection();
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::kIdle) {
            current_session_id_ = GenerateSessionId();
            session_state_ = SessionState::kStarting;
        }
        connection_state_ = ConnectionState::kConnecting;
        cloud_supports_text_ = false;
        last_realtime_item_.clear();
        if (UsesOpenAI()) SetSessionTimeout(std::chrono::seconds(15));
    }
    cancelled_ = false;
    worker_ = std::thread([this] { RunReusableWebSocket(); });
    return true;
}

bool AsrClientWin::SendStartSession() {
    if (UsesOpenAI()) {
        std::string error;
        {
            std::lock_guard lock(mutex_);
            if (session_state_ != SessionState::kStarting || connection_state_ != ConnectionState::kReady) return true;
            session_state_ = SessionState::kStreaming;
            SetSessionTimeout(std::chrono::seconds(0));
            for (const auto& chunk : queued_audio_chunks_) {
                error = SendOpenAIAudioLocked(chunk.data, chunk.is_last);
                if (!error.empty()) break;
            }
            queued_audio_chunks_.clear();
        }
        if (!error.empty()) FailReusableSession(error);
        return true;
    }
    ByteVector frame;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::kStarting) return false;
        auto options = session_options_;
        if (!cloud_supports_text_) options.cloud_text_profile.reset();
        cloud_text_requested_ = options.cloud_text_profile.has_value();
        frame = AsrProtocol::MakeStartSessionFrame(config_, current_session_id_, options);
    }
    return SendReusableFrameOrFail(frame, "start ASR session");
}

void AsrClientWin::SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) {
    if (UsesOpenAI()) {
        SendOpenAIAudio(data, is_last);
    } else {
        SendReusableAudio(data, is_last);
    }
}

void AsrClientWin::Cancel() {
    CancelReusableSession();
}

std::string AsrClientWin::LastStartError() const {
    std::lock_guard lock(mutex_);
    return last_start_error_;
}

void AsrClientWin::CancelReusableSession() {
    std::lock_guard lock(mutex_);
    callback_generation_->fetch_add(1);
    queued_audio_chunks_.clear();
    latest_session_transcript_.clear();
    emitted_definite_segment_keys_.clear();
    SetSessionTimeout(std::chrono::seconds(0));
    if (UsesOpenAI() && session_state_ != SessionState::kIdle) {
        cancelled_ = true;
        connection_state_ = ConnectionState::kDisconnected;
        if (websocket_) {
            WinHttpWebSocketShutdown(websocket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        }
    } else if (session_state_ == SessionState::kStarting ||
               session_state_ == SessionState::kStreaming ||
               session_state_ == SessionState::kFinishing ||
               session_state_ == SessionState::kTransforming) {
        if (websocket_ && !current_session_id_.empty()) {
            SendFrame(websocket_, AsrProtocol::MakeCancelSessionFrame(
                config_, current_session_id_, session_options_));
        }
    }
    current_session_id_.clear();
    MarkSessionIdle();
}

void AsrClientWin::ShutdownReusableConnection() {
    cancelled_ = true;
    const bool has_worker = worker_.joinable();
    {
        std::lock_guard lock(mutex_);
        if (websocket_) {
            if (!UsesOpenAI() && connection_state_ == ConnectionState::kReady) {
                SendFrame(websocket_,
                          AsrProtocol::MakeFinishConnectionFrame(config_, session_options_));
            }
            WinHttpWebSocketClose(websocket_,
                                  WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                  nullptr,
                                  0);
            if (!has_worker) {
                WinHttpCloseHandle(websocket_);
                websocket_ = nullptr;
            }
        }
        SetSessionTimeout(std::chrono::seconds(0));
        queued_audio_chunks_.clear();
        current_session_id_.clear();
        latest_session_transcript_.clear();
        emitted_definite_segment_keys_.clear();
        session_state_ = SessionState::kIdle;
        connection_state_ = ConnectionState::kDisconnected;
    }
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
        } else {
            worker_.join();
        }
    }
}

void AsrClientWin::RunReusableWebSocket() {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    const auto url = WinHttpUrlFromWebSocketUrl(config_.ActiveWebsocketUrl());
    if (!url.has_value() || !WinHttpCrackUrl(url->c_str(), 0, 0, &components)) {
        FailReusableSession("Invalid ASR URL");
        return;
    }

    HINTERNET session = WinHttpOpen(L"VoiceStick/Windows", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        FailReusableSession("Failed to start ASR network session: " + LastErrorText());
        return;
    }
    SetAsrWinHttpTimeouts(session);
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connect) {
        CloseHandles(session, nullptr, nullptr, nullptr);
        FailReusableSession("Failed to connect ASR host: " + LastErrorText());
        return;
    }
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    std::wstring path_and_query;
    if (components.lpszUrlPath && components.dwUrlPathLength > 0) {
        path_and_query.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
        path_and_query.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path_and_query.empty()) path_and_query = L"/";
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path_and_query.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        CloseHandles(session, connect, request, nullptr);
        FailReusableSession("Failed to create ASR request: " + LastErrorText());
        return;
    }
    SetAsrWinHttpTimeouts(request);

    if (UsesOpenAI()) {
        AddHeader(request, "Authorization", "Bearer " + config_.ActiveApiKey());
    } else {
        AddHeader(request, "X-Api-Key", config_.ActiveApiKey());
        AddHeader(request, "X-Api-Request-Id", GenerateSessionId());
        AddHeader(request, "X-Api-Sequence", "-1");
        AddHeader(request, "X-VoiceStick-Text", "1");
        if (!config_.paired_device_ids.empty()) {
            AddHeader(request, "X-Device-Id", config_.paired_device_ids.front());
        }
    }

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        CloseHandles(session, connect, request, nullptr);
        FailReusableSession("Failed to prepare ASR WebSocket upgrade");
        return;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        CloseHandles(session, connect, request, nullptr);
        FailReusableSession("ASR WebSocket handshake failed");
        return;
    }

    HINTERNET websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!websocket) {
        const auto status_code = QueryStatusCode(request);
        CloseHandles(session, connect, request, nullptr);
        FailReusableSession(status_code.empty()
                            ? "ASR WebSocket upgrade failed"
                            : "ASR WebSocket upgrade failed: HTTP " + status_code);
        return;
    }
    SetAsrWinHttpTimeouts(websocket);
    WinHttpCloseHandle(request);
    request = nullptr;

    AsrSessionOptions connection_options;
    bool should_open;
    {
        std::lock_guard lock(mutex_);
        should_open = !cancelled_;
        if (should_open) {
            websocket_ = websocket;
            connection_options = session_options_;
        }
    }
    if (should_open && UsesOpenAI()) {
        if (!SendTextFrame(websocket, OpenAITranscriptionSessionUpdate(connection_options.hotwords))) {
            FailReusableSession("Could not configure the OpenAI Realtime session.");
        }
    } else if (should_open) {
        connection_options.cloud_text_profile.reset();
        SendReusableFrameOrFail(AsrProtocol::MakeStartConnectionFrame(config_, connection_options),
                                "start ASR connection");
    }
    ByteVector message;
    while (!cancelled_) {
        ReceiveOneReusable(websocket, &message);
    }
    {
        std::lock_guard lock(mutex_);
        if (websocket_ == websocket) websocket_ = nullptr;
        connection_state_ = ConnectionState::kDisconnected;
    }
    CloseHandles(session, connect, request, websocket);
}

void AsrClientWin::FlushQueuedAudioChunks() {
    std::vector<QueuedAudioChunk> chunks;
    std::string session_id;
    {
        std::lock_guard lock(mutex_);
        chunks.swap(queued_audio_chunks_);
        session_id = current_session_id_;
    }
    for (const auto& chunk : chunks) {
        if (!SendReusableFrameOrFail(AsrProtocol::MakeTaskRequestFrame(chunk.data, session_id),
                                     "send ASR audio")) {
            return;
        }
        if (chunk.is_last) {
            FinishReusableSessionIfNeeded();
        }
    }
}

bool AsrClientWin::SendFrame(HINTERNET websocket, const ByteVector& frame) {
    return WinHttpWebSocketSend(websocket,
                                WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                const_cast<std::uint8_t*>(frame.data()),
                                static_cast<DWORD>(frame.size())) == ERROR_SUCCESS;
}

void AsrClientWin::ReceiveOneReusable(HINTERNET websocket, ByteVector* message) {
    std::array<std::uint8_t, 64 * 1024> buffer{};
    DWORD bytes_read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
    const DWORD result = WinHttpWebSocketReceive(websocket, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()),
                                                 &bytes_read, &type);
    if (result == ERROR_WINHTTP_TIMEOUT) return;
    if (result != ERROR_SUCCESS) {
        if (cancelled_.load()) return;
        FailReusableSession("ASR WebSocket receive failed: " + std::to_string(result));
        return;
    }
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        if (cancelled_.load()) return;
        FailReusableSession("ASR WebSocket disconnected");
        return;
    }
    constexpr std::size_t kMaxMessageBytes = 1024 * 1024;
    if (message->size() + bytes_read > kMaxMessageBytes) {
        FailReusableSession("ASR WebSocket message is too large");
        return;
    }
    message->insert(message->end(), buffer.begin(), buffer.begin() + bytes_read);
    if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
        type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
        return;
    }
    if (!message->empty()) {
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            const std::string_view json(reinterpret_cast<const char*>(message->data()), message->size());
            if (UsesOpenAI()) HandleOpenAIEvent(json);
            else HandleCloudTextMessage(json);
        } else if (!UsesOpenAI()) {
            HandleReusableResponse(*message, websocket);
        }
    }
    message->clear();
}

void AsrClientWin::HandleCloudTextMessage(std::string_view message) {
    if (config_.asr_provider != AsrProvider::kVoiceStickCloud) return;
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLength(message.data(), message.size()), cJSON_Delete);
    const auto* type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json.get(), "type"));
    if (!type) return;
    if (std::string_view(type) == "voicestick.text.ready") {
        std::lock_guard lock(mutex_);
        if (connection_state_ == ConnectionState::kConnecting) cloud_supports_text_ = true;
        return;
    }
    if (std::string_view(type) != "voicestick.text.result") return;
    const auto* session_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json.get(), "session_id"));
    if (!session_id) return;
    const auto* text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json.get(), "text"));
    const auto* error = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json.get(), "error"));
    FinishCloudText(session_id, text ? std::optional<std::string>(text) : std::nullopt,
                    error ? error : "VoiceStick Cloud translation failed.");
}

void AsrClientWin::SetSessionTimeout(std::chrono::seconds duration) {
    timeout_at_ = duration.count() == 0 ? std::chrono::steady_clock::time_point{}
                                        : std::chrono::steady_clock::now() + duration;
    timeout_changed_.notify_one();
}

void AsrClientWin::MarkSessionIdle() {
    session_state_ = SessionState::kIdle;
    if (!UsesOpenAI() && connection_state_ == ConnectionState::kReady) {
        SetSessionTimeout(kCloudIdleTimeout);
    }
}

void AsrClientWin::RunSessionTimeouts(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        if (timeout_at_ == std::chrono::steady_clock::time_point{}) {
            timeout_changed_.wait(lock, stop, [this] {
                return timeout_at_ != std::chrono::steady_clock::time_point{};
            });
            continue;
        }
        const auto deadline = timeout_at_;
        if (timeout_changed_.wait_until(lock, stop, deadline, [this, deadline] {
                return deadline != timeout_at_;
            }) || stop.stop_requested()) {
            continue;
        }
        const auto state = session_state_;
        const auto session_id = current_session_id_;
        SetSessionTimeout(std::chrono::seconds(0));
        if (state == SessionState::kIdle && !UsesOpenAI() &&
            connection_state_ == ConnectionState::kReady && websocket_) {
            // WebSocket receive can block independently of the HTTP receive timeout.
            cancelled_ = true;
            connection_state_ = ConnectionState::kDisconnected;
            WinHttpWebSocketClose(websocket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            websocket_ = nullptr;
            continue;
        }
        lock.unlock();
        if (state == SessionState::kTransforming) {
            FinishCloudText(session_id, std::nullopt, "VoiceStick Cloud text processing timed out.");
        } else if (!session_id.empty()) {
            FailReusableSession(state == SessionState::kStarting
                                    ? "OpenAI Realtime session could not start in time."
                                    : "OpenAI Realtime did not return a final transcript in time.",
                                {}, session_id, state);
        }
        lock.lock();
    }
}

void AsrClientWin::FinishCloudText(std::string_view session_id, std::optional<std::string> text, std::string error) {
    std::string raw;
    bool refine;
    std::uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::kTransforming || session_id != current_session_id_) return;
        SetSessionTimeout(std::chrono::seconds(0));
        refine = session_options_.cloud_text_profile->transform == TextTransform::kRefine;
        raw = std::exchange(latest_session_transcript_, {});
        generation = callback_generation_->load();
        current_session_id_.clear();
        emitted_definite_segment_keys_.clear();
        queued_audio_chunks_.clear();
        MarkSessionIdle();
    }
    if (text && (!text->empty() || raw.empty())) {
        raw = std::move(*text);
    } else if (!refine && !raw.empty()) {
        PostSessionError(generation, std::move(error), {});
        return;
    }
    PostSessionCallback(generation, [final = on_final, output = std::move(raw)] {
        if (final) final({output, true});
    });
}

void AsrClientWin::HandleOpenAIEvent(std::string_view message) {
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLength(message.data(), message.size()), cJSON_Delete);
    const auto field = [](const cJSON* object, const char* key) -> std::string_view {
        const auto* value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
        return value ? value : "";
    };
    const auto type = field(json.get(), "type");
    if (type == "session.updated") {
        {
            std::lock_guard lock(mutex_);
            if (cancelled_ || connection_state_ != ConnectionState::kConnecting) return;
            connection_state_ = ConnectionState::kReady;
        }
        SendStartSession();
        return;
    }
    if (type == "error") {
        const auto error = field(cJSON_GetObjectItemCaseSensitive(json.get(), "error"), "message");
        const auto key = config_.ActiveApiKey();
        FailReusableSession(error.empty() || (!key.empty() && error.find(key) != std::string_view::npos)
                                ? "OpenAI Realtime transcription failed." : std::string(error));
        return;
    }

    const auto item = field(json.get(), "item_id");
    if (item.empty()) return;
    std::string text;
    std::string session_id;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if ((session_state_ != SessionState::kStreaming && session_state_ != SessionState::kFinishing) ||
            item == last_realtime_item_ || (!realtime_item_.empty() && item != realtime_item_)) {
            return;
        }

        if (type == "input_audio_buffer.committed") {
            if (session_state_ == SessionState::kFinishing) realtime_item_ = item;
            return;
        }
        if (type == "conversation.item.input_audio_transcription.delta") {
            const auto delta = field(json.get(), "delta");
            if (delta.empty()) return;
            realtime_item_ = item;
            latest_session_transcript_.append(delta);
            text = latest_session_transcript_;
        } else if (type == "conversation.item.input_audio_transcription.completed" &&
                   session_state_ == SessionState::kFinishing) {
            const auto transcript = field(json.get(), "transcript");
            text = transcript.empty() ? latest_session_transcript_ : std::string(transcript);
            const auto first = text.find_first_not_of(" \r\n\t");
            const auto last = text.find_last_not_of(" \r\n\t");
            text = first == std::string::npos ? "" : text.substr(first, last - first + 1);
            last_realtime_item_ = item;
            latest_session_transcript_.clear();
            current_session_id_.clear();
            session_state_ = SessionState::kIdle;
            SetSessionTimeout(std::chrono::seconds(0));
        } else if (type == "conversation.item.input_audio_transcription.failed") {
            session_id = current_session_id_;
        } else {
            return;
        }
        generation = callback_generation_->load();
    }
    if (!session_id.empty()) {
        const auto error = field(cJSON_GetObjectItemCaseSensitive(json.get(), "error"), "message");
        const auto key = config_.ActiveApiKey();
        FailReusableSession(error.empty() || (!key.empty() && error.find(key) != std::string_view::npos)
                                ? "OpenAI Realtime transcription failed." : std::string(error), {}, session_id);
    } else if (type == "conversation.item.input_audio_transcription.delta") {
        PostSessionCallback(generation, [partial = on_partial, text = std::move(text)] {
            if (partial) partial(text);
        });
    } else {
        PostSessionCallback(generation, [final = on_final, text = std::move(text)] {
            if (final) final({text});
        });
    }
}

void AsrClientWin::SendOpenAIAudio(std::span<const std::uint8_t> data, bool is_last) {
    std::string error;
    std::string session_id;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::kStarting) {
            queued_audio_chunks_.push_back({ByteVector(data.begin(), data.end()), is_last});
            return;
        }
        if (session_state_ != SessionState::kStreaming) return;
        session_id = current_session_id_;
        error = SendOpenAIAudioLocked(data, is_last);
    }
    if (!error.empty()) FailReusableSession(error, {}, session_id);
}

std::string AsrClientWin::SendOpenAIAudioLocked(std::span<const std::uint8_t> data, bool is_last) {
    const auto pcm = decoder_.Decode(data);
    if (!pcm) return "Could not decode VoiceStick Opus audio.";
    if (!pcm->empty()) {
        const auto append = OpenAIAudioAppend(*pcm);
        if (!SendTextFrame(websocket_, append)) return "Could not send audio to OpenAI Realtime.";
        openai_pcm_samples_ += pcm->size();
    }
    if (is_last) {
        if (openai_pcm_samples_ < kOpenAIRealtimeSampleRate / 10) {
            return "OpenAI Realtime needs at least 100 ms of audio.";
        }
        if (!SendTextFrame(websocket_, R"({"type":"input_audio_buffer.commit"})")) {
            return "Could not commit audio to OpenAI Realtime.";
        }
        session_state_ = SessionState::kFinishing;
        SetSessionTimeout(std::chrono::seconds(20));
    }
    return {};
}

bool AsrClientWin::SendTextFrame(HINTERNET websocket, std::string_view json) {
    return websocket && !json.empty() &&
           WinHttpWebSocketSend(websocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                const_cast<char*>(json.data()), static_cast<DWORD>(json.size())) == ERROR_SUCCESS;
}

void AsrClientWin::HandleReusableResponse(std::span<const std::uint8_t> data, HINTERNET websocket) {
    if (data.size() < 4) {
        FailReusableSession("Short ASR response");
        return;
    }
    const std::uint8_t message_type = data[1] >> 4;
    if (message_type == 0x0f) {
        auto response = AsrProtocol::ParseResponse(data);
        if (response && response->is_error) {
            FailReusableSession(response->text, response->upgrade_url);
        }
        return;
    }

    auto event_response = AsrProtocol::ParseEventResponse(data);
    if (!event_response.has_value()) {
        return;
    }

    const auto& response = *event_response;
    switch (response.event.value_or(static_cast<AsrEvent>(0))) {
    case AsrEvent::kConnectionStarted: {
        {
            std::lock_guard lock(mutex_);
            connection_state_ = ConnectionState::kReady;
            if (session_state_ == SessionState::kIdle) MarkSessionIdle();
        }
        SendStartSession();
        break;
    }

    case AsrEvent::kConnectionFailed:
        FailReusableSession(response.payload_text.empty() ? "ASR connection failed" : response.payload_text);
        break;

    case AsrEvent::kConnectionFinished:
        {
            std::lock_guard lock(mutex_);
            connection_state_ = ConnectionState::kDisconnected;
            if (websocket_ == websocket) websocket_ = nullptr;
        }
        cancelled_ = true;
        break;

    case AsrEvent::kSessionStarted: {
        bool should_flush = false;
        {
            std::lock_guard lock(mutex_);
            if (session_state_ == SessionState::kStarting && response.session_id == current_session_id_) {
                session_state_ = SessionState::kStreaming;
                should_flush = true;
            }
        }
        if (should_flush) FlushQueuedAudioChunks();
        break;
    }

    case AsrEvent::kAsrResponse:
    case AsrEvent::kAsrInfo: {
        std::string transcript;
        std::vector<AsrSegment> segments;
        std::uint64_t generation;
        {
            std::lock_guard lock(mutex_);
            if (response.session_id != current_session_id_ || session_state_ == SessionState::kTransforming) return;
            generation = callback_generation_->load();
            transcript = AsrProtocol::ExtractTranscript(response.payload_text);
            if (!transcript.empty()) latest_session_transcript_ = transcript;
            segments = AsrProtocol::ExtractNewDefiniteSegments(
                response.payload_text, &emitted_definite_segment_keys_);
        }
        if (!transcript.empty() || !segments.empty()) {
            PostSessionCallback(generation, [partial = on_partial, segment_callback = on_segment,
                transcript = std::move(transcript), segments = std::move(segments)] {
                if (!transcript.empty() && partial) partial(transcript);
                if (segment_callback) {
                    for (const auto& segment : segments) segment_callback(segment);
                }
            });
        }
        break;
    }

    case AsrEvent::kSessionFinished: {
        std::string final_text;
        bool wait_for_cloud_text;
        std::vector<AsrSegment> segments;
        std::uint64_t generation;
        {
            std::lock_guard lock(mutex_);
            if (response.session_id != current_session_id_ || session_state_ == SessionState::kTransforming) return;
            generation = callback_generation_->load();
            final_text = AsrProtocol::ExtractTranscript(response.payload_text);
            if (final_text.empty()) final_text = latest_session_transcript_;
            wait_for_cloud_text = cloud_text_requested_ && !final_text.empty();
            if (wait_for_cloud_text) {
                latest_session_transcript_ = final_text;
                session_state_ = SessionState::kTransforming;
                SetSessionTimeout(std::chrono::seconds(8));
            } else {
                segments = AsrProtocol::ExtractNewDefiniteSegments(
                    response.payload_text, &emitted_definite_segment_keys_);
                current_session_id_.clear();
                latest_session_transcript_.clear();
                emitted_definite_segment_keys_.clear();
                queued_audio_chunks_.clear();
                MarkSessionIdle();
            }
        }
        if (wait_for_cloud_text) {
            PostSessionCallback(generation, [partial = on_partial, raw = std::move(final_text)] {
                if (partial) partial(raw);
            });
            break;
        }
        PostSessionCallback(generation, [final = on_final, segment_callback = on_segment,
            final_text = std::move(final_text), segments = std::move(segments)] {
            if (segment_callback) {
                for (const auto& segment : segments) segment_callback(segment);
            }
            if (final) final({final_text});
        });
        break;
    }

    case AsrEvent::kSessionCanceled:
        {
            std::lock_guard lock(mutex_);
            if (response.session_id == current_session_id_) {
                current_session_id_.clear();
                latest_session_transcript_.clear();
                emitted_definite_segment_keys_.clear();
                queued_audio_chunks_.clear();
                MarkSessionIdle();
            }
        }
        break;

    case AsrEvent::kAsrEnd:
    case AsrEvent::kUsageResponse:
        break;

    default:
        break;
    }
}

void AsrClientWin::SendReusableAudio(std::span<const std::uint8_t> data, bool is_last) {
    std::string session_id;
    SessionState state = SessionState::kIdle;
    {
        std::lock_guard lock(mutex_);
        state = session_state_;
        if (state == SessionState::kStarting || state == SessionState::kIdle) {
            queued_audio_chunks_.push_back(QueuedAudioChunk{ByteVector(data.begin(), data.end()), is_last});
            return;
        }
        if (state != SessionState::kStreaming) return;
        session_id = current_session_id_;
    }
    if (session_id.empty()) {
        FailReusableSession("ASR WebSocket is not connected");
        return;
    }
    if (!SendReusableFrameOrFail(AsrProtocol::MakeTaskRequestFrame(data, session_id),
                                 "send ASR audio")) {
        return;
    }
    if (is_last) {
        FinishReusableSessionIfNeeded();
    }
}

void AsrClientWin::FinishReusableSessionIfNeeded() {
    std::string session_id;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::kStarting) {
            const auto has_last = std::any_of(
                queued_audio_chunks_.begin(), queued_audio_chunks_.end(),
                [](const QueuedAudioChunk& chunk) { return chunk.is_last; });
            if (!has_last) queued_audio_chunks_.push_back(QueuedAudioChunk{{}, true});
            return;
        }
        if (session_state_ != SessionState::kStreaming || current_session_id_.empty()) return;
        session_state_ = SessionState::kFinishing;
        session_id = current_session_id_;
    }
    SendReusableFrameOrFail(AsrProtocol::MakeFinishSessionFrame(config_, session_id, session_options_),
                            "finish ASR session");
}

void AsrClientWin::FailReusableSession(const std::string& message, std::optional<std::string> upgrade_url,
                                       std::string_view expected_session_id,
                                       std::optional<SessionState> expected_state) {
    bool had_active_session = false;
    std::string partial_text;
    std::uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        if ((!expected_session_id.empty() && expected_session_id != current_session_id_) ||
            (expected_state && *expected_state != session_state_)) {
            return;
        }
        if (cancelled_.exchange(true)) return;
        SetSessionTimeout(std::chrono::seconds(0));
        if (UsesOpenAI() && websocket_) {
            WinHttpWebSocketShutdown(websocket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        }
        had_active_session = session_state_ != SessionState::kIdle;
        queued_audio_chunks_.clear();
        current_session_id_.clear();
        partial_text = std::exchange(latest_session_transcript_, {});
        generation = callback_generation_->load();
        session_state_ = SessionState::kIdle;
        connection_state_ = ConnectionState::kDisconnected;
        websocket_ = nullptr;
    }
    if (had_active_session) {
        PostSessionError(generation, message, std::move(partial_text), std::move(upgrade_url));
    }
}

bool AsrClientWin::SendReusableFrameOrFail(const ByteVector& frame, const std::string& context) {
    const auto message = "Failed to " + context;
    bool should_notify = false;
    std::string partial_text;
    std::uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        if (websocket_ && SendFrame(websocket_, frame)) return true;

        const bool was_cancelled = cancelled_.exchange(true);
        const bool had_active_session = session_state_ != SessionState::kIdle;
        last_start_error_ = message;
        queued_audio_chunks_.clear();
        current_session_id_.clear();
        partial_text = std::exchange(latest_session_transcript_, {});
        generation = callback_generation_->load();
        session_state_ = SessionState::kIdle;
        connection_state_ = ConnectionState::kDisconnected;
        websocket_ = nullptr;
        should_notify = !was_cancelled && had_active_session;
    }
    if (should_notify) PostSessionError(generation, message, std::move(partial_text));
    return false;
}

void AsrClientWin::PostSessionCallback(std::uint64_t generation, std::function<void()> callback) const {
    post_to_ui_([current = callback_generation_, generation, callback = std::move(callback)] {
        if (current->load() == generation) callback();
    });
}

void AsrClientWin::PostSessionError(std::uint64_t generation, std::string message,
                                   std::string partial_text, std::optional<std::string> upgrade_url) const {
    PostSessionCallback(generation, [error = on_error, upgrade = on_upgrade_url, message = std::move(message),
        partial_text = std::move(partial_text), upgrade_url = std::move(upgrade_url)]() mutable {
        if (error) error(message, std::move(partial_text));
        if (upgrade && upgrade_url) upgrade(*upgrade_url, message);
    });
}

void AsrClientWin::SetLastStartError(std::string message) {
    std::lock_guard lock(mutex_);
    last_start_error_ = std::move(message);
}

void AsrClientWin::AddHeader(HINTERNET request, std::string_view name, std::string_view value) {
    const auto header = Utf16FromUtf8(std::string(name) + ": " + std::string(value) + "\r\n");
    WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(header.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
}

std::string AsrClientWin::QueryStatusCode(HINTERNET request) {
    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status_code,
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    return std::to_string(status_code);
}

std::string AsrClientWin::LastErrorText() {
    return std::to_string(GetLastError());
}

std::string AsrClientWin::GenerateSessionId() {
    std::array<std::uint8_t, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        static std::atomic_uint counter = 0;
        const auto value = ++counter;
        char fallback[37]{};
        snprintf(fallback, sizeof(fallback), "voice-stick-windows-%08x", value);
        return fallback;
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    char out[37]{};
    snprintf(out, sizeof(out),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return out;
}

void AsrClientWin::CloseHandles(HINTERNET session, HINTERNET connect,
                                HINTERNET request, HINTERNET websocket) {
    if (websocket) WinHttpCloseHandle(websocket);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
}

} // namespace voicestick
