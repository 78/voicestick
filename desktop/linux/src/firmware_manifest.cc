#include "firmware_manifest.h"

#include "cJSON.h"
#include "http_client_linux.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <thread>
#include <vector>

namespace voicestick {

namespace {

std::string NormalizedHardwareName(std::string_view hardware) {
    std::string normalized;
    normalized.reserve(hardware.size());
    for (unsigned char ch : hardware) {
        if (std::isalnum(ch)) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return normalized;
}

struct ParsedVersion {
    std::vector<int> numbers;
    std::string suffix;
    bool has_suffix = false;
};

std::optional<ParsedVersion> ParseVersion(std::string_view text) {
    const auto trimmed = TrimCopy(std::string(text));
    if (trimmed.empty()) return std::nullopt;
    const auto dash = trimmed.find('-');
    const auto number_text = trimmed.substr(0, dash);
    ParsedVersion version;
    version.has_suffix = dash != std::string::npos;
    if (version.has_suffix) version.suffix = trimmed.substr(dash + 1);

    std::size_t start = 0;
    while (start <= number_text.size()) {
        const auto dot = number_text.find('.', start);
        const auto part = number_text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty()) return std::nullopt;
        int value = 0;
        auto result = std::from_chars(part.data(), part.data() + part.size(), value);
        if (result.ec != std::errc() || result.ptr != part.data() + part.size()) return std::nullopt;
        version.numbers.push_back(value);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return version.numbers.empty() ? std::nullopt : std::optional<ParsedVersion>(std::move(version));
}

bool VersionLess(const ParsedVersion& left, const ParsedVersion& right) {
    const auto count = std::max(left.numbers.size(), right.numbers.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int l = i < left.numbers.size() ? left.numbers[i] : 0;
        const int r = i < right.numbers.size() ? right.numbers[i] : 0;
        if (l != r) return l < r;
    }
    if (left.has_suffix && !right.has_suffix) return true;
    if (!left.has_suffix && right.has_suffix) return false;
    if (left.has_suffix && right.has_suffix) return left.suffix < right.suffix;
    return false;
}

std::string JsonStringValue(const cJSON* root, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

std::uint32_t JsonU32Value(const cJSON* root, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX) {
        return 0;
    }
    return static_cast<std::uint32_t>(item->valuedouble);
}

} // namespace

FirmwareManifestClient::FirmwareManifestClient(std::string manifest_url)
    : manifest_url_(std::move(manifest_url)) {}

std::string FirmwareManifestClient::DefaultManifestUrl() {
    return "https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/latest/manifest.json";
}

void FirmwareManifestClient::FetchManifest(ManifestCallback callback) const {
    const auto url = manifest_url_;
    std::thread([url, callback = std::move(callback)]() mutable {
        FirmwareManifestClient client(url);
        std::string error;
        auto manifest = client.FetchManifestSync(error);
        callback(std::move(manifest), std::move(error));
    }).detach();
}

std::optional<FirmwareManifest> FirmwareManifestClient::FetchManifestSync(std::string& error) const {
    auto response = HttpGet(manifest_url_);
    if (!response.ok()) {
        error = response.error.empty() ? "Failed to fetch firmware manifest." : response.error;
        return std::nullopt;
    }
    auto manifest = ParseFirmwareManifest(response.body);
    if (!manifest.has_value()) {
        error = "Firmware update server returned an invalid manifest.";
        return std::nullopt;
    }
    return manifest;
}

std::optional<ByteVector> FirmwareManifestClient::DownloadOtaSync(const FirmwareManifest& manifest,
                                                                  std::string& error) const {
    auto image = HttpGetBytes(manifest.ota_url, error);
    if (!error.empty()) return std::nullopt;
    if (image.size() != manifest.ota_size) {
        error = "Firmware size did not match the manifest.";
        return std::nullopt;
    }
    auto digest = Sha256Hex(image);
    if (digest.empty() || digest != manifest.ota_sha256) {
        error = "Firmware checksum did not match the manifest.";
        return std::nullopt;
    }
    return image;
}

bool FirmwareVersion::IsOlderThan(std::string_view current, std::string_view latest) {
    auto current_version = ParseVersion(current);
    auto latest_version = ParseVersion(latest);
    if (!current_version.has_value() || !latest_version.has_value()) return false;
    return VersionLess(*current_version, *latest_version);
}

std::optional<FirmwareManifest> ParseFirmwareManifest(std::string_view json) {
    std::string json_text(json);
    cJSON* root = cJSON_Parse(json_text.c_str());
    if (!root) return std::nullopt;

    FirmwareManifest manifest;
    manifest.hardware = JsonStringValue(root, "hardware");
    manifest.version = JsonStringValue(root, "version");
    manifest.ota_url = JsonStringValue(root, "ota_url");
    manifest.ota_sha256 = JsonStringValue(root, "ota_sha256");
    manifest.ota_size = JsonU32Value(root, "ota_size");
    manifest.merged_url = JsonStringValue(root, "merged_url");
    manifest.merged_sha256 = JsonStringValue(root, "merged_sha256");
    manifest.merged_size = JsonU32Value(root, "merged_size");
    cJSON_Delete(root);

    if (manifest.hardware.empty() || manifest.version.empty() ||
        manifest.ota_url.empty() || manifest.ota_sha256.empty() || manifest.ota_size == 0) {
        return std::nullopt;
    }
    return manifest;
}

bool IsFirmwareHardwareCompatible(std::string_view device_hardware,
                                  std::string_view current_version,
                                  std::string_view manifest_hardware) {
    (void)current_version;
    if (device_hardware.empty()) {
        return true;
    }
    return NormalizedHardwareName(device_hardware) == NormalizedHardwareName(manifest_hardware);
}

} // namespace voicestick
