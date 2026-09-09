#include "voice_stick_cloud_api_win.h"

#include "cJSON.h"

#include <Windows.h>
#include <Winhttp.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace voicestick {

namespace {

std::string Trim(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
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

std::optional<std::wstring> ApplyUrlFromWebSocketUrl(const std::string& websocket_url) {
    const auto trimmed = Trim(websocket_url);
    std::string http_url;
    if (StartsWithScheme(trimmed, "wss://")) {
        http_url = "https://";
        http_url.append(trimmed.substr(6));
    } else if (StartsWithScheme(trimmed, "ws://")) {
        http_url = "http://";
        http_url.append(trimmed.substr(5));
    } else if (StartsWithScheme(trimmed, "https://") || StartsWithScheme(trimmed, "http://")) {
        http_url = trimmed;
    } else {
        return std::nullopt;
    }

    auto wide = Utf16FromUtf8(http_url);
    if (wide.empty()) return std::nullopt;

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &components)) return std::nullopt;

    std::wstring apply_url;
    apply_url.assign(components.lpszScheme, components.dwSchemeLength);
    apply_url += L"://";
    apply_url.append(components.lpszHostName, components.dwHostNameLength);
    if ((components.nScheme == INTERNET_SCHEME_HTTP && components.nPort != INTERNET_DEFAULT_HTTP_PORT) ||
        (components.nScheme == INTERNET_SCHEME_HTTPS && components.nPort != INTERNET_DEFAULT_HTTPS_PORT)) {
        apply_url += L":";
        apply_url += std::to_wstring(components.nPort);
    }
    apply_url += L"/voicestick/api-key/apply";
    return apply_url;
}

void AddHeader(HINTERNET request, std::string_view header) {
    const auto wide = Utf16FromUtf8(std::string(header) + "\r\n");
    WinHttpAddRequestHeaders(request, wide.c_str(), static_cast<DWORD>(wide.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
}

std::string LastErrorText() {
    return "WinHTTP error " + std::to_string(GetLastError());
}

DWORD QueryStatusCode(HINTERNET request) {
    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status_code,
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return status_code;
}

std::string ReadResponseBody(HINTERNET request) {
    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        chunk.resize(read);
        body += chunk;
    }
    return body;
}

std::string JsonString(cJSON* object, const char* key) {
    auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return {};
    return item->valuestring;
}

std::string JsonEscape(std::string_view text) {
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

} // namespace

VoiceStickCloudApplyResult ApplyVoiceStickCloudTrialApiKey(
    const std::string& cloud_websocket_url,
    const std::string& device_id) {
    VoiceStickCloudApplyResult result;
    const auto apply_url = ApplyUrlFromWebSocketUrl(cloud_websocket_url);
    if (!apply_url.has_value()) {
        result.error = "Invalid VoiceStick Cloud URL.";
        return result;
    }

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(apply_url->c_str(), 0, 0, &components)) {
        result.error = "Invalid VoiceStick Cloud URL.";
        return result;
    }

    HINTERNET session = WinHttpOpen(L"VoiceStick/Windows",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        result.error = "Failed to start VoiceStick Cloud request: " + LastErrorText();
        return result;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        result.error = "Failed to connect VoiceStick Cloud: " + LastErrorText();
        return result;
    }

    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (path.empty()) path = L"/voicestick/api-key/apply";
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        result.error = "Failed to create VoiceStick Cloud request: " + LastErrorText();
        return result;
    }
    WinHttpSetTimeouts(request, 5000, 5000, 5000, 10000);
    AddHeader(request, "Content-Type: application/json");
    AddHeader(request, "Accept: application/json");

    const std::string payload = device_id.empty()
                                    ? "{}"
                                    : "{\"device_id\":\"" + JsonEscape(device_id) + "\"}";
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
        result.error = "VoiceStick Cloud request failed: " + LastErrorText();
        return result;
    }

    const DWORD status_code = QueryStatusCode(request);
    const auto body = ReadResponseBody(request);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    auto* root = cJSON_ParseWithLength(body.data(), body.size());
    if (root) {
        result.api_key = JsonString(root, "api_key");
        result.url = JsonString(root, "url");
        const auto message = JsonString(root, "message");
        const auto error = JsonString(root, "error");
        cJSON_Delete(root);
        if (result.ok()) return result;
        if (!message.empty()) result.error = message;
        if (result.error.empty() && !error.empty()) result.error = error;
    }

    if (result.error.empty()) {
        result.error = status_code >= 400
                           ? "VoiceStick Cloud request failed: HTTP " + std::to_string(status_code)
                           : "VoiceStick Cloud returned an invalid response.";
    }
    return result;
}

} // namespace voicestick
