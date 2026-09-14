#pragma once

#include "voice_stick_coordinator.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace voicestick {

struct ScannedBleDevice {
    std::string device_id;
    std::string name;
    std::uint64_t bluetooth_address = 0;
    BluetoothAddressKind address_kind = BluetoothAddressKind::kPublic;
    int rssi = 0;
    std::string object_path;
};

class BleCentralBluez : public BleCentral {
public:
    explicit BleCentralBluez(std::vector<std::string> paired_device_ids);
    ~BleCentralBluez() override;

    void Start() override;
    void UpdatePairedDeviceIds(const std::vector<std::string>& ids) override;
    void ConnectPairedDevice(const std::string& device_id,
                             std::uint64_t bluetooth_address,
                             BluetoothAddressKind address_kind,
                             const std::string& name) override;
    void SendUiState(const std::string& state,
                     const std::string& text,
                     const std::optional<std::string>& device_id) override;
    void SendInteractionMode(InteractionMode mode,
                             const std::optional<std::string>& device_id) override;
    void UpdateFirmware(ByteVector image,
                        const std::string& device_id,
                        std::function<void(FirmwareUpdateProgress)> progress,
                        std::function<void(bool, std::string)> completion) override;
    void CancelFirmwareUpdate() override;
    bool IsConnected(const std::string& device_id) const override;
    void CancelPendingConnect(const std::string& device_id) override;
    void Shutdown() override;

    void StartPairingScan();
    void StopPairingScan();
    std::vector<ScannedBleDevice> ScannedDevices() const;

    std::function<void(std::vector<ScannedBleDevice>)> on_scan_updated;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicestick
