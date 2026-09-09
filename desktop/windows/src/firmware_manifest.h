#pragma once

#include "byte_utils.h"

#include <functional>
#include <optional>
#include <string>

namespace voicestick {

struct FirmwareManifest {
    std::string hardware;
    std::string version;
    std::string ota_url;
    std::string ota_sha256;
    std::uint32_t ota_size = 0;
    std::string merged_url;
    std::string merged_sha256;
    std::uint32_t merged_size = 0;
};

struct DeviceFirmwareInfo {
    std::string hardware;
    std::string current_version;
    std::string latest_version;
    bool update_available = false;
    bool is_checking = false;
    std::string error_message;
};

class FirmwareManifestClient {
public:
    using ManifestCallback = std::function<void(std::optional<FirmwareManifest>, std::string)>;

    explicit FirmwareManifestClient(std::string manifest_url = DefaultManifestUrl());

    void FetchManifest(ManifestCallback callback) const;
    std::optional<FirmwareManifest> FetchManifestSync(std::string& error) const;
    std::optional<ByteVector> DownloadOtaSync(const FirmwareManifest& manifest, std::string& error) const;

    static std::string DefaultManifestUrl();

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
