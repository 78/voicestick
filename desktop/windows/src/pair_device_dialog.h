#pragma once

#include "ble_advertisement_scan.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace voicestick {

class PairDeviceDialog {
public:
    PairDeviceDialog(HINSTANCE instance,
                     HWND owner,
                     std::vector<std::string> existing_device_ids,
                     std::function<void(std::string, std::uint64_t, BluetoothAddressKind, std::string)> on_pair,
                     std::function<void(std::string, std::optional<DeviceInfo>)> on_pair_completed);
    ~PairDeviceDialog();

    void Show();
    void Close();
    HWND hwnd() const { return hwnd_; }
    void SetConnectedDevices(const std::vector<ConnectedDevice>& devices);
    void SetDeviceInfo(const DeviceInfo& info);
    void SetPairingError(const std::string& device_id, const std::string& message);

    std::function<void(std::string device_id)> on_pair_timeout;

private:
    struct PairingDevice {
        std::uint64_t bluetooth_address = 0;
        BluetoothAddressKind address_kind = BluetoothAddressKind::kUnspecified;
        std::string name;
        std::string device_id;
        int rssi = 0;
    };

    struct ScanResults {
        ScanResults(HWND window, std::wstring status) : notification_window(window), status(std::move(status)) {}

        std::mutex mutex;
        HWND notification_window;
        const std::wstring status;
        std::vector<PairingDevice> devices;
        // The service UUID and name can arrive in separate advertising reports.
        std::set<std::uint64_t> pairing_window_addresses;
    };

    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildContent();
    void StartScan(std::wstring status = {});
    void StopScan();
    void ClearScanResults();
    static void HandleAdvertisement(
        ScanResults& results,
        const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args);
    void RebuildList();
    void PairSelectedDevice();
    void BeginPairing(const PairingDevice& device);
    void HandlePairingConnected();
    void HandlePairingSucceeded(const DeviceInfo& info);
    void HandlePairingError(const std::string& message);
    void HandlePairingTimeout();
    void HandlePairingFinalize();
    void FinalizePairing(std::optional<DeviceInfo> info);
    bool IsExistingDevice(const std::string& device_id) const;
    std::wstring Utf16(const std::string& text) const;
    int Dp(int px) const;
    void RebuildUi();
    void DestroyControls();

    HINSTANCE instance_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    HWND status_label_ = nullptr;
    HWND device_list_ = nullptr;
    HWND pair_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HFONT ui_font_ = nullptr;
    UINT dpi_ = 96;
    std::vector<BYTE> dialog_template_;
    std::optional<std::string> pairing_device_id_;
    std::vector<std::string> existing_device_ids_;
    std::function<void(std::string, std::uint64_t, BluetoothAddressKind, std::string)> on_pair_;
    std::function<void(std::string, std::optional<DeviceInfo>)> on_pair_completed_;
    bool pairing_finalized_ = false;
    std::shared_ptr<ScanResults> scan_results_;
    BleAdvertisementScan scan_;
};

} // namespace voicestick
