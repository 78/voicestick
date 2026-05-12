#include "ble_central_win.h"

#include "app_config.h"
#include "ble_protocol.h"
#include "log.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

namespace voicestick {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothAddressType;
using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicProperties;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattClientCharacteristicConfigurationDescriptorValue;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteOption;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceUnpairingResultStatus;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::Storage::Streams::DataWriter;

constexpr int kServiceDiscoveryAttempts = 6;
constexpr std::chrono::milliseconds kServiceDiscoveryRetryDelay{1000};
constexpr std::chrono::milliseconds kServiceDiscoveryTimeout{8000};
constexpr std::chrono::milliseconds kCharacteristicDiscoveryTimeout{5000};
constexpr std::chrono::milliseconds kDeviceReopenDelay{800};
constexpr std::chrono::milliseconds kConnectionSettleDelay{300};

// HRESULT_FROM_WIN32(ERROR_BAD_COMMAND): Windows surfaces this for our
// scenario when the OS thinks the device is already paired/bonded but the
// remote refuses or rolled its keys. We special-case it to suggest unpairing.
constexpr std::int32_t kErrorBadCommand = static_cast<std::int32_t>(0x80070016);
constexpr std::int32_t kErrorTimeout = static_cast<std::int32_t>(0x800705B4);

BluetoothAddressType ToBluetoothAddressType(BluetoothAddressKind kind) {
    switch (kind) {
    case BluetoothAddressKind::kPublic:
        return BluetoothAddressType::Public;
    case BluetoothAddressKind::kRandom:
        return BluetoothAddressType::Random;
    case BluetoothAddressKind::kUnspecified:
    default:
        return BluetoothAddressType::Unspecified;
    }
}

const char* AddressKindName(BluetoothAddressKind kind) {
    switch (kind) {
    case BluetoothAddressKind::kPublic:
        return "public";
    case BluetoothAddressKind::kRandom:
        return "random";
    case BluetoothAddressKind::kUnspecified:
    default:
        return "unspecified";
    }
}

std::string Utf8FromHstring(const winrt::hstring& value) {
    return winrt::to_string(value);
}

bool HasNotify(const GattCharacteristic& characteristic) {
    return (characteristic.CharacteristicProperties() & GattCharacteristicProperties::Notify) ==
           GattCharacteristicProperties::Notify;
}

bool HasWriteWithoutResponse(const GattCharacteristic& characteristic) {
    return (characteristic.CharacteristicProperties() & GattCharacteristicProperties::WriteWithoutResponse) ==
           GattCharacteristicProperties::WriteWithoutResponse;
}

bool HasWrite(const GattCharacteristic& characteristic) {
    return (characteristic.CharacteristicProperties() & GattCharacteristicProperties::Write) ==
           GattCharacteristicProperties::Write;
}

winrt::Windows::Storage::Streams::IBuffer BufferFromBytes(std::span<const std::uint8_t> payload) {
    DataWriter writer;
    ByteVector bytes(payload.begin(), payload.end());
    writer.WriteBytes(bytes);
    return writer.DetachBuffer();
}

std::uint32_t RandomTransferId() {
    static std::random_device rd;
    static std::mt19937 rng(rd());
    static std::uniform_int_distribution<std::uint32_t> dist(1, UINT32_MAX);
    return dist(rng);
}

std::string FormatBluetoothAddress(std::uint64_t address) {
    char buffer[18]{};
    snprintf(buffer, sizeof(buffer), "%02llX:%02llX:%02llX:%02llX:%02llX:%02llX",
             (address >> 40) & 0xff,
             (address >> 32) & 0xff,
             (address >> 24) & 0xff,
             (address >> 16) & 0xff,
             (address >> 8) & 0xff,
             address & 0xff);
    return buffer;
}

std::string FormatHresult(std::int32_t code) {
    char buffer[16]{};
    snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(code));
    return buffer;
}

bool CanReadAdvertisementAddressType() {
    static const bool available =
        winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs",
            L"BluetoothAddressType");
    return available;
}

std::string ScanStartFailureMessage(const winrt::hresult_error& error) {
    std::string message = "Bluetooth LE scan failed (HRESULT=" + FormatHresult(error.code()) + ")";
    const auto detail = winrt::to_string(error.message());
    if (!detail.empty()) message += ": " + detail;
    message += ". Turn on Bluetooth in Windows Settings, then restart VoiceStick or update paired devices.";
    return message;
}

std::string GattStatusName(GattCommunicationStatus status) {
    switch (status) {
    case GattCommunicationStatus::Success:
        return "Success";
    case GattCommunicationStatus::Unreachable:
        return "Unreachable";
    case GattCommunicationStatus::ProtocolError:
        return "ProtocolError";
    case GattCommunicationStatus::AccessDenied:
        return "AccessDenied";
    default:
        return "Unknown";
    }
}

void LogBleLine(const std::string& message) {
    LogBle(message);
}

std::string PreviewBytes(std::span<const std::uint8_t> bytes, std::size_t limit = 96) {
    std::string out;
    if (bytes.empty()) return out;
    const std::size_t take = std::min(bytes.size(), limit);
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        const auto byte = bytes[i];
        // Show printable ASCII (the device_info is JSON) and dot-substitute
        // anything else so the log stays readable.
        out.push_back((byte >= 0x20 && byte < 0x7f) ? static_cast<char>(byte) : '.');
    }
    if (bytes.size() > take) out += "...";
    return out;
}

} // namespace

BleCentralWin::BleCentralWin(std::vector<std::string> paired_device_ids, HWND dispatch_hwnd)
    : dispatch_hwnd_(dispatch_hwnd),
      paired_device_ids_(paired_device_ids.begin(), paired_device_ids.end()) {}

void BleCentralWin::DispatchToUiThread(std::function<void()> callback) {
    if (!dispatch_hwnd_) {
        callback();
        return;
    }
    {
        std::lock_guard lock(dispatch_mutex_);
        dispatch_queue_.push(std::move(callback));
    }
    PostMessage(dispatch_hwnd_, WM_BLE_DISPATCH, 0, 0);
}

void BleCentralWin::ProcessDispatchedCallbacks() {
    std::queue<std::function<void()>> pending;
    {
        std::lock_guard lock(dispatch_mutex_);
        pending.swap(dispatch_queue_);
    }
    while (!pending.empty()) {
        pending.front()();
        pending.pop();
    }
}

BleCentralWin::~BleCentralWin() {
    Shutdown();
}

void BleCentralWin::Start() {
    StartScan();
}

void BleCentralWin::Shutdown() {
    StopScan();

    std::vector<std::shared_ptr<DeviceSession>> sessions;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [_, session] : sessions_by_device_id_) {
            if (session && session->ready) sessions.push_back(session);
        }
        connecting_addresses_.clear();
        cancelled_device_ids_.clear();
    }

    CloseSessions();
    PublishConnections();
}

