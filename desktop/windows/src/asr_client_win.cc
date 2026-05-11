#include "asr_client_win.h"

#include "asr_protocol.h"

#include <bcrypt.h>

#include <algorithm>
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

AsrClientWin::AsrClientWin(AppConfig config) : config_(std::move(config)) {}

AsrClientWin::~AsrClientWin() {
    ShutdownReusableConnection();
}

bool AsrClientWin::Start(AsrSessionOptions options) {
    if (config_.ActiveApiKey().empty()) {
        if (on_error) on_error("Missing ASR API key");
        return false;
    }
    session_options_ = std::move(options);
    if (session_options_.hotwords.empty()) {
        session_options_.hotwords = config_.asr_hotwords;
    }
    emitted_definite_segment_keys_.clear();
    return StartReusableSession();
}

bool AsrClientWin::StartReusableSession() {
    HINTERNET ready_websocket = nullptr;
    std::string ready_session_id;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::kIdle) {
            if (on_error) on_error("ASR session already active");
            return false;
        }
        current_session_id_ = GenerateSessionId();
        latest_session_transcript_.clear();
        queued_audio_chunks_.clear();
        session_state_ = SessionState::kStarting;
        if (connection_state_ == ConnectionState::kReady && websocket_) {
            ready_websocket = websocket_;
            ready_session_id = current_session_id_;
        }
        if (connection_state_ == ConnectionState::kConnecting && worker_.joinable()) {
            return true;
        }
    }
    if (ready_websocket) {
        return SendReusableFrameOrFail(
            ready_websocket,
            AsrProtocol::MakeStartSessionFrame(config_, ready_session_id, session_options_),
            "start ASR session");
    }

    ShutdownReusableConnection();
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::kIdle) {
            current_session_id_ = GenerateSessionId();
            session_state_ = SessionState::kStarting;
        }
        connection_state_ = ConnectionState::kConnecting;
    }
    cancelled_ = false;
    worker_ = std::thread([this] { RunReusableWebSocket(); });
    return true;
}

void AsrClientWin::SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) {
    SendReusableAudio(data, is_last);
}

void AsrClientWin::Cancel() {
    CancelReusableSession();
}

void AsrClientWin::CancelReusableSession() {
    std::lock_guard lock(mutex_);
    queued_audio_chunks_.clear();
    latest_session_transcript_.clear();
    emitted_definite_segment_keys_.clear();
    if (session_state_ == SessionState::kStarting ||
        session_state_ == SessionState::kStreaming ||
        session_state_ == SessionState::kFinishing) {
        if (websocket_ && !current_session_id_.empty()) {
            SendFrame(websocket_, AsrProtocol::MakeCancelSessionFrame(
                config_, current_session_id_, session_options_));
        }
    }
    current_session_id_.clear();
    session_state_ = SessionState::kIdle;
}

