#pragma once

#include "byte_utils.h"

#include <functional>
#include <optional>
#include <string>

namespace voicestick {

enum class FirmwareAppCompatibility {
    kSupported,
    kAppUpdateRequired,
};

struct FirmwareManifest {
    std::string hardware;
    std::string version;
    std::string ota_url;
    std::string ota_sha256;
    std::uint32_t ota_size = 0;
    std::string merged_url;
    std::string merged_sha256;
    std::uint32_t merged_size = 0;
    std::string minimum_windows_app_version;

    FirmwareAppCompatibility CheckWindowsAppVersion(std::string_view app_version) const;
};

struct DeviceFirmwareInfo {
    std::string hardware;
    std::string current_version;
    std::string device_uuid;
    bool reported_on_connection = false;
    bool requires_input_firmware = false;
    std::optional<std::vector<std::string>> buttons;
    std::string latest_version;
    bool update_available = false;
    bool is_checking = false;
    std::string error_message;
    std::string required_app_version;
};

DeviceFirmwareInfo EvaluateFirmwareAvailability(DeviceFirmwareInfo info,
                                               const FirmwareManifest* manifest,
                                               std::string_view error,
                                               bool is_checking,
                                               std::string_view app_version);

class FirmwareManifestClient {
public:
    using ManifestCallback = std::function<void(std::optional<FirmwareManifest>, std::string)>;

    explicit FirmwareManifestClient(std::string_view manifest_url);

    void FetchManifest(ManifestCallback callback) const;
    std::optional<FirmwareManifest> FetchManifestSync(std::string& error) const;
    std::optional<ByteVector> DownloadOtaSync(const FirmwareManifest& manifest, std::string& error) const;

    static constexpr std::string_view supported_hardware[] = {"stick_s3", "voicestick_c3", "magiclick_2p5"};
    static std::string_view ManifestUrlForHardware(std::string_view hardware);

private:
    std::string manifest_url_;
};

class FirmwareVersion {
public:
    static bool IsOlderThan(std::string_view current, std::string_view latest);
};

std::optional<FirmwareManifest> ParseFirmwareManifest(std::string_view json);
bool IsFirmwareHardwareCompatible(std::string_view device_hardware,
                                  std::string_view current_version,
                                  std::string_view manifest_hardware);

} // namespace voicestick
