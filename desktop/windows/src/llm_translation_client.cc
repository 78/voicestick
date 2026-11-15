#include "llm_translation_client.h"

#include "cJSON.h"

#define LLM_JSON_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable : 4005) // llm_json.hpp defines NOMINMAX, which CMake already sets.
#include "llm_json.hpp"
#pragma warning(pop)
#define LLM_RETRY_IMPLEMENTATION
#include "llm_retry.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <thread>

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
    const auto wide = LLMTranslationClient::Utf16FromUtf8(header + "\r\n");
    WinHttpAddRequestHeaders(request, wide.c_str(), static_cast<DWORD>(wide.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
}

struct WinHttpHandle {
    HINTERNET handle = nullptr;
    ~WinHttpHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

// Live translation is latency sensitive: one quick retry for dropped
// connections, 408/429 and 5xx, and none for auth or bad request errors.
llm::RetryConfig TranslationRetryConfig() {
    llm::RetryConfig config;
    config.max_attempts = 2;
    config.base_delay_ms = 300.0;
    return config;
}

} // namespace

LLMTranslationClient::LLMTranslationClient(AppConfig config) : config_(std::move(config)) {}

void LLMTranslationClient::Translate(std::string text,
                                     std::string target_language,
                                     std::vector<std::string> hotwords,
                                     std::function<void(bool, std::string)> completion) const {
    auto config = config_;
    std::thread([client = LLMTranslationClient(std::move(config)),
                 text = std::move(text),
                 target_language = std::move(target_language),
                 hotwords = std::move(hotwords),
                 completion = std::move(completion)]() mutable {
        std::string error;
        auto translated = client.TranslateSync(text, target_language, hotwords, &error);
        if (!error.empty()) {
            completion(false, error);
        } else {
            completion(true, translated);
        }
    }).detach();
}

std::string LLMTranslationClient::TranslateSync(const std::string& text,
                                                const std::string& target_language,
                                                const std::vector<std::string>& hotwords,
                                                std::string* error) const {
    const auto api_key = Trim(config_.llm_api_key);
    if (api_key.empty()) {
        *error = "Missing LLM API key";
        return {};
    }

    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    const auto path = ChatCompletionsPathAndQuery(&host, &port, &secure, error);
    if (!error->empty()) return {};

    llm::json::Value messages = llm::json::array();
    messages.push_back(llm::json::make_object({
        {"role", "system"},
        {"content", SystemPrompt(target_language, hotwords)},
    }));
    messages.push_back(llm::json::make_object({
        {"role", "user"},
        {"content", text},
    }));
    const auto payload = llm::json::make_object({
        {"model", config_.llm_model},
        {"temperature", 0},
        {"messages", messages},
    }).dump();

    std::string body;
    try {
        body = llm::with_retry<std::string>([&] {
            return PostOnce(host, port, secure, path, api_key, payload);
        }, TranslationRetryConfig()).value;
    } catch (const llm::LLMError& failure) {
        *error = failure.message;
        return {};
    }

    auto* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!root) {
        *error = "Invalid LLM translation response";
        return {};
    }
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    auto* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    auto* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    auto* message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : nullptr;
    auto* content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : nullptr;
    if (!cJSON_IsString(content) || content->valuestring == nullptr) {
        *error = "Invalid LLM translation response";
        return {};
    }
    return Trim(content->valuestring);
}

std::string LLMTranslationClient::PostOnce(const std::wstring& host,
                                           INTERNET_PORT port,
                                           bool secure,
                                           const std::string& path,
                                           const std::string& api_key,
                                           const std::string& payload) {
    WinHttpHandle session{WinHttpOpen(L"VoiceStick/Windows",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS,
                                      0)};
    if (!session.handle) {
        throw llm::LLMError{0, "Failed to start LLM network session: " + LastErrorText(), false};
    }
    WinHttpSetTimeouts(session.handle, 5000, 5000, 5000, 10000);

    WinHttpHandle connect{WinHttpConnect(session.handle, host.c_str(), port, 0)};
    if (!connect.handle) {
        throw llm::LLMError{0, "Failed to connect LLM host: " + LastErrorText(), true};
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    const auto path_w = Utf16FromUtf8(path);
    WinHttpHandle request{WinHttpOpenRequest(connect.handle, L"POST", path_w.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!request.handle) {
        throw llm::LLMError{0, "Failed to create LLM request: " + LastErrorText(), false};
    }
    WinHttpSetTimeouts(request.handle, 5000, 5000, 5000, 10000);
    AddHeader(request.handle, "Authorization: Bearer " + api_key);
    AddHeader(request.handle, "Content-Type: application/json");

    const BOOL sent = WinHttpSendRequest(
        request.handle,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        const_cast<char*>(payload.data()),
        static_cast<DWORD>(payload.size()),
        static_cast<DWORD>(payload.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request.handle, nullptr)) {
        throw llm::LLMError{0, "LLM request failed: " + LastErrorText(), true};
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status != 0 && (status < 200 || status >= 300)) {
        throw llm::LLMError{static_cast<int>(status),
                            "LLM request failed: HTTP " + std::to_string(status), true};
    }

    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request.handle, &available) && available > 0) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, chunk.data(), available, &read)) break;
        chunk.resize(read);
        body += chunk;
    }
    return body;
}

std::string LLMTranslationClient::ChatCompletionsPathAndQuery(std::wstring* host,
                                                              INTERNET_PORT* port,
                                                              bool* secure,
                                                              std::string* error) const {
    auto base = Trim(config_.llm_base_url);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) {
        *error = "Invalid LLM base URL";
        return {};
    }
    const auto url = base.ends_with("/chat/completions") ? base : base + "/chat/completions";
    const auto http_url = StartsWithScheme(url, "http://") || StartsWithScheme(url, "https://")
                              ? url
                              : "https://" + url;
    const auto wide = Utf16FromUtf8(http_url);
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wide.empty() || !WinHttpCrackUrl(wide.c_str(), 0, 0, &components)) {
        *error = "Invalid LLM base URL";
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
    if (path.empty()) path = L"/chat/completions";
    return Utf8FromUtf16(path);
}

std::string LLMTranslationClient::SystemPrompt(const std::string& target_language,
                                               const std::vector<std::string>& hotwords) {
    std::string prompt =
        "You are a real-time speech translator.\n"
        "Translate the user's text into " + target_language + ".\n"
        "Detect the source language automatically.\n"
        "Return only the translated text, with no explanations, quotes, prefixes, alternatives, or markdown.\n"
        "The text may come from live speech recognition and may contain minor recognition errors; infer the intended meaning when it is clear.";
    std::vector<std::string> terms;
    for (auto term : hotwords) {
        term = Trim(std::move(term));
        if (!term.empty()) terms.push_back(std::move(term));
    }
    if (!terms.empty()) {
        prompt += "\n\nImportant terms that may appear:\n";
        for (const auto& term : terms) {
            prompt += "- " + term + "\n";
        }
    }
    return prompt;
}

std::wstring LLMTranslationClient::Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string LLMTranslationClient::Utf8FromUtf16(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::string LLMTranslationClient::LastErrorText() {
    return std::to_string(GetLastError());
}

} // namespace voicestick