void BleCentralWin::UpdatePairedDeviceIds(const std::vector<std::string>& ids) {
    {
        std::lock_guard lock(mutex_);
        paired_device_ids_ = std::set<std::string>(ids.begin(), ids.end());
        connecting_addresses_.clear();
    }
    CloseSessions();
    PublishConnections();
    LogBleLine("paired device list updated; restarting scan");
    StartScan();
}

void BleCentralWin::ConnectPairedDevice(const std::string& device_id,
                                        std::uint64_t bluetooth_address,
                                        BluetoothAddressKind address_kind,
                                        const std::string& name) {
    {
        std::lock_guard lock(mutex_);
        paired_device_ids_.insert(device_id);
        if (sessions_by_device_id_.contains(device_id)) {
            LogBleLine("direct connect skipped: VS-" + device_id + " is already connected");
            return;
        }
        if (connecting_addresses_.contains(bluetooth_address)) {
            LogBleLine("direct connect skipped: " + FormatBluetoothAddress(bluetooth_address) +
                       " is already connecting");
            return;
        }
        connecting_addresses_.insert(bluetooth_address);
    }
    LogBleLine("direct connect requested VS-" + device_id + " address=" +
               FormatBluetoothAddress(bluetooth_address) +
               " kind=" + AddressKindName(address_kind));
    ConnectDeviceAsync(bluetooth_address, address_kind,
                       name.empty() ? "VS-" + device_id : name, device_id);
}

