#include "openai_transcription_client.h"

#include "cJSON.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <sstream>
#include <utility>

namespace voicestick {

namespace {

std::string Trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

bool StartsWithScheme(std::string_view text, std::string_view scheme) {
    return text.size() >= scheme.size() &&
           std::equal(scheme.begin(), scheme.end(), text.begin(), [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs)) ==
                      std::tolower(static_cast<unsigned char>(rhs));
           });
}

void AddHeader(HINTERNET request, const std::string& header) {
    if (header.empty()) return;
    const int len = MultiByteToWideChar(CP_UTF8, 0, header.data(),
                                        static_cast<int>(header.size()), nullptr, 0);
    if (len <= 0) return;
    std::wstring wide_header(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, header.data(), static_cast<int>(header.size()),
                        wide_header.data(), len);
    wide_header += L"\r\n";
    WinHttpAddRequestHeaders(request, wide_header.c_str(),
                             static_cast<DWORD>(wide_header.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
}

} // namespace

OpenAITranscriptionClient::OpenAITranscriptionClient(AppConfig config)
    : config_(std::move(config)) {}

OpenAITranscriptionClient::~OpenAITranscriptionClient() {
    Cancel();
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
        } else {
            worker_.join();
        }
    }
}

bool OpenAITranscriptionClient::Start(AsrSessionOptions options) {
    std::lock_guard lock(mutex_);
    last_start_error_.clear();
    const auto api_key = Trim(config_.llm_api_key);
    if (api_key.empty()) {
        last_start_error_ = "Missing OpenAI API key";
        return false;
    }
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    std::string error;
    const auto path = TranscriptionPathAndQuery(&host, &port, &secure, &error);
    if (path.empty()) {
        last_start_error_ = error.empty() ? "Invalid OpenAI base URL" : error;
        return false;
    }
    cancelled_ = false;
    buffered_audio_.clear();
    session_options_ = std::move(options);
    if (session_options_.hotwords.empty()) {
        session_options_.hotwords = config_.asr_hotwords;
    }
    started_ = true;
    return true;
}

void OpenAITranscriptionClient::SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) {
    std::lock_guard lock(mutex_);
    if (!started_ || cancelled_) return;
    if (!data.empty()) {
        buffered_audio_.insert(buffered_audio_.end(), data.begin(), data.end());
    }
    if (is_last) {
        auto audio = std::move(buffered_audio_);
        buffered_audio_.clear();
        started_ = false;
        auto hotwords = session_options_.hotwords;
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) {
                worker_.detach();
            } else {
                worker_.join();
            }
        }
        worker_ = std::thread([this, audio = std::move(audio), hotwords = std::move(hotwords)]() mutable {
            UploadTranscription(audio, std::move(hotwords));
        });
    }
}

void OpenAITranscriptionClient::Cancel() {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    buffered_audio_.clear();
    started_ = false;
}

std::string OpenAITranscriptionClient::LastStartError() const {
    std::lock_guard lock(mutex_);
    return last_start_error_;
}

void OpenAITranscriptionClient::UploadTranscription(const std::vector<std::uint8_t>& audio,
                                                    std::vector<std::string> hotwords) {
    if (cancelled_.load()) return;

    const auto api_key = Trim(config_.llm_api_key);
    if (api_key.empty()) {
        FailSession("Missing OpenAI API key");
        return;
    }

    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    std::string error;
    const auto path = TranscriptionPathAndQuery(&host, &port, &secure, &error);
    if (path.empty()) {
        FailSession(error.empty() ? "Invalid OpenAI base URL" : error);
        return;
    }

    if (audio.empty()) {
        FailSession("No audio data to transcribe");
        return;
    }

    if (audio.size() > 25 * 1024 * 1024) {
        FailSession("OpenAI transcription audio is too large (max 25 MB)");
        return;
    }

    const auto boundary = "VoiceStick-" + std::to_string(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const auto body = MakeMultipartBody(boundary, audio, hotwords);

    HINTERNET session = WinHttpOpen(L"VoiceStick/Windows",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        FailSession("Failed to start OpenAI network session: " + LastErrorText());
        return;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        FailSession("Failed to connect OpenAI host: " + LastErrorText());
        return;
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    const auto path_w = [&]() -> std::wstring {
        if (path.empty()) return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, path.data(),
                                            static_cast<int>(path.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring w(static_cast<std::size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                            w.data(), len);
        return w;
    }();

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path_w.c_str(), nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        FailSession("Failed to create OpenAI request: " + LastErrorText());
        return;
    }
    WinHttpSetTimeouts(request, 10000, 10000, 30000, 60000);

    AddHeader(request, "Authorization: Bearer " + api_key);
    AddHeader(request, "Content-Type: multipart/form-data; boundary=" + boundary);
    AddHeader(request, "Accept: application/json");

    const BOOL sent = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (!cancelled_.load()) {
            FailSession("OpenAI request failed: " + LastErrorText());
        }
        return;
    }

    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status_code, &size, WINHTTP_NO_HEADER_INDEX);

    std::string response_body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        chunk.resize(read);
        response_body += chunk;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (cancelled_.load()) return;

    if (status_code < 200 || status_code >= 300) {
        const auto msg = ExtractErrorMessage(response_body);
        if (!msg.empty()) {
            FailSession("OpenAI transcription failed (" + std::to_string(status_code) + "): " + msg);
        } else {
            FailSession("OpenAI transcription failed (" + std::to_string(status_code) + ")");
        }
        return;
    }

    const auto transcript = Trim(ExtractTranscript(response_body));
    if (transcript.empty()) {
        FailSession("OpenAI transcription returned empty text");
        return;
    }

    if (on_final && !cancelled_.load()) {
        on_final(transcript);
    }
}

