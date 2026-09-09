#pragma once

#include <string>

namespace voicestick {

struct VoiceStickCloudApplyResult {
    std::string api_key;
    std::string url;
    std::string error;

    bool ok() const { return !api_key.empty() || !url.empty(); }
};

VoiceStickCloudApplyResult ApplyVoiceStickCloudTrialApiKey(
    const std::string& cloud_websocket_url,
    const std::string& device_id);

} // namespace voicestick
