#pragma once

#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>

namespace voicestick {

class BleCentralWin : public BleCentral {
public:
    explicit BleCentralWin(std::vector<std::string> paired_device_ids, HWND dispatch_hwnd = nullptr);
    ~BleCentralWin() override;

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

private:
    struct DeviceSession {
        std::uint64_t bluetooth_address = 0;
        ConnectedDevice device;
        winrt::Windows::Devices::Bluetooth::BluetoothLEDevice ble_device{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession gatt_session{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService service{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic audio_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic state_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic control_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic ota_rx_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic ota_state_characteristic{nullptr};
        winrt::event_token audio_value_changed_token{};
        winrt::event_token state_value_changed_token{};
        winrt::event_token ota_state_value_changed_token{};
        winrt::event_token connection_status_token{};
        winrt::event_token gatt_services_changed_token{};
        bool audio_subscribed = false;
        bool state_subscribed = false;
        bool ota_state_subscribed = false;
        bool ready = false;
    };

    struct FirmwareUpdateSession {
        std::string device_id;
        std::uint32_t transfer_id = 0;
        ByteVector image;
        std::function<void(FirmwareUpdateProgress)> progress;
        std::function<void(bool, std::string)> completion;
        std::atomic<std::uint32_t> device_confirmed_written{0};
        bool cancel_requested = false;
    };

    void StartScan();
    void StopScan();
    void HandleAdvertisement(const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher& watcher,
                              const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args);
    winrt::fire_and_forget ConnectDeviceAsync(std::uint64_t bluetooth_address,
                                              BluetoothAddressKind address_kind,
                                              std::string local_name,
                                              std::string device_id);
    winrt::fire_and_forget WriteControlPayloadAsync(std::shared_ptr<DeviceSession> session, ByteVector payload);
    winrt::fire_and_forget UpdateFirmwareAsync(std::shared_ptr<DeviceSession> session,
                                               std::shared_ptr<FirmwareUpdateSession> update_session);
    void HandleFirmwareOtaStateEvent(const std::string& device_id, const FirmwareOtaStateEvent& event);
    void FinishFirmwareUpdate(std::shared_ptr<FirmwareUpdateSession> update_session,
                              bool success,
                              const std::string& message);
    void HandleDeviceDisconnected(const std::string& device_id, std::shared_ptr<DeviceSession> session);
    void CloseSession(std::shared_ptr<DeviceSession> session);
    void CloseSessions();
    static ByteVector BytesFromBuffer(const winrt::Windows::Storage::Streams::IBuffer& buffer);
    void PublishConnections();

    void DispatchToUiThread(std::function<void()> callback);

    HWND dispatch_hwnd_ = nullptr;
    mutable std::mutex mutex_;
    std::mutex dispatch_mutex_;
    std::queue<std::function<void()>> dispatch_queue_;
    std::set<std::string> paired_device_ids_;
    std::map<std::string, std::shared_ptr<DeviceSession>> sessions_by_device_id_;
    std::shared_ptr<FirmwareUpdateSession> firmware_update_session_;
    std::set<std::uint64_t> connecting_addresses_;
    std::set<std::string> cancelled_device_ids_;
    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher watcher_{nullptr};
    winrt::event_token received_token_{};

public:
    static constexpr UINT WM_BLE_DISPATCH = WM_APP + 100;
    void ProcessDispatchedCallbacks();
};

} // namespace voicestick
