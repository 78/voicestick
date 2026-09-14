#include "http_client_linux.h"

#include <libsoup/soup.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>

namespace voicestick {

namespace {

struct SoupSessionDeleter {
    void operator()(SoupSession* session) const {
        if (session) g_object_unref(session);
    }
};

std::unique_ptr<SoupSession, SoupSessionDeleter> MakeSession() {
    return std::unique_ptr<SoupSession, SoupSessionDeleter>(
        soup_session_new_with_options("timeout", 15, "user-agent", "VoiceStick/Linux", nullptr));
}

HttpResponse Send(SoupSession* session, SoupMessage* message) {
    HttpResponse response;
    GError* error = nullptr;
    GBytes* bytes = soup_session_send_and_read(session, message, nullptr, &error);
    response.status = soup_message_get_status(message);
    if (error) {
        response.error = error->message ? error->message : "HTTP request failed";
        g_error_free(error);
        if (bytes) g_bytes_unref(bytes);
        return response;
    }
    if (bytes) {
        gsize size = 0;
        const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
        if (data && size > 0) response.body.assign(data, size);
        g_bytes_unref(bytes);
    }
    if (response.status < 200 || response.status >= 300) {
        response.error = "HTTP " + std::to_string(response.status);
    }
    return response;
}

void ApplyHeaders(SoupMessage* message, const std::map<std::string, std::string>& headers) {
    auto* header_table = soup_message_get_request_headers(message);
    for (const auto& [name, value] : headers) {
        soup_message_headers_replace(header_table, name.c_str(), value.c_str());
    }
}

} // namespace

HttpResponse HttpGet(const std::string& url, const std::map<std::string, std::string>& headers) {
    auto session = MakeSession();
    SoupMessage* message = soup_message_new(SOUP_METHOD_GET, url.c_str());
    if (!message) {
        return HttpResponse{.error = "Invalid URL"};
    }
    soup_message_headers_replace(soup_message_get_request_headers(message), "Cache-Control", "no-cache");
    ApplyHeaders(message, headers);
    auto response = Send(session.get(), message);
    g_object_unref(message);
    return response;
}

HttpResponse HttpPostJson(const std::string& url,
                          const std::string& json_body,
                          const std::map<std::string, std::string>& headers) {
    auto session = MakeSession();
    SoupMessage* message = soup_message_new(SOUP_METHOD_POST, url.c_str());
    if (!message) {
        return HttpResponse{.error = "Invalid URL"};
    }
    GBytes* body = g_bytes_new(json_body.data(), json_body.size());
    soup_message_set_request_body_from_bytes(message, "application/json", body);
    g_bytes_unref(body);
    ApplyHeaders(message, headers);
    auto response = Send(session.get(), message);
    g_object_unref(message);
    return response;
}

ByteVector HttpGetBytes(const std::string& url, std::string& error) {
    auto response = HttpGet(url);
    if (!response.ok()) {
        error = response.error.empty() ? "Download failed" : response.error;
        return {};
    }
    return ByteVector(response.body.begin(), response.body.end());
}

std::string Sha256Hex(std::span<const std::uint8_t> data) {
    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    SHA256(data.data(), data.size(), digest);
    char hex[SHA256_DIGEST_LENGTH * 2 + 1] = {};
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return hex;
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

std::string TrimCopy(std::string value) {
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

} // namespace voicestick