void AsrClientWin::ShutdownReusableConnection() {
    cancelled_ = true;
    {
        std::lock_guard lock(mutex_);
        if (websocket_) {
            if (connection_state_ == ConnectionState::kReady) {
                SendFrame(websocket_, AsrProtocol::MakeFinishConnectionFrame(config_, session_options_));
            }
            WinHttpCloseHandle(websocket_);
            websocket_ = nullptr;
        }
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

    AddHeader(request, "X-Api-Key", config_.ActiveApiKey());
    AddHeader(request, "X-Api-Request-Id", GenerateSessionId());
    AddHeader(request, "X-Api-Sequence", "-1");
    if (config_.asr_provider == AsrProvider::kVoiceStickCloud) {
        if (!config_.paired_device_ids.empty()) {
            AddHeader(request, "X-Device-Id", config_.paired_device_ids.front());
        }
    } else {
        AddHeader(request, "X-Api-Resource-Id", config_.resource_id);
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

    {
        std::lock_guard lock(mutex_);
        websocket_ = websocket;
        connection_state_ = ConnectionState::kConnecting;
    }
    if (!SendReusableFrameOrFail(websocket, AsrProtocol::MakeStartConnectionFrame(config_, session_options_),
                                 "start ASR connection")) {
        CloseHandles(session, connect, request, websocket);
        return;
    }
    while (!cancelled_) {
        ReceiveOneReusable(websocket);
    }
    {
        std::lock_guard lock(mutex_);
        if (websocket_ == websocket) websocket_ = nullptr;
        connection_state_ = ConnectionState::kDisconnected;
    }
    CloseHandles(session, connect, request, websocket);
}

void AsrClientWin::FlushQueuedAudioChunks(HINTERNET websocket) {
    std::vector<QueuedAudioChunk> chunks;
    std::string session_id;
    {
        std::lock_guard lock(mutex_);
        chunks.swap(queued_audio_chunks_);
        session_id = current_session_id_;
    }
    for (const auto& chunk : chunks) {
        if (!SendReusableFrameOrFail(websocket,
                                     AsrProtocol::MakeTaskRequestFrame(chunk.data, session_id),
                                     "send ASR audio")) {
            return;
        }
        if (chunk.is_last) {
            FinishReusableSessionIfNeeded(websocket);
        }
    }
}

bool AsrClientWin::SendFrame(HINTERNET websocket, const ByteVector& frame) {
    return WinHttpWebSocketSend(websocket,
                                WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                const_cast<std::uint8_t*>(frame.data()),
                                static_cast<DWORD>(frame.size())) == ERROR_SUCCESS;
}

void AsrClientWin::ReceiveOneReusable(HINTERNET websocket) {
    std::array<std::uint8_t, 64 * 1024> buffer{};
    DWORD bytes_read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
    const DWORD result = WinHttpWebSocketReceive(websocket, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()),
                                                 &bytes_read, &type);
    if (result == ERROR_WINHTTP_TIMEOUT) return;
    if (result != ERROR_SUCCESS) {
        FailReusableSession("ASR WebSocket receive failed: " + std::to_string(result));
        return;
    }
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        FailReusableSession("ASR WebSocket disconnected");
        return;
    }
    if (bytes_read == 0) return;
    if (type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE &&
        type != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
        return;
    }
    HandleReusableResponse(std::span(buffer.data(), bytes_read), websocket);
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
            FailReusableSession(response->text);
            if (response->upgrade_url && on_upgrade_url) {
                on_upgrade_url(*response->upgrade_url, response->text);
            }
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
        std::string session_id;
        bool should_start_session = false;
        {
            std::lock_guard lock(mutex_);
            connection_state_ = ConnectionState::kReady;
            should_start_session = session_state_ == SessionState::kStarting;
            session_id = current_session_id_;
        }
        if (should_start_session && !session_id.empty()) {
            SendReusableFrameOrFail(websocket,
                                    AsrProtocol::MakeStartSessionFrame(config_, session_id, session_options_),
                                    "start ASR session");
        }
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
            if (response.session_id == current_session_id_) {
                session_state_ = SessionState::kStreaming;
                should_flush = true;
            }
        }
        if (should_flush) FlushQueuedAudioChunks(websocket);
        break;
    }

    case AsrEvent::kAsrResponse:
    case AsrEvent::kAsrInfo: {
        std::string transcript;
        std::vector<AsrSegment> segments;
        {
            std::lock_guard lock(mutex_);
            if (response.session_id != current_session_id_) return;
            transcript = AsrProtocol::ExtractTranscript(response.payload_text);
            if (!transcript.empty()) latest_session_transcript_ = transcript;
            segments = AsrProtocol::ExtractNewDefiniteSegments(
                response.payload_text, &emitted_definite_segment_keys_);
        }
        if (!transcript.empty() && on_partial) on_partial(transcript);
        for (const auto& segment : segments) {
            if (on_segment) on_segment(segment);
        }
        break;
    }

    case AsrEvent::kSessionFinished: {
        std::string final_text;
        std::vector<AsrSegment> segments;
        {
            std::lock_guard lock(mutex_);
            if (response.session_id != current_session_id_) return;
            final_text = AsrProtocol::ExtractTranscript(response.payload_text);
            if (final_text.empty()) final_text = latest_session_transcript_;
            segments = AsrProtocol::ExtractNewDefiniteSegments(
                response.payload_text, &emitted_definite_segment_keys_);
            current_session_id_.clear();
            latest_session_transcript_.clear();
            emitted_definite_segment_keys_.clear();
            queued_audio_chunks_.clear();
            session_state_ = SessionState::kIdle;
        }
        for (const auto& segment : segments) {
            if (on_segment) on_segment(segment);
        }
        if (on_final) on_final(final_text);
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
                session_state_ = SessionState::kIdle;
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
    HINTERNET websocket = nullptr;
    std::string session_id;
    SessionState state = SessionState::kIdle;
    {
        std::lock_guard lock(mutex_);
        state = session_state_;
        if (state == SessionState::kStarting || state == SessionState::kIdle) {
            queued_audio_chunks_.push_back(QueuedAudioChunk{ByteVector(data.begin(), data.end()), is_last});
            return;
        }
        if (state == SessionState::kFinishing) return;
        websocket = websocket_;
        session_id = current_session_id_;
    }
    if (!websocket || session_id.empty()) {
        FailReusableSession("ASR WebSocket is not connected");
        return;
    }
    if (!SendReusableFrameOrFail(websocket, AsrProtocol::MakeTaskRequestFrame(data, session_id),
                                 "send ASR audio")) {
        return;
    }
    if (is_last) {
        FinishReusableSessionIfNeeded(websocket);
    }
}

void AsrClientWin::FinishReusableSessionIfNeeded(HINTERNET websocket) {
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
    SendReusableFrameOrFail(websocket,
                            AsrProtocol::MakeFinishSessionFrame(config_, session_id, session_options_),
                            "finish ASR session");
}

void AsrClientWin::FailReusableSession(const std::string& message) {
    const bool was_cancelled = cancelled_.load();
    bool had_active_session = false;
    cancelled_ = true;
    {
        std::lock_guard lock(mutex_);
        had_active_session = session_state_ != SessionState::kIdle;
        queued_audio_chunks_.clear();
        current_session_id_.clear();
        latest_session_transcript_.clear();
        session_state_ = SessionState::kIdle;
        connection_state_ = ConnectionState::kDisconnected;
        websocket_ = nullptr;
    }
    if (!was_cancelled && had_active_session && on_error) on_error(message);
}

bool AsrClientWin::SendReusableFrameOrFail(HINTERNET websocket, const ByteVector& frame,
                                           const std::string& context) {
    if (SendFrame(websocket, frame)) return true;
    FailReusableSession("Failed to " + context);
    return false;
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
