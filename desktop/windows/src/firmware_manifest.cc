#include "firmware_manifest.h"

#include "app_config.h"
#include "cJSON.h"

#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace voicestick {

namespace {

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Trim(std::string_view text) {
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

std::string NormalizedHardwareName(std::string_view hardware) {
    std::string normalized;
    normalized.reserve(hardware.size());
    for (unsigned char ch : hardware) {
        if (std::isalnum(ch)) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return normalized == "m5stacksticks3" ? "sticks3" : normalized;
}

struct ParsedVersion {
    std::vector<int> numbers;
    std::string suffix;
    bool has_suffix = false;
};

std::optional<ParsedVersion> ParseVersion(std::string_view text) {
    const auto trimmed = Trim(text);
    if (trimmed.empty()) return std::nullopt;
    const auto dash = trimmed.find('-');
    const auto number_text = trimmed.substr(0, dash);
    ParsedVersion version;
    version.has_suffix = dash != std::string::npos;
    if (version.has_suffix) version.suffix = trimmed.substr(dash + 1);
    if (version.has_suffix && version.suffix.empty()) return std::nullopt;

    std::size_t start = 0;
    while (start <= number_text.size()) {
        const auto dot = number_text.find('.', start);
        const auto part = number_text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty()) return std::nullopt;
        int value = 0;
        auto result = std::from_chars(part.data(), part.data() + part.size(), value);
        if (result.ec != std::errc() || result.ptr != part.data() + part.size() || value < 0) {
            return std::nullopt;
        }
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

std::string DownloadText(const std::string& url, std::string& error) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    const auto wide_url = Utf16FromUtf8(url);
    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &parts)) {
        error = "Firmware manifest URL is invalid.";
        return {};
    }

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(L"VoiceStick/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "Failed to open HTTP session.";
        return {};
    }
    WinHttpSetTimeouts(session, 5000, 10000, 10000, 15000);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        error = "Failed to connect to firmware update server.";
        return {};
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = "Failed to create firmware update request.";
        return {};
    }

    constexpr wchar_t kNoCacheHeaders[] =
        L"Cache-Control: no-cache\r\n"
        L"Pragma: no-cache\r\n";

    std::string body;
    if (!WinHttpSendRequest(request, kNoCacheHeaders, static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = "Failed to fetch firmware manifest.";
    } else {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        if (status < 200 || status >= 300) {
            error = "Firmware update server returned HTTP " + std::to_string(status) + ".";
        } else {
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                chunk.resize(read);
                body += chunk;
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

ByteVector DownloadBytes(const std::string& url, std::string& error) {
    const auto text = DownloadText(url, error);
    return ByteVector(text.begin(), text.end());
}

std::string Sha256Hex(std::span<const std::uint8_t> data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::uint8_t digest[32] = {};
    const auto status = BCryptHash(algorithm, nullptr, 0,
                                   const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data.data())),
                                   static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) return {};
    char hex[65] = {};
    for (std::size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return hex;
}

} // namespace

FirmwareManifestClient::FirmwareManifestClient(std::string_view manifest_url)
    : manifest_url_(manifest_url) {}

std::string_view FirmwareManifestClient::ManifestUrlForHardware(std::string_view hardware) {
    const auto normalized = NormalizedHardwareName(hardware);
    if (normalized == "sticks3") {
        return "https://78.github.io/voicestick/firmwares/m5stack_stick_s3/latest/manifest.json";
    }
    if (normalized == "voicestickc3") {
        return "https://78.github.io/voicestick/firmwares/voicestick_c3/latest/manifest.json";
    }
    if (normalized == "magiclick2p5") {
        return "https://78.github.io/voicestick/firmwares/magiclick_2p5/latest/manifest.json";
    }
    return {};
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
    auto body = DownloadText(manifest_url_, error);
    if (!error.empty()) return std::nullopt;
    auto manifest = ParseFirmwareManifest(body);
    if (!manifest.has_value()) {
        error = "Firmware update server returned an invalid manifest.";
        return std::nullopt;
    }
    return manifest;
}

std::optional<ByteVector> FirmwareManifestClient::DownloadOtaSync(const FirmwareManifest& manifest,
                                                                  std::string& error) const {
    auto image = DownloadBytes(manifest.ota_url, error);
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

FirmwareAppCompatibility FirmwareManifest::CheckWindowsAppVersion(std::string_view app_version) const {
    if (minimum_windows_app_version.empty()) return FirmwareAppCompatibility::kSupported;
    const auto minimum = ParseVersion(minimum_windows_app_version);
    const auto current = ParseVersion(app_version);
    if (!minimum || !current || VersionLess(*current, *minimum)) {
        return FirmwareAppCompatibility::kAppUpdateRequired;
    }
    return FirmwareAppCompatibility::kSupported;
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
    const auto* minimum_app_versions = cJSON_GetObjectItemCaseSensitive(root, "minimum_app_versions");
    bool valid_minimum = !minimum_app_versions || cJSON_IsNull(minimum_app_versions) ||
                         cJSON_IsObject(minimum_app_versions);
    if (cJSON_IsObject(minimum_app_versions) &&
        cJSON_GetObjectItemCaseSensitive(minimum_app_versions, "windows")) {
        manifest.minimum_windows_app_version = Trim(JsonStringValue(minimum_app_versions, "windows"));
        valid_minimum = ParseVersion(manifest.minimum_windows_app_version).has_value();
    }
    cJSON_Delete(root);

    if (!valid_minimum || manifest.hardware.empty() || manifest.version.empty() ||
        manifest.ota_url.empty() || manifest.ota_sha256.empty() || manifest.ota_size == 0) {
        return std::nullopt;
    }
    return manifest;
}

bool IsFirmwareHardwareCompatible(std::string_view device_hardware,
                                  std::string_view current_version,
                                  std::string_view manifest_hardware) {
    if (device_hardware.empty()) {
        return true;
    }
    return NormalizedHardwareName(device_hardware) == NormalizedHardwareName(manifest_hardware);
}

DeviceFirmwareInfo EvaluateFirmwareAvailability(DeviceFirmwareInfo info,
                                               const FirmwareManifest* manifest,
                                               std::string_view error,
                                               bool is_checking,
                                               std::string_view app_version) {
    info.latest_version.clear();
    info.required_app_version.clear();
    info.is_checking = is_checking;
    info.update_available = false;
    info.error_message = error;
    if (is_checking || !error.empty() || !manifest) return info;
    if (info.hardware.empty() ||
        !IsFirmwareHardwareCompatible(info.hardware, info.current_version, manifest->hardware)) {
        info.error_message = "Firmware manifest does not match the device model.";
        return info;
    }
    info.latest_version = manifest->version;
    if (manifest->CheckWindowsAppVersion(app_version) == FirmwareAppCompatibility::kAppUpdateRequired) {
        info.required_app_version = manifest->minimum_windows_app_version;
    } else {
        info.update_available = FirmwareVersion::IsOlderThan(info.current_version, info.latest_version) &&
                                (!info.requires_input_firmware || !FirmwareVersion::IsOlderThan(
                                    info.latest_version, AppConfig::minimum_compatible_firmware_version));
    }
    return info;
}

} // namespace voicestick