void BleCentralWin::SendUiState(const std::string& state,
                                const std::string& text,
                                const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::UiStatePayload(state, text);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            } else {
                LogBleLine("send ui_state skipped state=" + state +
                           " dev=VS-" + *device_id +
                           " text_len=" + std::to_string(text.size()));
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
            LogBleLine("send ui_state broadcast state=" + state +
                       " targets=" + std::to_string(targets.size()) +
                       " text_len=" + std::to_string(text.size()));
        }
    }

    for (auto& session : targets) {
        LogBleLine("send ui_state state=" + state +
                   " dev=VS-" + session->device.id +
                   " text_len=" + std::to_string(text.size()));
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendInteractionMode(InteractionMode mode,
                                        const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::InteractionModePayload(InteractionModeName(mode));
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

bool BleCentralWin::IsConnected(const std::string& device_id) const {
    std::lock_guard lock(mutex_);
    auto it = sessions_by_device_id_.find(device_id);
    return it != sessions_by_device_id_.end() && it->second->ready;
}

void BleCentralWin::UpdateFirmware(ByteVector image,
                                   const std::string& device_id,
                                   std::function<void(FirmwareUpdateProgress)> progress,
                                   std::function<void(bool, std::string)> completion) {
    std::shared_ptr<DeviceSession> session;
    {
        std::lock_guard lock(mutex_);
        if (firmware_update_session_) {
            completion(false, "A firmware update is already running.");
            return;
        }
        auto it = sessions_by_device_id_.find(device_id);
        if (it == sessions_by_device_id_.end() || !it->second->ready) {
            completion(false, "No VoiceStick is connected.");
            return;
        }
        session = it->second;
        if (!session->ota_rx_characteristic || !session->ota_state_characteristic) {
            completion(false, "The connected firmware does not expose BLE OTA.");
            return;
        }
        if (image.size() > 3 * 1024 * 1024) {
            completion(false, "Firmware image is larger than the OTA partition.");
            return;
        }
        firmware_update_session_ = std::make_shared<FirmwareUpdateSession>();
        firmware_update_session_->device_id = device_id;
        firmware_update_session_->transfer_id = RandomTransferId();
        firmware_update_session_->image = std::move(image);
        firmware_update_session_->progress = std::move(progress);
        firmware_update_session_->completion = std::move(completion);
    }
    if (firmware_update_session_->progress) {
        firmware_update_session_->progress(FirmwareUpdateProgress{
            0, static_cast<int>(firmware_update_session_->image.size()), true});
    }
    UpdateFirmwareAsync(std::move(session), firmware_update_session_);
}

void BleCentralWin::CancelFirmwareUpdate() {
    std::shared_ptr<FirmwareUpdateSession> update_session;
    std::shared_ptr<DeviceSession> device_session;
    {
        std::lock_guard lock(mutex_);
        update_session = firmware_update_session_;
        if (!update_session) return;
        update_session->cancel_requested = true;
        auto it = sessions_by_device_id_.find(update_session->device_id);
        if (it != sessions_by_device_id_.end()) device_session = it->second;
    }
    if (device_session && device_session->ota_rx_characteristic) {
        auto payload = BleProtocol::OtaAbortPayload(update_session->transfer_id);
        try {
            device_session->ota_rx_characteristic.WriteValueAsync(
                BufferFromBytes(payload), GattWriteOption::WriteWithoutResponse);
        } catch (...) {}
    }
    FinishFirmwareUpdate(update_session, false, "Firmware update cancelled.");
}

void BleCentralWin::CancelPendingConnect(const std::string& device_id) {
    std::lock_guard lock(mutex_);
    cancelled_device_ids_.insert(device_id);
    LogBleLine("cancel requested for VS-" + device_id);
}

void BleCentralWin::StartScan() {
    StopScan();
    bool has_paired_devices = false;
    {
        std::lock_guard lock(mutex_);
        has_paired_devices = !paired_device_ids_.empty();
    }
    if (!has_paired_devices) {
        LogBleLine("scan skipped: no paired devices");
        PublishConnections();
        return;
    }
    watcher_ = BluetoothLEAdvertisementWatcher();
    watcher_.ScanningMode(BluetoothLEScanningMode::Active);
    // No AdvertisementFilter here: the firmware puts its 128-bit service UUID
    // in the ADV PDU but the LocalName "VS-XXXX" only in the scan response,
    // and WinRT's per-PDU filter would drop the scan response so we'd never
    // see the device id. Filter on device_id in HandleAdvertisement instead.
    received_token_ = watcher_.Received({this, &BleCentralWin::HandleAdvertisement});
    try {
        watcher_.Start();
        LogBleLine("scan started");
    } catch (const winrt::hresult_error& error) {
        const auto message = ScanStartFailureMessage(error);
        LogBleLine("scan start failed: " + message);
        try {
            watcher_.Received(received_token_);
        } catch (...) {
        }
        watcher_ = nullptr;
        PublishConnections();
        if (on_scan_error) on_scan_error(message);
    } catch (...) {
        const std::string message = "Bluetooth LE scan failed with an unknown error.";
        LogBleLine("scan start failed: unknown error");
        try {
            watcher_.Received(received_token_);
        } catch (...) {
        }
        watcher_ = nullptr;
        PublishConnections();
        if (on_scan_error) on_scan_error(message);
    }
}

void BleCentralWin::StopScan() {
    if (watcher_) {
        try {
            watcher_.Received(received_token_);
            watcher_.Stop();
        } catch (const winrt::hresult_error& error) {
            LogBleLine("scan stop failed: hr=" + FormatHresult(error.code()));
        } catch (...) {
            LogBleLine("scan stop failed: unknown error");
        }
        watcher_ = nullptr;
        LogBleLine("scan stopped");
    }
}

void BleCentralWin::HandleAdvertisement(const BluetoothLEAdvertisementWatcher&,
                                        const BluetoothLEAdvertisementReceivedEventArgs& args) {
    const auto local_name = Utf8FromHstring(args.Advertisement().LocalName());
    auto device_id = BleProtocol::DeviceIdFromName(local_name);
    if (!device_id.has_value()) return;

    const auto bluetooth_address = args.BluetoothAddress();
    BluetoothAddressKind address_kind = BluetoothAddressKind::kUnspecified;
    if (CanReadAdvertisementAddressType()) {
        try {
            switch (args.BluetoothAddressType()) {
            case BluetoothAddressType::Public:
                address_kind = BluetoothAddressKind::kPublic;
                break;
            case BluetoothAddressType::Random:
                address_kind = BluetoothAddressKind::kRandom;
                break;
            default:
                break;
            }
        } catch (...) {
            // Windows 10 2019 builds do not expose this property. Treating
            // the address type as unspecified keeps the old BLE stack on the
            // one-argument FromBluetoothAddressAsync path.
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (!paired_device_ids_.contains(*device_id)) return;
        if (sessions_by_device_id_.contains(*device_id)) return;
        if (connecting_addresses_.contains(bluetooth_address)) return;
        connecting_addresses_.insert(bluetooth_address);
    }

    LogBleLine("advertisement matched VS-" + *device_id + " address=" +
               FormatBluetoothAddress(bluetooth_address) +
               " kind=" + AddressKindName(address_kind));
    ConnectDeviceAsync(bluetooth_address, address_kind, local_name, *device_id);
}

namespace {

winrt::Windows::Foundation::IAsyncAction WaitMs(std::chrono::milliseconds delay) {
    using winrt::Windows::Foundation::TimeSpan;
    co_await winrt::resume_after(TimeSpan{delay});
}

std::string UnpairStatusName(DeviceUnpairingResultStatus status) {
    switch (status) {
    case DeviceUnpairingResultStatus::Unpaired: return "Unpaired";
    case DeviceUnpairingResultStatus::AlreadyUnpaired: return "AlreadyUnpaired";
    case DeviceUnpairingResultStatus::OperationAlreadyInProgress: return "OperationAlreadyInProgress";
    case DeviceUnpairingResultStatus::AccessDenied: return "AccessDenied";
    case DeviceUnpairingResultStatus::Failed: return "Failed";
    default: return "Unknown";
    }
}

// Clears any stale Windows pairing/bond record so that subsequent GATT
// connections do not attempt to encrypt with a long-term key the peripheral
// no longer holds (which manifests as ESP32 NimBLE BLE_HS_EENCRYPT_KEY_SZ).
winrt::Windows::Foundation::IAsyncOperation<bool> TryUnpairAsync(winrt::hstring device_id) {
    try {
        auto info = co_await DeviceInformation::CreateFromIdAsync(device_id);
        if (!info) {
            LogBleLine("unpair: DeviceInformation not found");
            co_return false;
        }
        auto pairing = info.Pairing();
        if (!pairing) {
            LogBleLine("unpair: no pairing interface");
            co_return false;
        }
        LogBleLine("unpair: IsPaired=" + std::string(pairing.IsPaired() ? "true" : "false") +
                   " CanPair=" + std::string(pairing.CanPair() ? "true" : "false"));
        // Always attempt UnpairAsync even if IsPaired() returns false:
        // the Windows API "paired" state does not always reflect the
        // controller-level bond/LTK cache.
        auto result = co_await pairing.UnpairAsync();
        LogBleLine("unpair: result=" + UnpairStatusName(result.Status()));
        co_return result.Status() == DeviceUnpairingResultStatus::Unpaired ||
               result.Status() == DeviceUnpairingResultStatus::AlreadyUnpaired;
    } catch (const winrt::hresult_error& error) {
        LogBleLine("unpair: exception hr=" + FormatHresult(error.code()));
        co_return false;
    } catch (...) {
        LogBleLine("unpair: unknown exception");
        co_return false;
    }
}

winrt::Windows::Foundation::IAsyncOperation<bool> TryResetBluetoothRadioAsync() {
    using winrt::Windows::Devices::Radios::Radio;
    using winrt::Windows::Devices::Radios::RadioKind;
    using winrt::Windows::Devices::Radios::RadioState;
    using winrt::Windows::Devices::Radios::RadioAccessStatus;
    try {
        auto access = co_await Radio::RequestAccessAsync();
        if (access != RadioAccessStatus::Allowed) {
            LogBleLine("radio reset: access denied");
            co_return false;
        }
        auto radios = co_await Radio::GetRadiosAsync();
        for (const auto& radio : radios) {
            if (radio.Kind() == RadioKind::Bluetooth) {
                LogBleLine("radio reset: turning off");
                co_await radio.SetStateAsync(RadioState::Off);
                co_await WaitMs(std::chrono::milliseconds(2000));
                LogBleLine("radio reset: turning on");
                co_await radio.SetStateAsync(RadioState::On);
                co_await WaitMs(std::chrono::milliseconds(3000));
                LogBleLine("radio reset: complete");
                co_return true;
            }
        }
        LogBleLine("radio reset: no Bluetooth radio found");
        co_return false;
    } catch (const winrt::hresult_error& error) {
        LogBleLine("radio reset: failed hr=" + FormatHresult(error.code()));
        co_return false;
    } catch (...) {
        LogBleLine("radio reset: unknown error");
        co_return false;
    }
}

bool IsLikelyStaleBondError(std::int32_t hresult) {
    // Common HRESULTs we have observed when Windows trips over a stale bond
    // or has been left in an inconsistent state by a previous attempt.
    // A timeout also frequently indicates a stale bond: GetGattServicesAsync
    // hangs indefinitely when Windows holds a long-term key the peripheral
    // no longer recognises.
    return hresult == kErrorBadCommand ||
           hresult == kErrorTimeout ||
           hresult == static_cast<std::int32_t>(0x800710DF) || // ERROR_DEVICE_NOT_AVAILABLE
           hresult == static_cast<std::int32_t>(0x8007048F);   // ERROR_DEVICE_NOT_CONNECTED
}

} // namespace

winrt::fire_and_forget BleCentralWin::ConnectDeviceAsync(std::uint64_t bluetooth_address,
                                                         BluetoothAddressKind address_kind,
                                                         std::string local_name,
                                                         std::string device_id) {
    auto session = std::make_shared<DeviceSession>();
    session->bluetooth_address = bluetooth_address;
    session->device = ConnectedDevice{device_id, local_name.empty() ? "VS-" + device_id : local_name};

    auto detach_device_handlers = [device_id](std::shared_ptr<DeviceSession> s) {
        if (!s || !s->ble_device) return;
        if (s->connection_status_token.value != 0) {
            try {
                s->ble_device.ConnectionStatusChanged(s->connection_status_token);
            } catch (...) {
            }
            s->connection_status_token = {};
        }
        if (s->gatt_services_changed_token.value != 0) {
            try {
                s->ble_device.GattServicesChanged(s->gatt_services_changed_token);
            } catch (...) {
            }
            s->gatt_services_changed_token = {};
        }
    };

    auto fail = [this, bluetooth_address, device_id, session, detach_device_handlers](const std::string& message) {
        {
            std::lock_guard lock(mutex_);
            connecting_addresses_.erase(bluetooth_address);
            cancelled_device_ids_.erase(device_id);
        }
        detach_device_handlers(session);
        if (session && session->gatt_session) {
            try {
                session->gatt_session.MaintainConnection(false);
                session->gatt_session.Close();
            } catch (...) {
            }
            session->gatt_session = nullptr;
        }
        if (session && session->ble_device) {
            try {
                session->ble_device.Close();
            } catch (...) {
            }
            session->ble_device = nullptr;
        }
        LogBleLine("connect failed VS-" + device_id + " address=" +
                   FormatBluetoothAddress(bluetooth_address) + " reason=" + message);
        if (on_connection_error) on_connection_error(device_id, message);
    };

    auto open_device = [&](BluetoothAddressType type) -> winrt::Windows::Foundation::IAsyncOperation<BluetoothLEDevice> {
        if (type == BluetoothAddressType::Unspecified) {
            return BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address);
        }
        return BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address, type);
    };

    auto attach_device_handlers = [this, device_id](std::shared_ptr<DeviceSession> s) {
        if (!s || !s->ble_device) return;
        s->connection_status_token = s->ble_device.ConnectionStatusChanged(
            [this, device_id, weak_session = std::weak_ptr<DeviceSession>(s)](
                const BluetoothLEDevice& sender, const winrt::Windows::Foundation::IInspectable&) {
                const auto status = sender.ConnectionStatus();
                LogBleLine("connection status VS-" + device_id + " = " +
                           (status == BluetoothConnectionStatus::Connected ? "connected" : "disconnected"));
                if (status == BluetoothConnectionStatus::Disconnected) {
                    HandleDeviceDisconnected(device_id, weak_session.lock());
                }
            });
        // GattServicesChanged fires when Windows invalidates its system-wide
        // GATT service cache for this peripheral (very common with unpaired
        // devices and ESP32/NimBLE peripherals). Logging it helps diagnose
        // why a subsequent service-discovery call may need to be retried.
        s->gatt_services_changed_token = s->ble_device.GattServicesChanged(
            [device_id](const BluetoothLEDevice&, const winrt::Windows::Foundation::IInspectable&) {
                LogBleLine("GattServicesChanged VS-" + device_id);
            });
    };

    try {
        const auto address_type = ToBluetoothAddressType(address_kind);
        LogBleLine("connecting VS-" + device_id + " address=" + FormatBluetoothAddress(bluetooth_address) +
                   " kind=" + AddressKindName(address_kind));

        // Proactively remove any stale Windows pairing/bond before connecting.
        // ESP32/NimBLE peripherals lose their bond when reset, but Windows
        // caches the old LTK and tries to encrypt with it, causing
        // BLE_HS_EENCRYPT_KEY_SZ (status=26) on the device and
        // Unreachable GATT status on Windows.
        {
            auto probe_device = co_await open_device(address_type);
            if (probe_device) {
                if (co_await TryUnpairAsync(probe_device.DeviceId())) {
                    LogBleLine("cleared stale Windows pairing for VS-" + device_id);
                    co_await WaitMs(std::chrono::milliseconds(500));
                }
                probe_device.Close();
            }
        }

        session->ble_device = co_await open_device(address_type);
        if (!session->ble_device) {
            fail("Windows could not open the BLE device. Make sure the device is advertising and try again.");
            co_return;
        }
        LogBleLine("BluetoothLEDevice opened VS-" + device_id +
                   " device_id=" + winrt::to_string(session->ble_device.DeviceId()));
        attach_device_handlers(session);

        // Give the controller a brief moment to actually establish the link
        // before triggering service discovery.
        co_await WaitMs(kConnectionSettleDelay);

        try {
            session->gatt_session = co_await GattSession::FromDeviceIdAsync(
                session->ble_device.BluetoothDeviceId());
            if (session->gatt_session) {
                session->gatt_session.MaintainConnection(true);
                LogBleLine("GattSession created+maintained VS-" + device_id +
                           " max_pdu_size=" + std::to_string(session->gatt_session.MaxPduSize()));
            }
        } catch (const winrt::hresult_error& error) {
            LogBleLine("early GattSession unavailable VS-" + device_id + ": " +
                       FormatHresult(error.code()));
        }

        // Give MaintainConnection time to establish the link-layer
        // connection before triggering service discovery.
        constexpr int kConnectionPollIntervalMs = 500;
        constexpr int kConnectionPollAttempts = 8;
        for (int poll = 0; poll < kConnectionPollAttempts; ++poll) {
            co_await WaitMs(std::chrono::milliseconds(kConnectionPollIntervalMs));
            if (session->ble_device.ConnectionStatus() == BluetoothConnectionStatus::Connected) {
                LogBleLine("link-layer connected VS-" + device_id +
                           " after " + std::to_string((poll + 1) * kConnectionPollIntervalMs) + "ms");
                break;
            }
        }
        auto pre_status = session->ble_device.ConnectionStatus();
        LogBleLine("pre-discovery status VS-" + device_id + " = " +
                   (pre_status == BluetoothConnectionStatus::Connected ? "connected" : "disconnected") +
                   " max_pdu_size=" + (session->gatt_session
                       ? std::to_string(session->gatt_session.MaxPduSize()) : "n/a"));

        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceServicesResult service_result{nullptr};
        bool unpair_attempted = false;
        for (int attempt = 1; attempt <= kServiceDiscoveryAttempts; ++attempt) {
            {
                bool cancelled = false;
                {
                    std::lock_guard lock(mutex_);
                    cancelled = cancelled_device_ids_.contains(device_id);
                }
                if (cancelled) {
                    fail("cancelled");
                    co_return;
                }
            }
            const auto cache_mode = (attempt == kServiceDiscoveryAttempts)
                                        ? BluetoothCacheMode::Cached
                                        : BluetoothCacheMode::Uncached;

            std::int32_t throw_hresult = 0;
            std::string throw_message;
            try {
                auto async_op = session->ble_device.GetGattServicesAsync(cache_mode);
                constexpr int kPollMs = 500;
                const int max_polls = static_cast<int>(kServiceDiscoveryTimeout.count()) / kPollMs;
                for (int p = 0; p < max_polls; ++p) {
                    co_await WaitMs(std::chrono::milliseconds(kPollMs));
                    if (async_op.Status() != winrt::Windows::Foundation::AsyncStatus::Started) break;
                    bool cancelled = false;
                    {
                        std::lock_guard lock(mutex_);
                        cancelled = cancelled_device_ids_.contains(device_id);
                    }
                    if (cancelled) {
                        async_op.Cancel();
                        fail("cancelled");
                        co_return;
                    }
                }
                if (async_op.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
                    async_op.Cancel();
                    throw_hresult = kErrorTimeout;
                    throw_message = "service discovery timed out";
                } else {
                    service_result = async_op.GetResults();
                }
            } catch (const winrt::hresult_error& error) {
                throw_hresult = error.code();
                throw_message = winrt::to_string(error.message());
            }

            if (throw_hresult != 0) {
                LogBleLine("service discovery threw VS-" + device_id +
                           " attempt=" + std::to_string(attempt) +
                           " hr=" + FormatHresult(throw_hresult) +
                           " message=" + throw_message);
                if (!unpair_attempted && IsLikelyStaleBondError(throw_hresult)) {
                    unpair_attempted = true;
                    LogBleLine("attempting to remove stale Windows pairing for VS-" + device_id);
                    if (co_await TryUnpairAsync(session->ble_device.DeviceId())) {
                        LogBleLine("stale Windows pairing removed for VS-" + device_id);
                    }
                }
                if (attempt < kServiceDiscoveryAttempts) {
                    // Tear the device object down completely and re-open it.
                    // Reusing a BluetoothLEDevice that has already returned
                    // 0x80070016 keeps producing the same error indefinitely;
                    // a fresh handle re-runs the OS connection state machine.
                    LogBleLine("recycling BluetoothLEDevice VS-" + device_id);
                    detach_device_handlers(session);
                    try {
                        session->ble_device.Close();
                    } catch (...) {
                    }
                    session->ble_device = nullptr;
                    co_await WaitMs(kDeviceReopenDelay);
                    session->ble_device = co_await open_device(address_type);
                    if (!session->ble_device) {
                        fail("Windows could not reopen the BLE device after a transient failure.");
                        co_return;
                    }
                    attach_device_handlers(session);
                    try {
                        session->gatt_session = co_await GattSession::FromDeviceIdAsync(
                            session->ble_device.BluetoothDeviceId());
                        if (session->gatt_session) {
                            session->gatt_session.MaintainConnection(true);
                        }
                    } catch (...) {}
                    co_await WaitMs(kConnectionSettleDelay);
                    continue;
                }

                std::string hint = " (HRESULT=" + FormatHresult(throw_hresult) + ")";
                if (throw_hresult == kErrorBadCommand) {
                    hint += ". Toggle Bluetooth off and back on from the Windows "
                            "Action Center to flush the OS GATT cache, then retry. "
                            "If the device is listed in \"Bluetooth & devices\" "
                            "settings, remove it first.";
                }
                fail("Windows BLE refused the connection" + hint);
                co_return;
            }

            const auto status = service_result.Status();
            const auto services = service_result.Services();
            const auto count = services.Size();
            LogBleLine("service discovery attempt " + std::to_string(attempt) +
                       " VS-" + device_id + " mode=" +
                       (cache_mode == BluetoothCacheMode::Uncached ? "uncached" : "cached") +
                       " status=" + GattStatusName(status) +
                       " count=" + std::to_string(count));

            if (status == GattCommunicationStatus::Success && count > 0) {
                const winrt::guid wanted{BleProtocol::service_uuid};
                for (uint32_t i = 0; i < count; ++i) {
                    auto candidate = services.GetAt(i);
                    if (candidate.Uuid() == wanted) {
                        session->service = candidate;
                        break;
                    }
                }
                if (session->service) break;
                LogBleLine("VoiceStick service UUID not present in result for VS-" + device_id);
            }

            if (attempt == kServiceDiscoveryAttempts) {
                fail("VoiceStick service discovery failed after retries (status=" +
                     GattStatusName(status) + ", services=" + std::to_string(count) +
                     "). Toggle Bluetooth off and back on, then try pairing again.");
                co_return;
            }

            // Unreachable usually means the peripheral rejected the encrypted
            // link (stale bond / LTK mismatch). Unpair, reset the Bluetooth
            // radio to flush the controller-level key cache, then recycle.
            if (status == GattCommunicationStatus::Unreachable && !unpair_attempted) {
                unpair_attempted = true;
                LogBleLine("Unreachable: removing stale Windows pairing for VS-" + device_id);
                co_await TryUnpairAsync(session->ble_device.DeviceId());

                LogBleLine("Unreachable: tearing down device handles before radio reset");
                detach_device_handlers(session);
                if (session->gatt_session) {
                    try { session->gatt_session.MaintainConnection(false); session->gatt_session.Close(); } catch (...) {}
                    session->gatt_session = nullptr;
                }
                try { session->ble_device.Close(); } catch (...) {}
                session->ble_device = nullptr;

                if (co_await TryResetBluetoothRadioAsync()) {
                    LogBleLine("Unreachable: radio reset succeeded, reopening device");
                } else {
                    LogBleLine("Unreachable: radio reset skipped/failed, recycling anyway");
                    co_await WaitMs(kDeviceReopenDelay);
                }

                session->ble_device = co_await open_device(address_type);
                if (!session->ble_device) {
                    fail("Windows could not reopen the BLE device after unpairing.");
                    co_return;
                }
                attach_device_handlers(session);
                try {
                    session->gatt_session = co_await GattSession::FromDeviceIdAsync(
                        session->ble_device.BluetoothDeviceId());
                    if (session->gatt_session) session->gatt_session.MaintainConnection(true);
                } catch (...) {}
                co_await WaitMs(kConnectionSettleDelay);
                continue;
            }

            co_await WaitMs(kServiceDiscoveryRetryDelay);
        }

        LogBleLine("service discovered VS-" + device_id);

        if (!session->gatt_session) {
            try {
                session->gatt_session = co_await GattSession::FromDeviceIdAsync(
                    session->ble_device.BluetoothDeviceId());
                if (session->gatt_session) {
                    session->gatt_session.MaintainConnection(true);
                    LogBleLine("GattSession (late) maintained VS-" + device_id +
                               " max_pdu_size=" + std::to_string(session->gatt_session.MaxPduSize()));
                }
            } catch (const winrt::hresult_error& error) {
                LogBleLine("GattSession unavailable VS-" + device_id + ": " +
                           FormatHresult(error.code()));
            }
        } else {
            LogBleLine("GattSession confirmed VS-" + device_id +
                       " max_pdu_size=" + std::to_string(session->gatt_session.MaxPduSize()));
        }

        // Characteristic discovery uses wait_for() which blocks; move off
        // the STA so we don't deadlock the UI message pump.
        co_await winrt::resume_background();

        using winrt::Windows::Foundation::AsyncStatus;
        using GattCharsResult = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicsResult;

        auto discover_characteristic = [&](const winrt::guid& uuid,
                                           const char* label) -> GattCharsResult {
            auto op = session->service.GetCharacteristicsForUuidAsync(uuid, BluetoothCacheMode::Uncached);
            if (op.wait_for(kCharacteristicDiscoveryTimeout) == AsyncStatus::Started) {
                op.Cancel();
                LogBleLine(std::string(label) + " characteristic discovery timed out VS-" + device_id);
                return nullptr;
            }
            return op.GetResults();
        };

        auto audio_result = discover_characteristic(winrt::guid{BleProtocol::audio_uuid}, "audio_tx");
        auto state_result = discover_characteristic(winrt::guid{BleProtocol::state_uuid}, "state_tx");
        auto control_result = discover_characteristic(winrt::guid{BleProtocol::control_uuid}, "control_rx");
        auto ota_rx_result = discover_characteristic(winrt::guid{BleProtocol::ota_rx_uuid}, "ota_rx");
        auto ota_state_result = discover_characteristic(winrt::guid{BleProtocol::ota_state_uuid}, "ota_state");
        if (!audio_result || audio_result.Status() != GattCommunicationStatus::Success || audio_result.Characteristics().Size() == 0) {
            fail("audio_tx discovery failed: " + (audio_result ? GattStatusName(audio_result.Status()) : std::string("timeout")));
            co_return;
        }
        if (!state_result || state_result.Status() != GattCommunicationStatus::Success || state_result.Characteristics().Size() == 0) {
            fail("state_tx discovery failed: " + (state_result ? GattStatusName(state_result.Status()) : std::string("timeout")));
            co_return;
        }
        if (!control_result || control_result.Status() != GattCommunicationStatus::Success || control_result.Characteristics().Size() == 0) {
            fail("control_rx discovery failed: " + (control_result ? GattStatusName(control_result.Status()) : std::string("timeout")));
            co_return;
        }

        session->audio_characteristic = audio_result.Characteristics().GetAt(0);
        session->state_characteristic = state_result.Characteristics().GetAt(0);
        session->control_characteristic = control_result.Characteristics().GetAt(0);
        if (ota_rx_result && ota_rx_result.Status() == GattCommunicationStatus::Success &&
            ota_rx_result.Characteristics().Size() > 0) {
            session->ota_rx_characteristic = ota_rx_result.Characteristics().GetAt(0);
        }
        if (ota_state_result && ota_state_result.Status() == GattCommunicationStatus::Success &&
            ota_state_result.Characteristics().Size() > 0) {
            session->ota_state_characteristic = ota_state_result.Characteristics().GetAt(0);
        }
        if (!HasNotify(session->audio_characteristic) || !HasNotify(session->state_characteristic) ||
            !HasWriteWithoutResponse(session->control_characteristic)) {
            fail("required GATT characteristic properties are missing");
            co_return;
        }
        if (session->ota_rx_characteristic &&
            !HasWrite(session->ota_rx_characteristic) &&
            !HasWriteWithoutResponse(session->ota_rx_characteristic)) {
            session->ota_rx_characteristic = nullptr;
        }
        if (session->ota_state_characteristic && !HasNotify(session->ota_state_characteristic)) {
            session->ota_state_characteristic = nullptr;
        }

        session->audio_value_changed_token = session->audio_characteristic.ValueChanged(
            [this, device_id](const GattCharacteristic&, const auto& args) {
                auto bytes = BytesFromBuffer(args.CharacteristicValue());
                auto frame = BleProtocol::ParseAudioFrame(bytes);
                if (frame.has_value()) {
                    DispatchToUiThread([this, device_id, f = std::move(*frame)]() {
                        if (on_audio_frame) on_audio_frame(device_id, f);
                    });
                }
            });
        session->state_value_changed_token = session->state_characteristic.ValueChanged(
            [this, device_id](const GattCharacteristic&, const auto& args) {
                auto bytes = BytesFromBuffer(args.CharacteristicValue());
                LogBleLine("state notify VS-" + device_id +
                           " len=" + std::to_string(bytes.size()) +
                           " preview=" + PreviewBytes(bytes));
                auto event = BleProtocol::ParseStateEvent(bytes);
                if (!event.has_value()) {
                    LogBleLine("state notify VS-" + device_id + " parse failed");
                    return;
                }
                LogBleLine("state event VS-" + device_id + " type=" + event->event +
                           (event->firmware_version.empty()
                                ? std::string()
                                : " firmware=" + event->firmware_version));
                DispatchToUiThread([this, device_id, e = std::move(*event)]() {
                    if (on_state_event) on_state_event(device_id, e);
                });
            });
        if (session->ota_state_characteristic) {
            session->ota_state_value_changed_token = session->ota_state_characteristic.ValueChanged(
                [this, device_id](const GattCharacteristic&, const auto& args) {
                    auto bytes = BytesFromBuffer(args.CharacteristicValue());
                    auto event = BleProtocol::ParseFirmwareOtaStateEvent(bytes);
                    if (!event.has_value()) {
                        LogBleLine("ota state notify VS-" + device_id + " parse failed");
                        return;
                    }
                    DispatchToUiThread([this, device_id, e = std::move(*event)]() {
                        HandleFirmwareOtaStateEvent(device_id, e);
                    });
                });
        }

        // Give the Windows BTHLE driver a brief moment to wire the
        // ValueChanged handlers in before we ask for notifications. Without
        // this gap the very first notification can race past the handler.
        co_await WaitMs(std::chrono::milliseconds(150));

        // Subscribe to state first so the firmware delivers device_info as
        // soon as possible; the audio CCCD write is heavier (Windows seems
        // to delay the next ATT op for a few hundred ms after enabling
        // notifications on a high-throughput characteristic), and putting
        // state second has been observed to push device_info out by ~1s.
        LogBleLine("subscribing state notifications VS-" + device_id);
        auto state_subscribe = co_await session->state_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        LogBleLine("state subscribe VS-" + device_id +
                   " status=" + GattStatusName(state_subscribe));

        // Let device_info ride out on the air before we issue another ATT op
        // (CCCD writes serialize the ATT channel and can delay the firmware's
        // outgoing notification by tens to hundreds of ms).
        co_await WaitMs(std::chrono::milliseconds(250));

        LogBleLine("subscribing audio notifications VS-" + device_id);
        auto audio_subscribe = co_await session->audio_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        LogBleLine("audio subscribe VS-" + device_id +
                   " status=" + GattStatusName(audio_subscribe));

        GattCommunicationStatus ota_subscribe = GattCommunicationStatus::Success;
        if (session->ota_state_characteristic) {
            ota_subscribe = co_await session->ota_state_characteristic
                .WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue::Notify);
            LogBleLine("ota state subscribe VS-" + device_id +
                       " status=" + GattStatusName(ota_subscribe));
        }

        if (audio_subscribe != GattCommunicationStatus::Success ||
            state_subscribe != GattCommunicationStatus::Success ||
            ota_subscribe != GattCommunicationStatus::Success) {
            fail("notification subscription failed: audio=" + GattStatusName(audio_subscribe) +
                 " state=" + GattStatusName(state_subscribe) +
                 " ota=" + GattStatusName(ota_subscribe));
            co_return;
        }

        session->audio_subscribed = true;
        session->state_subscribed = true;
        session->ota_state_subscribed = session->ota_state_characteristic != nullptr;
        session->ready = true;
        {
            std::lock_guard lock(mutex_);
            sessions_by_device_id_[device_id] = session;
            connecting_addresses_.erase(bluetooth_address);
        }
        PublishConnections();
        LogBleLine("connected VS-" + device_id);
        SendUiState("ready", "", device_id);
    } catch (const winrt::hresult_error& error) {
        std::string message = "WinRT error " + FormatHresult(error.code()) +
                              ": " + winrt::to_string(error.message());
        if (error.code() == kErrorBadCommand) {
            message += ". Open Windows \"Bluetooth & devices\" settings, remove any "
                       "existing VS-" + device_id + " entry, then retry pairing.";
        }
        fail(message);
    } catch (const std::exception& error) {
        fail(error.what());
    } catch (...) {
        fail("unknown BLE exception");
    }
}