void OpenAITranscriptionClient::FailSession(const std::string& message) {
    if (cancelled_.load()) return;
    cancelled_ = true;
    if (on_error) on_error(message);
}

void OpenAITranscriptionClient::NotifyError(const std::string& message) {
    if (on_error) on_error(message);
}

std::string OpenAITranscriptionClient::TranscriptionPathAndQuery(
    std::wstring* host, INTERNET_PORT* port, bool* secure, std::string* error) const {
    auto base = Trim(config_.llm_base_url);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) {
        *error = "Invalid OpenAI base URL";
        return {};
    }
    const auto url = base.ends_with("/audio/transcriptions") ? base : base + "/audio/transcriptions";
    const auto http_url = StartsWithScheme(url, "http://") || StartsWithScheme(url, "https://")
                              ? url
                              : "https://" + url;
    const auto wide = [&](const std::string& s) -> std::wstring {
        if (s.empty()) return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                            static_cast<int>(s.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring w(static_cast<std::size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            w.data(), len);
        return w;
    }(http_url);

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wide.empty() || !WinHttpCrackUrl(wide.c_str(), 0, 0, &components)) {
        *error = "Invalid OpenAI base URL";
        return {};
    }
    *host = std::wstring(components.lpszHostName, components.dwHostNameLength);
    *port = components.nPort;
    *secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring path;
    if (components.lpszUrlPath && components.dwUrlPathLength > 0) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/audio/transcriptions";
    std::string result;
    if (path.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                        static_cast<int>(path.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 0) return {};
    result.resize(static_cast<std::size_t>(len));
    WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                        result.data(), len, nullptr, nullptr);
    return result;
}

std::string OpenAITranscriptionClient::MakeMultipartBody(
    std::string_view boundary, std::span<const std::uint8_t> audio,
    const std::vector<std::string>& hotwords) {
    std::ostringstream body;
    body << "--" << boundary << "\r\n";
    body << "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
    body << "whisper-1\r\n";

    std::string prompt;
    for (const auto& hw : hotwords) {
        auto trimmed = Trim(hw);
        if (trimmed.empty()) continue;
        if (!prompt.empty()) prompt += ", ";
        prompt += std::move(trimmed);
    }
    if (!prompt.empty()) {
        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n";
        body << prompt << "\r\n";
    }

    body << "--" << boundary << "\r\n";
    body << "Content-Disposition: form-data; name=\"file\"; filename=\"speech.ogg\"\r\n";
    body << "Content-Type: audio/ogg\r\n\r\n";

    const auto head = body.str();
    std::string result;
    result.reserve(head.size() + audio.size() + boundary.size() + 8);
    result += head;
    result.append(reinterpret_cast<const char*>(audio.data()), audio.size());
    result += "\r\n--" + std::string(boundary) + "--\r\n";
    return result;
}

std::string OpenAITranscriptionClient::ExtractTranscript(std::string_view json_response) {
    auto* root = cJSON_ParseWithLength(json_response.data(), json_response.size());
    if (!root) return {};
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    auto* text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(text) && text->valuestring) {
        return text->valuestring;
    }
    return {};
}

std::string OpenAITranscriptionClient::ExtractErrorMessage(std::string_view json_response) {
    auto* root = cJSON_ParseWithLength(json_response.data(), json_response.size());
    if (!root) {
        return std::string(json_response);
    }
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    auto* error_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error_obj)) {
        auto* message = cJSON_GetObjectItemCaseSensitive(error_obj, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            return message->valuestring;
        }
        auto* type = cJSON_GetObjectItemCaseSensitive(error_obj, "type");
        if (cJSON_IsString(type) && type->valuestring) {
            return type->valuestring;
        }
    }
    auto* message_direct = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsString(message_direct) && message_direct->valuestring) {
        return message_direct->valuestring;
    }
    return std::string(json_response);
}

std::string OpenAITranscriptionClient::LastErrorText() {
    return std::to_string(GetLastError());
}

} // namespace voicestick
