#include "llm_translation_client.h"

#include "cJSON.h"

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

    HINTERNET session = WinHttpOpen(L"VoiceStick/Windows",
                                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS,
                                   0);
    if (!session) {
        *error = "Failed to start LLM network session: " + LastErrorText();
        return {};
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        *error = "Failed to connect LLM host: " + LastErrorText();
        return {};
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    const auto path_w = Utf16FromUtf8(path);
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path_w.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        *error = "Failed to create LLM request: " + LastErrorText();
        return {};
    }
    WinHttpSetTimeouts(request, 5000, 5000, 5000, 10000);
    AddHeader(request, "Authorization: Bearer " + api_key);
    AddHeader(request, "Content-Type: application/json");

    const auto payload =
        "{\"model\":\"" + JsonEscape(config_.llm_model) + "\","
        "\"temperature\":0,"
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + JsonEscape(SystemPrompt(target_language, hotwords)) + "\"},"
        "{\"role\":\"user\",\"content\":\"" + JsonEscape(text) + "\"}"
        "]}";

    const BOOL sent = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        const_cast<char*>(payload.data()),
        static_cast<DWORD>(payload.size()),
        static_cast<DWORD>(payload.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        *error = "LLM request failed: " + LastErrorText();
        return {};
    }

    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        chunk.resize(read);
        body += chunk;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

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

std::string LLMTranslationClient::JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
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