winrt::fire_and_forget BleCentralWin::WriteControlPayloadAsync(std::shared_ptr<DeviceSession> session, ByteVector payload) {
    try {
        if (!session || !session->ready || !session->control_characteristic) co_return;
        DataWriter writer;
        writer.WriteBytes(payload);
        co_await session->control_characteristic.WriteValueAsync(
            writer.DetachBuffer(), GattWriteOption::WriteWithoutResponse);
    } catch (...) {
    }
}

winrt::fire_and_forget BleCentralWin::UpdateFirmwareAsync(
    std::shared_ptr<DeviceSession> session,
    std::shared_ptr<FirmwareUpdateSession> update_session) {
    try {
        if (!session || !update_session || !session->ota_rx_characteristic) {
            FinishFirmwareUpdate(update_session, false, "The connected firmware does not expose BLE OTA.");
            co_return;
        }

        auto write_payload = [&](const ByteVector& payload, GattWriteOption option)
            -> winrt::Windows::Foundation::IAsyncOperation<GattCommunicationStatus> {
            return session->ota_rx_characteristic.WriteValueAsync(BufferFromBytes(payload), option);
        };
        const bool ota_supports_write_without_response =
            HasWriteWithoutResponse(session->ota_rx_characteristic);

        LogBleLine("OTA begin VS-" + update_session->device_id +
                   " transfer=" + std::to_string(update_session->transfer_id) +
                   " size=" + std::to_string(update_session->image.size()));
        auto begin = BleProtocol::OtaBeginPayload(
            static_cast<std::uint32_t>(update_session->image.size()),
            update_session->transfer_id);
        auto status = co_await write_payload(begin, GattWriteOption::WriteWithResponse);
        if (status != GattCommunicationStatus::Success) {
            FinishFirmwareUpdate(update_session, false, "BLE OTA begin failed: " + GattStatusName(status));
            co_return;
        }

        const std::size_t max_pdu = session->gatt_session ? session->gatt_session.MaxPduSize() : 247;
        const std::size_t chunk_size = std::max<std::size_t>(
            20, std::min<std::size_t>(max_pdu > 15 ? max_pdu - 15 : 20, 244));
        const std::size_t max_in_flight = 48 * 1024;
        LogBleLine("OTA data VS-" + update_session->device_id +
                   " chunk_size=" + std::to_string(chunk_size) +
                   " max_pdu=" + std::to_string(max_pdu) +
                   " write_without_response=" +
                   (ota_supports_write_without_response ? "true" : "false"));
        std::size_t offset = 0;
        std::size_t last_progress = 0;
        while (offset < update_session->image.size()) {
            if (update_session->cancel_requested) co_return;
            if (ota_supports_write_without_response &&
                offset > update_session->device_confirmed_written.load() + max_in_flight) {
                co_await winrt::resume_after(std::chrono::milliseconds(20));
                continue;
            }
            const auto end = std::min(offset + chunk_size, update_session->image.size());
            auto payload = BleProtocol::OtaDataPayload(
                update_session->transfer_id,
                static_cast<std::uint32_t>(offset),
                std::span<const std::uint8_t>(update_session->image.data() + offset, end - offset));
            status = co_await write_payload(
                payload,
                ota_supports_write_without_response
                    ? GattWriteOption::WriteWithoutResponse
                    : GattWriteOption::WriteWithResponse);
            if (status != GattCommunicationStatus::Success) {
                LogBleLine("OTA write failed VS-" + update_session->device_id +
                           " offset=" + std::to_string(offset) +
                           " status=" + GattStatusName(status));
                FinishFirmwareUpdate(update_session, false, "BLE OTA write failed: " + GattStatusName(status));
                co_return;
            }
            offset = end;
            if (offset - last_progress >= 64 * 1024 || offset == update_session->image.size()) {
                last_progress = offset;
                LogBleLine("OTA sent VS-" + update_session->device_id +
                           " written=" + std::to_string(offset) +
                           "/" + std::to_string(update_session->image.size()));
                if (update_session->progress) {
                    update_session->progress(FirmwareUpdateProgress{
                        static_cast<int>(offset),
                        static_cast<int>(update_session->image.size()),
                        false});
                }
            }
        }

        auto final_wait_started = std::chrono::steady_clock::now();
        while (update_session->device_confirmed_written.load() < update_session->image.size()) {
            if (update_session->cancel_requested) co_return;
            if (std::chrono::steady_clock::now() - final_wait_started > std::chrono::seconds(10)) {
                LogBleLine("OTA final device progress timed out VS-" + update_session->device_id +
                           " confirmed=" +
                           std::to_string(update_session->device_confirmed_written.load()) +
                           "/" + std::to_string(update_session->image.size()));
                FinishFirmwareUpdate(update_session, false,
                                     "Device stopped confirming OTA progress.");
                co_return;
            }
            co_await winrt::resume_after(std::chrono::milliseconds(20));
        }

        LogBleLine("OTA end VS-" + update_session->device_id +
                   " transfer=" + std::to_string(update_session->transfer_id) +
                   " size=" + std::to_string(update_session->image.size()));
        auto end = BleProtocol::OtaEndPayload(
            update_session->transfer_id,
            static_cast<std::uint32_t>(update_session->image.size()));
        status = co_await write_payload(end, GattWriteOption::WriteWithResponse);
        if (status != GattCommunicationStatus::Success) {
            LogBleLine("OTA end failed VS-" + update_session->device_id +
                       " status=" + GattStatusName(status));
            FinishFirmwareUpdate(update_session, false, "BLE OTA end failed: " + GattStatusName(status));
        }
    } catch (const winrt::hresult_error& error) {
        FinishFirmwareUpdate(update_session, false,
                             "BLE OTA failed: " + FormatHresult(error.code()) +
                                 ": " + winrt::to_string(error.message()));
    } catch (...) {
        FinishFirmwareUpdate(update_session, false, "BLE OTA failed.");
    }
}

