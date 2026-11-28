#pragma once

#include "ble_advertisement_scan.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
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

class BleCentralWin : public BleCentral, public std::enable_shared_from_this<BleCentralWin> {
public:
    explicit BleCentralWin(std::vector<std::string> paired_device_ids, HWND dispatch_hwnd = nullptr);
    ~BleCentralWin() override;

    void Start() override;
    void SetPairingDialogOpen(bool open);
    void UpdatePairedDeviceIds(const std::vector<std::string>& ids) override;
    void ConnectPairedDevice(const std::string& device_id,
                             std::uint64_t bluetooth_address,
                             BluetoothAddressKind address_kind,
                             const std::string& name) override;
    void PairDevice(const std::string& device_id,
                    std::uint64_t bluetooth_address,
                    BluetoothAddressKind address_kind,
                    const std::string& name) override;
    void SetDisplay(const std::string& scene,
                    const std::string& text,
                    const std::optional<std::string>& device_id) override;
    void StartAudioIn(const std::string& device_id, InteractionMode mode) override;
    void StopAudioIn(const std::string& device_id) override;
    void StartImu(const std::string& device_id,
                  const std::string& axes,
                  int rate_hz,
                  int lease_ms) override;
    void RenewImu(const std::string& device_id, int lease_ms) override;
    void StopImu(const std::string& device_id) override;
    std::vector<std::string> ConnectedReadyDeviceIds() const override;
    void UpdateFirmware(ByteVector image,
                        const std::string& device_id,
                        std::function<void(FirmwareUpdateProgress)> progress,
                        std::function<void(bool, std::string)> completion) override;
    void CancelFirmwareUpdate() override;
    bool IsConnected(const std::string& device_id) const override;
    void CancelPendingConnect(const std::string& device_id) override;
    void Shutdown() override;

private:
    using RpcCallback = std::function<void(std::optional<ControlRpcResponse>)>;

    struct DeviceSession {
        std::uint64_t bluetooth_address = 0;
        ConnectedDevice device;
        winrt::Windows::Devices::Bluetooth::BluetoothLEDevice ble_device{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession gatt_session{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService service{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic stream_tx_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic control_tx_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic control_rx_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic ota_rx_characteristic{nullptr};
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic ota_state_characteristic{nullptr};
        winrt::event_token stream_tx_value_changed_token{};
        winrt::event_token control_tx_value_changed_token{};
        winrt::event_token ota_state_value_changed_token{};
        winrt::event_token connection_status_token{};
        winrt::event_token gatt_services_changed_token{};
        bool stream_tx_subscribed = false;
        bool control_tx_subscribed = false;
        bool ota_state_subscribed = false;
        std::atomic_bool ready{false};
        bool initialized = false;
        ControlReassembler control_reassembler;
        std::uint8_t next_control_msg_id = 1;
        std::uint32_t next_rpc_id = 1;
        std::map<std::uint32_t, RpcCallback> pending_rpcs;
        std::optional<DeviceInfoResult> info;
        std::uint16_t input_stream_id = 0;
        std::uint16_t audio_in_stream_id = 0;
        std::uint16_t imu_stream_id = 0;
        std::optional<std::uint16_t> last_input_seq;
        std::uint32_t buttons_mask = 0;
        std::optional<std::string> last_display_scene;
        std::optional<std::string> last_display_text;
    };

    struct FirmwareUpdateSession {
        std::string device_id;
        std::uint32_t transfer_id = 0;
        ByteVector image;
        std::function<void(FirmwareUpdateProgress)> progress;
        std::function<void(bool, std::string)> completion;
        std::atomic_size_t confirmed_bytes{0};
        std::atomic_bool finished{false};
    };

    void StartScan();
    void StopScan();
    winrt::fire_and_forget WatchBluetoothRadioAsync();
    static winrt::fire_and_forget RestartScanAfterRadioOn(std::weak_ptr<BleCentralWin> weak_self);
    void HandleAdvertisement(const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher& watcher,
                              const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args);
    void StartDirectConnect(const std::string& device_id,
                            std::uint64_t bluetooth_address,
                            BluetoothAddressKind address_kind,
                            const std::string& name,
                            bool pairing);
    winrt::fire_and_forget ConnectDeviceAsync(std::uint64_t bluetooth_address,
                                              BluetoothAddressKind address_kind,
                                              std::string local_name,
                                              std::string device_id,
                                              bool pairing);
    void StartV2Handshake(const std::string& device_id, std::shared_ptr<DeviceSession> session);
    void ProbeLegacyDeviceInfo(const std::string& device_id, std::shared_ptr<DeviceSession> session);
    void SendRpc(const std::string& device_id,
                 std::shared_ptr<DeviceSession> session,
                 std::function<std::string(std::uint32_t)> build_json,
                 RpcCallback completion = {},
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
    winrt::fire_and_forget WriteControlFramesAsync(std::shared_ptr<DeviceSession> session,
                                                   std::vector<ByteVector> frames);
    winrt::fire_and_forget RpcTimeoutAsync(std::shared_ptr<DeviceSession> session,
                                           std::uint32_t rpc_id,
                                           std::chrono::milliseconds timeout);
    void HandleControlMessage(const std::string& device_id,
                              std::shared_ptr<DeviceSession> session,
                              ControlMessage message);
    void HandleInputFrame(const std::string& device_id,
                          std::shared_ptr<DeviceSession> session,
                          const InputStreamFrame& frame);
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
    std::atomic_bool shutting_down_{false};
    mutable std::mutex mutex_;
    std::mutex dispatch_mutex_;
    std::queue<std::function<void()>> dispatch_queue_;
    std::set<std::string> paired_device_ids_;
    std::map<std::string, std::shared_ptr<DeviceSession>> sessions_by_device_id_;
    std::shared_ptr<FirmwareUpdateSession> firmware_update_session_;
    std::set<std::uint64_t> connecting_addresses_;
    bool pairing_dialog_open_ = false;
    std::set<std::uint64_t> pairing_window_addresses_;
    std::set<std::string> cancelled_device_ids_;
    BleAdvertisementScan scan_;
    std::mutex radio_mutex_;
    winrt::Windows::Devices::Radios::Radio bluetooth_radio_{nullptr};
    winrt::event_token radio_state_token_{};

public:
    static constexpr UINT WM_BLE_DISPATCH = WM_APP + 100;
    void ProcessDispatchedCallbacks();
};

} // namespace voicestick
