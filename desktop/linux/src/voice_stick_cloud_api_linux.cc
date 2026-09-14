#include "voice_stick_cloud_api_linux.h"

#include "cJSON.h"
#include "http_client_linux.h"

#include <memory>

namespace voicestick {

namespace {

std::string ApplyUrlFromWebSocketUrl(const std::string& websocket_url) {
    auto trimmed = TrimCopy(websocket_url);
    std::string http_url;
    if (StartsWithScheme(trimmed, "wss://")) {
        http_url = "https://" + trimmed.substr(6);
    } else if (StartsWithScheme(trimmed, "ws://")) {
        http_url = "http://" + trimmed.substr(5);
    } else if (StartsWithScheme(trimmed, "https://") || StartsWithScheme(trimmed, "http://")) {
        http_url = trimmed;
    } else {
        return {};
    }
    const auto scheme_end = http_url.find("://");
    if (scheme_end == std::string::npos) return {};
    const auto path_start = http_url.find('/', scheme_end + 3);
    const auto origin = path_start == std::string::npos ? http_url : http_url.substr(0, path_start);
    return origin + "/voicestick/api-key/apply";
}

std::string JsonString(cJSON* object, const char* key) {
    auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return {};
    return item->valuestring;
}

} // namespace

VoiceStickCloudApplyResult ApplyVoiceStickCloudTrialApiKey(
    const std::string& cloud_websocket_url,
    const std::string& device_id) {
    VoiceStickCloudApplyResult result;
    const auto apply_url = ApplyUrlFromWebSocketUrl(cloud_websocket_url);
    if (apply_url.empty()) {
        result.error = "Invalid VoiceStick Cloud URL.";
        return result;
    }
    const std::string payload = device_id.empty()
                                    ? "{}"
                                    : "{\"device_id\":\"" + JsonEscape(device_id) + "\"}";
    auto response = HttpPostJson(apply_url, payload, {{"Accept", "application/json"}});
    auto* root = cJSON_ParseWithLength(response.body.data(), response.body.size());
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
        result.error = response.status >= 400
                           ? "VoiceStick Cloud request failed: HTTP " + std::to_string(response.status)
                           : (response.error.empty()
                                  ? "VoiceStick Cloud returned an invalid response."
                                  : response.error);
    }
    return result;
}

} // namespace voicestick