void BleCentralWin::HandleFirmwareOtaStateEvent(const std::string& device_id,
                                                const FirmwareOtaStateEvent& event) {
    std::shared_ptr<FirmwareUpdateSession> update_session;
    {
        std::lock_guard lock(mutex_);
        update_session = firmware_update_session_;
    }
    if (!update_session || update_session->device_id != device_id) return;
    if (event.transfer_id.has_value() && *event.transfer_id != update_session->transfer_id) return;

    if (event.event == "progress") {
        if (event.written.has_value() && event.size.has_value() && update_session->progress) {
            update_session->device_confirmed_written.store(*event.written);
            LogBleLine("OTA device progress VS-" + device_id +
                       " written=" + std::to_string(*event.written) +
                       "/" + std::to_string(*event.size));
            update_session->progress(FirmwareUpdateProgress{
                static_cast<int>(*event.written),
                static_cast<int>(*event.size),
                true});
        }
    } else if (event.event == "done") {
        LogBleLine("OTA device done VS-" + device_id);
        if (update_session->progress) {
            update_session->progress(FirmwareUpdateProgress{
                static_cast<int>(update_session->image.size()),
                static_cast<int>(update_session->image.size()),
                true});
        }
        FinishFirmwareUpdate(update_session, true, {});
    } else if (event.event == "error") {
        LogBleLine("OTA device error VS-" + device_id +
                   " code=" + (event.code.empty() ? "unknown" : event.code));
        FinishFirmwareUpdate(update_session, false,
                             "Device rejected OTA: " + (event.code.empty() ? "unknown" : event.code));
    }
}

void BleCentralWin::FinishFirmwareUpdate(std::shared_ptr<FirmwareUpdateSession> update_session,
                                         bool success,
                                         const std::string& message) {
    if (!update_session) return;
    {
        std::lock_guard lock(mutex_);
        if (firmware_update_session_ != update_session) return;
        firmware_update_session_.reset();
    }
    if (update_session->completion) {
        DispatchToUiThread([completion = std::move(update_session->completion), success, message] {
            completion(success, message);
        });
    }
}

void BleCentralWin::HandleDeviceDisconnected(const std::string& device_id,
                                              std::shared_ptr<DeviceSession> session) {
    std::shared_ptr<DeviceSession> removed;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_by_device_id_.find(device_id);
        if (it == sessions_by_device_id_.end()) return;
        if (session && it->second != session) return;
        removed = std::move(it->second);
        sessions_by_device_id_.erase(it);
        if (removed) connecting_addresses_.erase(removed->bluetooth_address);
    }
    std::shared_ptr<FirmwareUpdateSession> update_session;
    {
        std::lock_guard lock(mutex_);
        if (firmware_update_session_ && firmware_update_session_->device_id == device_id) {
            update_session = firmware_update_session_;
        }
    }
    if (update_session) {
        FinishFirmwareUpdate(update_session, false, "Device disconnected during firmware update.");
    }
    if (removed) CloseSession(std::move(removed));
    LogBleLine("device disconnected VS-" + device_id + "; restarting scan for reconnection");
    DispatchToUiThread([this] {
        PublishConnections();
        StartScan();
    });
}

void BleCentralWin::CloseSession(std::shared_ptr<DeviceSession> session) {
    if (!session) return;
    if (session->audio_characteristic && session->audio_value_changed_token.value != 0) {
        try { session->audio_characteristic.ValueChanged(session->audio_value_changed_token); } catch (...) {}
    }
    if (session->state_characteristic && session->state_value_changed_token.value != 0) {
        try { session->state_characteristic.ValueChanged(session->state_value_changed_token); } catch (...) {}
    }
    if (session->ota_state_characteristic && session->ota_state_value_changed_token.value != 0) {
        try { session->ota_state_characteristic.ValueChanged(session->ota_state_value_changed_token); } catch (...) {}
    }
    if (session->ble_device && session->connection_status_token.value != 0) {
        try { session->ble_device.ConnectionStatusChanged(session->connection_status_token); } catch (...) {}
    }
    if (session->ble_device && session->gatt_services_changed_token.value != 0) {
        try { session->ble_device.GattServicesChanged(session->gatt_services_changed_token); } catch (...) {}
    }
    if (session->gatt_session) {
        try {
            session->gatt_session.MaintainConnection(false);
            session->gatt_session.Close();
        } catch (...) {}
        session->gatt_session = nullptr;
    }
    if (session->service) {
        try { session->service.Close(); } catch (...) {}
        session->service = nullptr;
    }
    if (session->ble_device) {
        try { session->ble_device.Close(); } catch (...) {}
        session->ble_device = nullptr;
    }
    session->audio_characteristic = nullptr;
    session->state_characteristic = nullptr;
    session->control_characteristic = nullptr;
    session->ota_rx_characteristic = nullptr;
    session->ota_state_characteristic = nullptr;
    session->ready = false;
}

void BleCentralWin::CloseSessions() {
    std::map<std::string, std::shared_ptr<DeviceSession>> sessions;
    {
        std::lock_guard lock(mutex_);
        sessions.swap(sessions_by_device_id_);
    }
    for (auto& [_, session] : sessions) {
        CloseSession(std::move(session));
    }
}

ByteVector BleCentralWin::BytesFromBuffer(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    DataReader reader = DataReader::FromBuffer(buffer);
    ByteVector bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) {
        reader.ReadBytes(bytes);
    }
    return bytes;
}

void BleCentralWin::PublishConnections() {
    if (!on_connection_change) return;
    std::vector<ConnectedDevice> devices;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [_, session] : sessions_by_device_id_) {
            if (session->ready) devices.push_back(session->device);
        }
    }
    std::sort(devices.begin(), devices.end(), [](const ConnectedDevice& lhs, const ConnectedDevice& rhs) {
        return lhs.id < rhs.id;
    });
    on_connection_change(devices);
}

} // namespace voicestick
