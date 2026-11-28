#include "ble_central_win.h"

#include "app_config.h"
#include "ble_protocol.h"
#include "log.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <random>
#include <utility>

namespace voicestick {

namespace {

using winrt::Windows::Devices::Bluetooth::BluetoothAddressType;
using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Devices::Bluetooth::BluetoothLEPreferredConnectionParameters;
using winrt::Windows::Devices::Bluetooth::BluetoothLEPreferredConnectionParametersRequest;
using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs;
using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
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
constexpr std::chrono::milliseconds kCharacteristicDiscoveryTimeout{8000};
constexpr std::chrono::milliseconds kAuthorizationReadTimeout{10000};
constexpr std::chrono::milliseconds kAuthorizationRetryDelay{500};
constexpr std::chrono::milliseconds kDeviceReopenDelay{800};
constexpr std::chrono::milliseconds kConnectionSettleDelay{300};
constexpr std::chrono::milliseconds kPairAfterConnectDelay{300};

// HRESULT_FROM_WIN32(ERROR_BAD_COMMAND): Windows surfaces this for our
// scenario when the OS thinks the device is already paired/bonded but the
// remote refuses or rolled its keys. We special-case it to suggest unpairing.
constexpr std::int32_t kErrorBadCommand = static_cast<std::int32_t>(0x80070016);
constexpr std::int32_t kErrorTimeout = static_cast<std::int32_t>(0x800705B4);

struct OtaConnectionPreference {
    BluetoothLEPreferredConnectionParametersRequest request{nullptr};

    ~OtaConnectionPreference() {
        if (request) {
            try { request.Close(); } catch (...) {}
        }
    }
};

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

// True for the pairing advertisement, false for the reconnect advertisement,
// and nullopt for reports that carry neither, such as scan responses.
std::optional<bool> AdvertisesPairingWindow(const BluetoothLEAdvertisementReceivedEventArgs& args) {
    static const winrt::guid pairing_uuid{BleProtocol::pairing_advertisement_uuid};
    static const winrt::guid reconnect_uuid{BleProtocol::reconnect_advertisement_uuid};
    bool advertises_reconnect = false;
    for (const auto& uuid : args.Advertisement().ServiceUuids()) {
        if (uuid == pairing_uuid) return true;
        if (uuid == reconnect_uuid) advertises_reconnect = true;
    }
    using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementType;
    // When an already-bonded device opens pairing, its scan response contains
    // the reconnect UUID alongside the device name. Only the primary
    // advertisement can say that the pairing window is closed.
    if (advertises_reconnect && args.AdvertisementType() != BluetoothLEAdvertisementType::ScanResponse) {
        return false;
    }
    return std::nullopt;
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

std::string TruncateUtf8(std::string_view text, std::size_t max_bytes = 80) {
    if (text.size() <= max_bytes) return std::string(text);
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return std::string(text.substr(0, end));
}

} // namespace

BleCentralWin::BleCentralWin(std::vector<std::string> paired_device_ids, HWND dispatch_hwnd)
    : dispatch_hwnd_(dispatch_hwnd),
      paired_device_ids_(paired_device_ids.begin(), paired_device_ids.end()) {}

void BleCentralWin::DispatchToUiThread(std::function<void()> callback) {
    if (shutting_down_) return;
    if (!dispatch_hwnd_) {
        callback();
        return;
    }
    {
        std::lock_guard lock(dispatch_mutex_);
        if (shutting_down_) return;
        dispatch_queue_.push(std::move(callback));
    }
    PostMessage(dispatch_hwnd_, WM_BLE_DISPATCH, 0, 0);
}

void BleCentralWin::ProcessDispatchedCallbacks() {
    const auto self = weak_from_this().lock();
    std::queue<std::function<void()>> pending;
    {
        std::lock_guard lock(dispatch_mutex_);
        pending.swap(dispatch_queue_);
    }
    while (!pending.empty() && !shutting_down_) {
        pending.front()();
        pending.pop();
    }
}

BleCentralWin::~BleCentralWin() {
    Shutdown();
}

void BleCentralWin::Start() {
    WatchBluetoothRadioAsync();
    StartScan();
}

winrt::fire_and_forget BleCentralWin::WatchBluetoothRadioAsync() {
    using winrt::Windows::Devices::Bluetooth::BluetoothAdapter;
    using winrt::Windows::Devices::Radios::Radio;
    using winrt::Windows::Devices::Radios::RadioState;
    std::weak_ptr<BleCentralWin> weak_self = weak_from_this();
    try {
        auto adapter = co_await BluetoothAdapter::GetDefaultAsync();
        if (!adapter) {
            LogBleLine("radio watch: no Bluetooth adapter");
            co_return;
        }
        auto radio = co_await adapter.GetRadioAsync();
        auto self = weak_self.lock();
        if (!radio || !self || self->shutting_down_) co_return;
        std::lock_guard lock(self->radio_mutex_);
        self->radio_state_token_ = radio.StateChanged(
            [weak_self](const Radio& sender, const winrt::Windows::Foundation::IInspectable&) {
                const bool on = sender.State() == RadioState::On;
                LogBleLine(std::string("bluetooth radio ") + (on ? "on" : "off"));
                if (on) RestartScanAfterRadioOn(weak_self);
            });
        self->bluetooth_radio_ = radio;
    } catch (const winrt::hresult_error& error) {
        LogBleLine("radio watch unavailable hr=" + FormatHresult(error.code()));
    } catch (...) {
        LogBleLine("radio watch unavailable: unknown error");
    }
}

winrt::fire_and_forget BleCentralWin::RestartScanAfterRadioOn(std::weak_ptr<BleCentralWin> weak_self) {
    // The LE stack keeps reporting "device not ready" for a moment after the radio turns on.
    co_await winrt::resume_after(std::chrono::seconds(2));
    auto self = weak_self.lock();
    if (!self || self->shutting_down_) co_return;
    self->DispatchToUiThread([weak_self] {
        if (auto self = weak_self.lock()) {
            LogBleLine("restarting scan after Bluetooth turned on");
            self->StartScan();
        }
    });
}

void BleCentralWin::Shutdown() {
    if (shutting_down_.exchange(true)) return;
    {
        std::lock_guard lock(radio_mutex_);
        if (bluetooth_radio_) {
            try {
                bluetooth_radio_.StateChanged(radio_state_token_);
            } catch (...) {
            }
            bluetooth_radio_ = nullptr;
        }
    }
    StopScan();
    {
        std::lock_guard lock(mutex_);
        connecting_addresses_.clear();
        cancelled_device_ids_.clear();
    }
    CancelFirmwareUpdate();
    CloseSessions();
    {
        std::lock_guard lock(dispatch_mutex_);
        dispatch_queue_ = {};
    }
}

void BleCentralWin::SetPairingDialogOpen(bool open) {
    std::lock_guard lock(mutex_);
    pairing_dialog_open_ = open;
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
    StartDirectConnect(device_id, bluetooth_address, address_kind, name, false);
}

void BleCentralWin::PairDevice(const std::string& device_id,
                               std::uint64_t bluetooth_address,
                               BluetoothAddressKind address_kind,
                               const std::string& name) {
    StartDirectConnect(device_id, bluetooth_address, address_kind, name, true);
}

void BleCentralWin::StartDirectConnect(const std::string& device_id,
                                       std::uint64_t bluetooth_address,
                                       BluetoothAddressKind address_kind,
                                       const std::string& name,
                                       bool pairing) {
    {
        std::lock_guard lock(mutex_);
        if (!pairing && pairing_dialog_open_) return;
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
    LogBleLine(std::string(pairing ? "pairing" : "direct") + " connect requested VS-" + device_id +
               " address=" + FormatBluetoothAddress(bluetooth_address) +
               " kind=" + AddressKindName(address_kind));
    ConnectDeviceAsync(bluetooth_address, address_kind,
                       name.empty() ? "VS-" + device_id : name, device_id, pairing);
}

void BleCentralWin::SetDisplay(const std::string& scene,
                               const std::string& text,
                               const std::optional<std::string>& device_id) {
    const auto trimmed = TruncateUtf8(text, 80);
    std::vector<std::pair<std::string, std::shared_ptr<DeviceSession>>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready && it->second->initialized &&
                it->second->info && it->second->info->has_display) {
                targets.emplace_back(it->first, it->second);
            }
        } else {
            for (const auto& [id, session] : sessions_by_device_id_) {
                if (session->ready && session->initialized &&
                    session->info && session->info->has_display) {
                    targets.emplace_back(id, session);
                }
            }
        }
    }
    for (auto& [id, session] : targets) {
        {
            std::lock_guard lock(mutex_);
            if (session->last_display_scene == scene && session->last_display_text == trimmed) {
                continue;
            }
            session->last_display_scene = scene;
            session->last_display_text = trimmed;
        }
        SendRpc(id, session, [scene, trimmed](std::uint32_t rpc_id) {
            return BleProtocol::RpcDisplaySet(rpc_id, scene, trimmed);
        }, [this, session, scene, trimmed](std::optional<ControlRpcResponse> resp) {
            if (!resp || resp->is_error) {
                std::lock_guard lock(mutex_);
                if (session->last_display_scene == scene && session->last_display_text == trimmed) {
                    session->last_display_scene.reset();
                    session->last_display_text.reset();
                }
            }
        });
    }
}

void BleCentralWin::StartAudioIn(const std::string& device_id, InteractionMode mode) {
    std::shared_ptr<DeviceSession> session;
    std::uint16_t previous_stream_id = 0;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_by_device_id_.find(device_id);
        if (it == sessions_by_device_id_.end() || !it->second->ready || !it->second->initialized) return;
        session = it->second;
        previous_stream_id = session->audio_in_stream_id;
        session->audio_in_stream_id = 0;
    }
    const std::string trigger = mode == InteractionMode::kHoldToTalk ? "button_hold" : "button_toggle";
    auto start_new = [this, device_id, session, trigger] {
        SendRpc(device_id, session, [trigger](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamStartAudioIn(rpc_id, trigger);
        }, [this, device_id, session](std::optional<ControlRpcResponse> resp) {
            if (!resp || resp->is_error || !resp->stream_id) {
                LogBleLine("stream.start(audio_in) failed VS-" + device_id +
                           (resp && resp->is_error ? " code=" + resp->error_code : " timeout"));
                return;
            }
            std::lock_guard lock(mutex_);
            session->audio_in_stream_id = *resp->stream_id;
        });
    };
    if (previous_stream_id != 0) {
        SendRpc(device_id, session, [previous_stream_id](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamStop(rpc_id, previous_stream_id);
        }, [start_new = std::move(start_new)](std::optional<ControlRpcResponse>) {
            start_new();
        });
    } else {
        start_new();
    }
}

void BleCentralWin::StopAudioIn(const std::string& device_id) {
    std::shared_ptr<DeviceSession> session;
    std::uint16_t stream_id = 0;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_by_device_id_.find(device_id);
        if (it == sessions_by_device_id_.end() || !it->second->ready || !it->second->initialized) return;
        session = it->second;
        stream_id = session->audio_in_stream_id;
        session->audio_in_stream_id = 0;
    }
    if (stream_id != 0) {
        SendRpc(device_id, session, [stream_id](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamStop(rpc_id, stream_id);
        });
    }
}

void BleCentralWin::StartImu(const std::string& device_id,
                             const std::string& axes,
                             int rate_hz,
                             int lease_ms) {
    std::shared_ptr<DeviceSession> session;
    std::uint16_t previous_stream_id = 0;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_by_device_id_.find(device_id);
        if (it == sessions_by_device_id_.end() || !it->second->ready || !it->second->initialized) return;
        session = it->second;
        previous_stream_id = session->imu_stream_id;
        session->imu_stream_id = 0;
    }
    auto start_new = [this, device_id, session, axes, rate_hz, lease_ms] {
        SendRpc(device_id, session, [axes, rate_hz, lease_ms](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamStartImu(rpc_id, axes, rate_hz, lease_ms);
        }, [this, device_id, session](std::optional<ControlRpcResponse> resp) {
            if (!resp || resp->is_error || !resp->stream_id) {
                LogBleLine("stream.start(imu) failed VS-" + device_id +
                           (resp && resp->is_error ? " code=" + resp->error_code : " timeout"));
                return;
            }
            std::lock_guard lock(mutex_);
            session->imu_stream_id = *resp->stream_id;
        });
    };
    if (previous_stream_id != 0) {
        SendRpc(device_id, session, [previous_stream_id](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamStop(rpc_id, previous_stream_id);
        }, [start_new = std::move(start_new)](std::optional<ControlRpcResponse>) {
            start_new();
        });
    } else {
        start_new();
    }
}

void BleCentralWin::RenewImu(const std::string& device_id, int lease_ms) {
    std::shared_ptr<DeviceSession> session;
    std::uint16_t stream_id = 0;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_by_device_id_.find(device_id);
        if (it == sessions_by_device_id_.end() || !it->second->ready || !it->second->initialized) return;
        session = it->second;
        stream_id = session->imu_stream_id;
    }
    if (stream_id != 0) {
        SendRpc(device_id, session, [stream_id, lease_ms](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamRenew(rpc_id, stream_id, lease_ms);
        });
    }
}

void BleCentralWin::StopImu(const std::string& device_id) {
    std::shared_ptr<DeviceSession> session;
    std::uint16_t stream_id = 0;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_by_device_id_.find(device_id);
        if (it == sessions_by_device_id_.end() || !it->second->ready || !it->second->initialized) return;
        session = it->second;
        stream_id = session->imu_stream_id;
        session->imu_stream_id = 0;
    }
    if (stream_id != 0) {
        SendRpc(device_id, session, [stream_id](std::uint32_t rpc_id) {
            return BleProtocol::RpcStreamStop(rpc_id, stream_id);
        });
    }
}

std::vector<std::string> BleCentralWin::ConnectedReadyDeviceIds() const {
    std::vector<std::string> ids;
    std::lock_guard lock(mutex_);
    for (const auto& [id, session] : sessions_by_device_id_) {
        if (session->ready && session->initialized) ids.push_back(id);
    }
    return ids;
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
    std::shared_ptr<FirmwareUpdateSession> update_session;
    {
        std::lock_guard lock(mutex_);
        if (firmware_update_session_) {
            completion(false, "A firmware update is already running.");
            return;
        }
        auto it = sessions_by_device_id_.find(device_id);
        if (shutting_down_ || it == sessions_by_device_id_.end() || !it->second->ready) {
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
        update_session = std::make_shared<FirmwareUpdateSession>();
        update_session->device_id = device_id;
        update_session->transfer_id = RandomTransferId();
        update_session->image = std::move(image);
        update_session->progress = std::move(progress);
        update_session->completion = std::move(completion);
        firmware_update_session_ = update_session;
    }
    if (update_session->progress) {
        update_session->progress(FirmwareUpdateProgress{
            0, static_cast<int>(update_session->image.size()), true});
    }
    DispatchToUiThread([this, session = std::move(session), update_session = std::move(update_session)] {
        if (update_session->finished) return;
        StopScan();
        UpdateFirmwareAsync(session, update_session);
    });
}

void BleCentralWin::CancelFirmwareUpdate() {
    std::shared_ptr<FirmwareUpdateSession> update_session;
    std::shared_ptr<DeviceSession> device_session;
    {
        std::lock_guard lock(mutex_);
        update_session = firmware_update_session_;
        if (!update_session) return;
        update_session->finished = true;
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
    if (shutting_down_) return;
    const auto self = weak_from_this().lock();
    bool can_scan = false;
    bool has_paired_devices = false;
    {
        std::lock_guard lock(mutex_);
        can_scan = !firmware_update_session_;
        has_paired_devices = !paired_device_ids_.empty();
    }
    if (!can_scan || !has_paired_devices) StopScan();
    if (!can_scan) return;
    if (!has_paired_devices) {
        LogBleLine("scan skipped: no paired devices");
        PublishConnections();
        return;
    }
    // No AdvertisementFilter here: the firmware puts its 128-bit service UUID
    // in the ADV PDU but the LocalName "VS-XXXX" only in the scan response,
    // and WinRT's per-PDU filter would drop the scan response so we'd never
    // see the device id. Filter on device_id in HandleAdvertisement instead.
    try {
        if (scan_.Start([weak_self = weak_from_this()](const auto& watcher, const auto& args) {
                if (auto self = weak_self.lock(); self && !self->shutting_down_) {
                    self->HandleAdvertisement(watcher, args);
                }
            })) {
            LogBleLine("scan started");
        }
    } catch (const winrt::hresult_error& error) {
        const auto message = ScanStartFailureMessage(error);
        LogBleLine("scan start failed: " + message);
        PublishConnections();
        if (on_scan_error) on_scan_error(message);
    } catch (...) {
        const std::string message = "Bluetooth LE scan failed with an unknown error.";
        LogBleLine("scan start failed: unknown error");
        PublishConnections();
        if (on_scan_error) on_scan_error(message);
    }
}

void BleCentralWin::StopScan() {
    scan_.Stop();
}

void BleCentralWin::HandleAdvertisement(const BluetoothLEAdvertisementWatcher&,
                                        const BluetoothLEAdvertisementReceivedEventArgs& args) {
    const auto bluetooth_address = args.BluetoothAddress();
    if (const auto pairing_window = AdvertisesPairingWindow(args)) {
        std::lock_guard lock(mutex_);
        if (*pairing_window) {
            pairing_window_addresses_.insert(bluetooth_address);
        } else {
            pairing_window_addresses_.erase(bluetooth_address);
        }
    }

    const auto local_name = Utf8FromHstring(args.Advertisement().LocalName());
    auto device_id = BleProtocol::DeviceIdFromName(local_name);
    if (!device_id.has_value()) return;

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
        if (pairing_dialog_open_ || !paired_device_ids_.contains(*device_id)) return;
        if (sessions_by_device_id_.contains(*device_id)) return;
        if (connecting_addresses_.contains(bluetooth_address)) return;
        // A device in its pairing window is waiting for the pairing dialog.
        if (pairing_window_addresses_.contains(bluetooth_address)) return;
        connecting_addresses_.insert(bluetooth_address);
    }

    LogBleLine("advertisement matched VS-" + *device_id + " address=" +
               FormatBluetoothAddress(bluetooth_address) +
               " kind=" + AddressKindName(address_kind));
    ConnectDeviceAsync(bluetooth_address, address_kind, local_name, *device_id, false);
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

bool IsPairingSuccess(winrt::Windows::Devices::Enumeration::DevicePairingResultStatus status) {
    using winrt::Windows::Devices::Enumeration::DevicePairingResultStatus;
    return status == DevicePairingResultStatus::Paired || status == DevicePairingResultStatus::AlreadyPaired;
}

// Pairs without showing Windows UI; the device has no input or output.
winrt::Windows::Foundation::IAsyncOperation<bool> TryCustomPairAsync(
    winrt::Windows::Devices::Enumeration::DeviceInformationPairing pairing) {
    using winrt::Windows::Devices::Enumeration::DeviceInformationCustomPairing;
    using winrt::Windows::Devices::Enumeration::DevicePairingKinds;
    using winrt::Windows::Devices::Enumeration::DevicePairingProtectionLevel;
    using winrt::Windows::Devices::Enumeration::DevicePairingRequestedEventArgs;
    try {
        auto custom = pairing.Custom();
        auto token = custom.PairingRequested(
            [](const DeviceInformationCustomPairing&, const DevicePairingRequestedEventArgs& args) {
                if (args.PairingKind() == DevicePairingKinds::ConfirmOnly) args.Accept();
            });
        auto result = co_await custom.PairAsync(DevicePairingKinds::ConfirmOnly,
                                                DevicePairingProtectionLevel::Encryption);
        custom.PairingRequested(token);
        const auto status = result.Status();
        LogBleLine("Windows custom pair: result=" + std::to_string(static_cast<int>(status)));
        co_return IsPairingSuccess(status);
    } catch (const winrt::hresult_error& error) {
        LogBleLine("Windows custom pair: exception hr=" + FormatHresult(error.code()));
    }
    co_return false;
}

// Pairs without Windows UI first and falls back to the system pairing dialog.
winrt::Windows::Foundation::IAsyncOperation<bool> TryPairAsync(BluetoothLEDevice device) {
    using winrt::Windows::Devices::Enumeration::DevicePairingProtectionLevel;
    try {
        auto info = device.DeviceInformation();
        if (!info) {
            LogBleLine("pair: DeviceInformation not found");
            co_return false;
        }
        const auto pairing = info.Pairing();
        if (co_await TryCustomPairAsync(pairing)) co_return true;
        if (pairing.IsPaired()) co_return true;

        co_await WaitMs(kDeviceReopenDelay);
        const auto system_result = co_await pairing.PairAsync(DevicePairingProtectionLevel::Encryption);
        const auto system_status = system_result.Status();
        LogBleLine("Windows system pair: result=" + std::to_string(static_cast<int>(system_status)));
        co_return IsPairingSuccess(system_status);
    } catch (const winrt::hresult_error& error) {
        LogBleLine("pair: exception hr=" + FormatHresult(error.code()));
        co_return false;
    } catch (...) {
        LogBleLine("pair: unknown exception");
        co_return false;
    }
}

// The device only returns 0x01 after bonding and authorizing this host.
winrt::Windows::Foundation::IAsyncOperation<bool> ReadAuthorized(
    GattCharacteristic characteristic, std::string device_id, bool pairing) {
    using winrt::Windows::Foundation::AsyncStatus;
    constexpr uint8_t kAttInsufficientAuthentication = 0x05;
    constexpr uint8_t kAttInsufficientEncryption = 0x0f;
    const auto deadline = std::chrono::steady_clock::now() + kAuthorizationReadTimeout;
    try {
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            auto op = characteristic.ReadValueAsync(BluetoothCacheMode::Uncached);
            if (op.wait_for(remaining) == AsyncStatus::Started) {
                op.Cancel();
                break;
            }
            auto result = op.GetResults();
            if (result.Status() == GattCommunicationStatus::Success) {
                auto reader = DataReader::FromBuffer(result.Value());
                const bool authorized = reader.UnconsumedBufferLength() == 1 && reader.ReadByte() == 1;
                LogBleLine("authorized read VS-" + device_id + " value=" + (authorized ? "1" : "invalid"));
                co_return authorized;
            }
            const auto error = result.ProtocolError();
            LogBleLine("authorized read VS-" + device_id + " status=" + GattStatusName(result.Status()) +
                       (error ? " ATT error=" + std::to_string(error.Value()) : ""));
            if (!pairing || !error || (error.Value() != kAttInsufficientAuthentication &&
                                        error.Value() != kAttInsufficientEncryption)) {
                co_return false;
            }
            // The peripheral can start security after GATT is already available.
            co_await WaitMs(kAuthorizationRetryDelay);
        }
        LogBleLine("authorized read timed out VS-" + device_id);
    } catch (const winrt::hresult_error& error) {
        LogBleLine("authorized read VS-" + device_id + " hr=" + FormatHresult(error.code()));
    }
    co_return false;
}

} // namespace

winrt::fire_and_forget BleCentralWin::ConnectDeviceAsync(std::uint64_t bluetooth_address,
                                                         BluetoothAddressKind address_kind,
                                                         std::string local_name,
                                                         std::string device_id,
                                                         bool pairing) {
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
        {
            std::lock_guard lock(mutex_);
            connecting_addresses_.erase(bluetooth_address);
            cancelled_device_ids_.erase(device_id);
        }
        LogBleLine("connect failed VS-" + device_id + " address=" +
                   FormatBluetoothAddress(bluetooth_address) + " reason=" + message);
        if (message != "cancelled") {
            DispatchToUiThread([this, device_id, message] {
                if (on_connection_error) on_connection_error(device_id, message);
            });
        }
    };

    const auto cancelled = [this, &device_id, pairing] {
        std::lock_guard lock(mutex_);
        return shutting_down_ || cancelled_device_ids_.contains(device_id) ||
               (pairing_dialog_open_ && !pairing);
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
        if (cancelled()) {
            fail("cancelled");
            co_return;
        }
        const auto address_type = ToBluetoothAddressType(address_kind);
        LogBleLine("connecting VS-" + device_id + " address=" + FormatBluetoothAddress(bluetooth_address) +
                   " kind=" + AddressKindName(address_kind));

        // Start pairing from a clean Windows bond. If the device lost its
        // bond, Windows would otherwise encrypt with a key the device no
        // longer has; the device replaces its own old bond in the pairing
        // window.
        if (pairing) {
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
        bool system_paired = false;
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
        constexpr int kConnectionPollIntervalMs = 100;
        constexpr int kConnectionPollAttempts = 40;
        for (int poll = 0; poll < kConnectionPollAttempts; ++poll) {
            co_await WaitMs(std::chrono::milliseconds(kConnectionPollIntervalMs));
            if (cancelled()) {
                fail("cancelled");
                co_return;
            }
            if (session->ble_device.ConnectionStatus() == BluetoothConnectionStatus::Connected) {
                LogBleLine("link-layer connected VS-" + device_id +
                           " after " + std::to_string((poll + 1) * kConnectionPollIntervalMs) + "ms");
                break;
            }
        }
        // Pair on the open link, before reading an encrypted characteristic
        // and before the device sends its own security request. The device
        // rejects a pairing request from a host it still has keys for if it
        // arrives right after the link comes up.
        if (pairing) {
            co_await WaitMs(kPairAfterConnectDelay);
            LogBleLine("starting Windows pairing VS-" + device_id);
            system_paired = co_await TryPairAsync(session->ble_device);
        }

        auto pre_status = session->ble_device.ConnectionStatus();
        LogBleLine("pre-discovery status VS-" + device_id + " = " +
                   (pre_status == BluetoothConnectionStatus::Connected ? "connected" : "disconnected") +
                   " max_pdu_size=" + (session->gatt_session
                       ? std::to_string(session->gatt_session.MaxPduSize()) : "n/a"));

        using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceServicesResult;
        GattDeviceServicesResult service_result{nullptr};
        const winrt::guid wanted{BleProtocol::service_uuid};
        for (int attempt = 1; attempt <= kServiceDiscoveryAttempts; ++attempt) {
            if (cancelled()) {
                fail("cancelled");
                co_return;
            }
            const bool use_cache = pairing ? attempt == kServiceDiscoveryAttempts : attempt == 1;
            const auto cache_mode = use_cache ? BluetoothCacheMode::Cached : BluetoothCacheMode::Uncached;

            std::int32_t throw_hresult = 0;
            std::string throw_message;
            try {
                auto async_op = session->ble_device.GetGattServicesForUuidAsync(wanted, cache_mode);
                constexpr int kPollMs = 500;
                const int max_polls = static_cast<int>(kServiceDiscoveryTimeout.count()) / kPollMs;
                for (int p = 0; p < max_polls; ++p) {
                    co_await WaitMs(std::chrono::milliseconds(kPollMs));
                    if (async_op.Status() != winrt::Windows::Foundation::AsyncStatus::Started) break;
                    if (cancelled()) {
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
                if (attempt < kServiceDiscoveryAttempts) {
                    // Tear the device object down completely and re-open it.
                    // Reusing a BluetoothLEDevice that has already returned
                    // 0x80070016 keeps producing the same error indefinitely;
                    // a fresh handle re-runs the OS connection state machine.
                    LogBleLine("recycling BluetoothLEDevice VS-" + device_id);
                    detach_device_handlers(session);
                    if (session->gatt_session) {
                        try {
                            session->gatt_session.MaintainConnection(false);
                            session->gatt_session.Close();
                        } catch (...) {
                        }
                        session->gatt_session = nullptr;
                    }
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

        auto read_characteristics = [&](BluetoothCacheMode mode) -> GattCharsResult {
            auto op = session->service.GetCharacteristicsAsync(mode);
            if (op.wait_for(kCharacteristicDiscoveryTimeout) == AsyncStatus::Started) {
                op.Cancel();
                return nullptr;
            }
            return op.GetResults();
        };

        auto cache_mode = pairing ? BluetoothCacheMode::Uncached : BluetoothCacheMode::Cached;
        auto characteristics = read_characteristics(cache_mode);
        const auto find_characteristic = [&characteristics](const winrt::guid& wanted) -> GattCharacteristic {
            if (!characteristics || characteristics.Status() != GattCommunicationStatus::Success) return nullptr;
            for (const auto& characteristic : characteristics.Characteristics()) {
                if (characteristic.Uuid() == wanted) return characteristic;
            }
            return nullptr;
        };

        if (cache_mode == BluetoothCacheMode::Cached &&
            (!find_characteristic(winrt::guid{BleProtocol::stream_tx_uuid}) ||
             !find_characteristic(winrt::guid{BleProtocol::control_tx_uuid}) ||
             !find_characteristic(winrt::guid{BleProtocol::control_rx_uuid}) ||
             !find_characteristic(winrt::guid{BleProtocol::authorized_uuid}))) {
            cache_mode = BluetoothCacheMode::Uncached;
            characteristics = read_characteristics(cache_mode);
        }
        const auto discovery_status = characteristics ? GattStatusName(characteristics.Status()) : "timeout";
        LogBleLine("characteristic discovery VS-" + device_id + " mode=" +
                   (cache_mode == BluetoothCacheMode::Cached ? "cached" : "uncached") +
                   " status=" + discovery_status);
        if (!characteristics || characteristics.Status() != GattCommunicationStatus::Success) {
            fail("VoiceStick GATT characteristic discovery failed: " + discovery_status);
            co_return;
        }

        session->stream_tx_characteristic = find_characteristic(winrt::guid{BleProtocol::stream_tx_uuid});
        session->control_tx_characteristic = find_characteristic(winrt::guid{BleProtocol::control_tx_uuid});
        session->control_rx_characteristic = find_characteristic(winrt::guid{BleProtocol::control_rx_uuid});
        if (!session->stream_tx_characteristic || !session->control_tx_characteristic ||
            !session->control_rx_characteristic || !HasNotify(session->stream_tx_characteristic) ||
            !HasNotify(session->control_tx_characteristic) ||
            (!HasWriteWithoutResponse(session->control_rx_characteristic) &&
             !HasWrite(session->control_rx_characteristic))) {
            fail("required GATT characteristics or properties are missing");
            co_return;
        }

        session->ota_rx_characteristic = find_characteristic(winrt::guid{BleProtocol::ota_rx_uuid});
        session->ota_state_characteristic = find_characteristic(winrt::guid{BleProtocol::ota_state_uuid});
        if (session->ota_rx_characteristic &&
            !HasWrite(session->ota_rx_characteristic) &&
            !HasWriteWithoutResponse(session->ota_rx_characteristic)) {
            session->ota_rx_characteristic = nullptr;
        }
        if (session->ota_state_characteristic && !HasNotify(session->ota_state_characteristic)) {
            session->ota_state_characteristic = nullptr;
        }

        // Firmware without the characteristic predates the pairing window.
        if (auto authorized = find_characteristic(winrt::guid{BleProtocol::authorized_uuid})) {
            const bool authorized_link = co_await ReadAuthorized(authorized, device_id, pairing);
            if (cancelled()) {
                fail("cancelled");
                co_return;
            }
            if (!authorized_link) {
                fail(pairing
                         ? "VS-" + device_id + " did not authorize this PC. Press the main voice "
                           "button while the device is disconnected, then pair again."
                         : "Could not verify Bluetooth authorization for VS-" + device_id +
                           ". VoiceStick will retry the connection.");
                co_return;
            }
        }

        // Windows can complete bonding when the encrypted characteristic is
        // read, even after the explicit pairing request returned a failure.
        if (pairing && !system_paired && !session->ble_device.DeviceInformation().Pairing().IsPaired()) {
            fail("Windows did not finish pairing VS-" + device_id + ". Open the device's pairing window and try again.");
            co_return;
        }

        session->stream_tx_value_changed_token = session->stream_tx_characteristic.ValueChanged(
            [this, device_id, weak_session = std::weak_ptr<DeviceSession>(session)](
                const GattCharacteristic&, const auto& args) {
                auto bytes = BytesFromBuffer(args.CharacteristicValue());
                if (bytes.size() < 4 || bytes[0] != BleProtocol::protocol_version) return;
                const auto type = bytes[1];
                if (type == BleProtocol::type_nop) {
                    return;
                }
                if (type == BleProtocol::type_audio_in) {
                    auto frame = BleProtocol::ParseAudioStreamFrame(bytes);
                    if (!frame) return;
                    DispatchToUiThread([this, device_id, weak_session, f = std::move(*frame)]() {
                        const auto current = weak_session.lock();
                        if (!current) return;
                        {
                            std::lock_guard lock(mutex_);
                            if (current->audio_in_stream_id != 0 && current->audio_in_stream_id != f.stream_id) {
                                return;
                            }
                            if (f.IsStart()) {
                                current->last_display_scene = "recording";
                                current->last_display_text = "";
                            }
                        }
                        if (on_audio_frame) on_audio_frame(device_id, f);
                    });
                } else if (type == BleProtocol::type_imu) {
                    auto frame = BleProtocol::ParseImuStreamFrame(bytes);
                    if (!frame) return;
                    DispatchToUiThread([this, device_id, weak_session, f = std::move(*frame)]() {
                        const auto current = weak_session.lock();
                        if (!current) return;
                        {
                            std::lock_guard lock(mutex_);
                            if (current->imu_stream_id != 0 && current->imu_stream_id != f.stream_id) {
                                return;
                            }
                        }
                        if (on_imu_frame) on_imu_frame(device_id, f);
                    });
                } else if (type == BleProtocol::type_input) {
                    auto frame = BleProtocol::ParseInputStreamFrame(bytes);
                    if (!frame) return;
                    DispatchToUiThread([this, device_id, weak_session, f = std::move(*frame)]() {
                        const auto current = weak_session.lock();
                        if (!current) return;
                        HandleInputFrame(device_id, current, f);
                    });
                }
            });
        session->control_tx_value_changed_token = session->control_tx_characteristic.ValueChanged(
            [this, device_id, weak_session = std::weak_ptr<DeviceSession>(session)](
                const GattCharacteristic&, const auto& args) {
                auto bytes = BytesFromBuffer(args.CharacteristicValue());
                LogBleLine("control notify VS-" + device_id +
                           " len=" + std::to_string(bytes.size()) +
                           " preview=" + PreviewBytes(bytes));
                if (bytes.size() >= 4 && bytes[0] == 1 && bytes[1] == 0x10) {
                    if (auto legacy = BleProtocol::ParseLegacyDeviceInfo(bytes)) {
                        if (const auto current = weak_session.lock();
                            current && current->ready && !current->legacy_ready_sent.exchange(true)) {
                            WriteControlFramesAsync(current, {BleProtocol::LegacyUiStateReadyPayload()});
                        }
                        DispatchToUiThread([this, device_id, info = std::move(*legacy)]() {
                            if (on_legacy_device_info) on_legacy_device_info(device_id, info);
                        });
                    }
                    return;
                }
                std::optional<std::string> json;
                if (const auto current = weak_session.lock()) {
                    std::lock_guard lock(mutex_);
                    json = current->control_reassembler.Push(bytes);
                }
                if (!json) return;
                auto message = BleProtocol::ParseControlMessage(*json);
                if (!message) {
                    LogBleLine("control notify VS-" + device_id + " parse failed");
                    return;
                }
                DispatchToUiThread([this, device_id, weak_session, msg = std::move(*message)]() mutable {
                    const auto current = weak_session.lock();
                    if (!current) return;
                    HandleControlMessage(device_id, current, std::move(msg));
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

        co_await WaitMs(std::chrono::milliseconds(150));

        LogBleLine("subscribing control_tx notifications VS-" + device_id);
        auto control_subscribe = co_await session->control_tx_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        LogBleLine("control_tx subscribe VS-" + device_id +
                   " status=" + GattStatusName(control_subscribe));

        co_await WaitMs(std::chrono::milliseconds(150));

        LogBleLine("subscribing stream_tx notifications VS-" + device_id);
        auto stream_subscribe = co_await session->stream_tx_characteristic
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify);
        LogBleLine("stream_tx subscribe VS-" + device_id +
                   " status=" + GattStatusName(stream_subscribe));

        GattCommunicationStatus ota_subscribe = GattCommunicationStatus::Success;
        if (session->ota_state_characteristic) {
            ota_subscribe = co_await session->ota_state_characteristic
                .WriteClientCharacteristicConfigurationDescriptorAsync(
                    GattClientCharacteristicConfigurationDescriptorValue::Notify);
            LogBleLine("ota state subscribe VS-" + device_id +
                       " status=" + GattStatusName(ota_subscribe));
        }

        if (stream_subscribe != GattCommunicationStatus::Success ||
            control_subscribe != GattCommunicationStatus::Success ||
            ota_subscribe != GattCommunicationStatus::Success) {
            fail("notification subscription failed: stream=" + GattStatusName(stream_subscribe) +
                 " control=" + GattStatusName(control_subscribe) +
                 " ota=" + GattStatusName(ota_subscribe));
            co_return;
        }

        session->stream_tx_subscribed = true;
        session->control_tx_subscribed = true;
        session->ota_state_subscribed = session->ota_state_characteristic != nullptr;
        session->device.supports_firmware_update =
            session->ota_rx_characteristic && session->ota_state_subscribed;

        DispatchToUiThread([this, session, device_id, bluetooth_address] {
            {
                std::lock_guard lock(mutex_);
                session->ready = true;
                sessions_by_device_id_[device_id] = session;
                connecting_addresses_.erase(bluetooth_address);
            }
            PublishConnections();
            LogBleLine("connected VS-" + device_id + "; starting Protocol v2 handshake");
            StartV2Handshake(device_id, session);
        });
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

void BleCentralWin::StartV2Handshake(const std::string& device_id, std::shared_ptr<DeviceSession> session) {
    SendRpc(device_id, session, [](std::uint32_t rpc_id) {
        return BleProtocol::RpcDeviceGetInfo(rpc_id);
    }, [this, device_id, session](std::optional<ControlRpcResponse> resp) {
        if (!resp || resp->is_error || !resp->device_info || !resp->device_info->SupportsProtocolV2()) {
            LogBleLine("v2 device.get_info fallback to legacy probe VS-" + device_id);
            ProbeLegacyDeviceInfo(device_id, session);
            return;
        }
        auto info = *resp->device_info;
        {
            std::lock_guard lock(mutex_);
            session->info = info;
        }
        if (on_device_info) on_device_info(device_id, info);
        if (info.battery && on_battery_changed) on_battery_changed(device_id, *info.battery);

        SendRpc(device_id, session, [](std::uint32_t rpc_id) {
            return BleProtocol::RpcDeviceReset(rpc_id);
        }, [this, device_id, session, info](std::optional<ControlRpcResponse> reset_resp) {
            if (!reset_resp || reset_resp->is_error) {
                LogBleLine("v2 device.reset failed VS-" + device_id);
                return;
            }
            SendRpc(device_id, session, [](std::uint32_t rpc_id) {
                return BleProtocol::RpcStreamStartInput(rpc_id);
            }, [this, device_id, session, info](std::optional<ControlRpcResponse> input_resp) {
                if (!input_resp || input_resp->is_error || !input_resp->stream_id) {
                    LogBleLine("v2 stream.start(input) failed VS-" + device_id);
                    return;
                }
                {
                    std::lock_guard lock(mutex_);
                    session->input_stream_id = *input_resp->stream_id;
                    session->last_input_seq.reset();
                    session->buttons_mask = input_resp->buttons_mask.value_or(0);
                    session->initialized = true;
                }
                LogBleLine("v2 handshake ready VS-" + device_id +
                           " input_stream=" + std::to_string(*input_resp->stream_id));
                if (on_device_ready) on_device_ready(device_id, info);
            });
        });
    }, std::chrono::milliseconds(1500));
}

void BleCentralWin::ProbeLegacyDeviceInfo(const std::string& device_id, std::shared_ptr<DeviceSession> session) {
    if (!session || !session->ready) return;
    std::vector<ByteVector> frames = {BleProtocol::LegacyGetDeviceInfoPayload()};
    if (!session->legacy_ready_sent.exchange(true)) {
        frames.push_back(BleProtocol::LegacyUiStateReadyPayload());
    }
    WriteControlFramesAsync(std::move(session), std::move(frames));
    LogBleLine("legacy get_device_info sent VS-" + device_id);
}

void BleCentralWin::SendRpc(const std::string& device_id,
                            std::shared_ptr<DeviceSession> session,
                            std::function<std::string(std::uint32_t)> build_json,
                            RpcCallback completion,
                            std::chrono::milliseconds timeout) {
    if (!session || !session->ready || !session->control_rx_characteristic) {
        if (completion) completion(std::nullopt);
        return;
    }
    std::uint32_t rpc_id = 0;
    std::vector<ByteVector> frames;
    {
        std::lock_guard lock(mutex_);
        rpc_id = session->next_rpc_id++;
        if (session->next_rpc_id == 0) session->next_rpc_id = 1;
        const std::size_t max_pdu = session->gatt_session ? session->gatt_session.MaxPduSize() : 23;
        const std::size_t max_write = max_pdu > 3 ? max_pdu - 3 : 20;
        const std::string json = build_json(rpc_id);
        frames = BleProtocol::EncodeControlFrames(json, max_write, session->next_control_msg_id);
        if (frames.empty()) {
            LogBleLine("RPC encode failed VS-" + device_id + " len=" + std::to_string(json.size()));
            if (completion) completion(std::nullopt);
            return;
        }
        if (completion) {
            session->pending_rpcs[rpc_id] = std::move(completion);
        }
    }
    WriteControlFramesAsync(session, std::move(frames));
    if (timeout.count() > 0) {
        RpcTimeoutAsync(session, rpc_id, timeout);
    }
}

winrt::fire_and_forget BleCentralWin::WriteControlFramesAsync(
    std::shared_ptr<DeviceSession> session,
    std::vector<ByteVector> frames) {
    try {
        if (!session || !session->ready || !session->control_rx_characteristic) co_return;
        const auto write_option = HasWrite(session->control_rx_characteristic)
            ? GattWriteOption::WriteWithResponse
            : GattWriteOption::WriteWithoutResponse;
        for (const auto& frame : frames) {
            if (!session->ready) co_return;
            co_await session->control_rx_characteristic.WriteValueAsync(
                BufferFromBytes(frame), write_option);
        }
    } catch (...) {
    }
}

winrt::fire_and_forget BleCentralWin::RpcTimeoutAsync(
    std::shared_ptr<DeviceSession> session,
    std::uint32_t rpc_id,
    std::chrono::milliseconds timeout) {
    co_await WaitMs(timeout);
    if (shutting_down_) co_return;
    RpcCallback cb;
    {
        std::lock_guard lock(mutex_);
        auto it = session->pending_rpcs.find(rpc_id);
        if (it == session->pending_rpcs.end()) co_return;
        cb = std::move(it->second);
        session->pending_rpcs.erase(it);
    }
    DispatchToUiThread([cb = std::move(cb)]() {
        if (cb) cb(std::nullopt);
    });
}

void BleCentralWin::HandleControlMessage(const std::string& device_id,
                                         std::shared_ptr<DeviceSession> session,
                                         ControlMessage message) {
    if (message.kind == ControlMessage::Kind::kRpcResponse) {
        RpcCallback cb;
        {
            std::lock_guard lock(mutex_);
            auto it = session->pending_rpcs.find(message.rpc_response.id);
            if (it != session->pending_rpcs.end()) {
                cb = std::move(it->second);
                session->pending_rpcs.erase(it);
            }
        }
        if (cb) cb(std::move(message.rpc_response));
        return;
    }

    const auto& ev = message.control_event;
    if (ev.event == "battery.changed" && ev.battery) {
        if (on_battery_changed) on_battery_changed(device_id, *ev.battery);
    } else if (ev.event == "stream.closed" && ev.stream_id) {
        {
            std::lock_guard lock(mutex_);
            if (session->audio_in_stream_id == *ev.stream_id) session->audio_in_stream_id = 0;
            if (session->imu_stream_id == *ev.stream_id) session->imu_stream_id = 0;
            if (session->input_stream_id == *ev.stream_id) session->input_stream_id = 0;
        }
        if (on_stream_closed) on_stream_closed(device_id, *ev.stream_id, ev.reason);
    }
}

void BleCentralWin::HandleInputFrame(const std::string& device_id,
                                     std::shared_ptr<DeviceSession> session,
                                     const InputStreamFrame& frame) {
    bool has_gap = false;
    std::uint32_t prev_mask = 0;
    std::vector<std::string> buttons;
    {
        std::lock_guard lock(mutex_);
        if (session->input_stream_id == 0 || session->input_stream_id != frame.stream_id) return;
        if (session->last_input_seq.has_value() &&
            static_cast<std::uint16_t>(*session->last_input_seq + 1) != frame.seq) {
            has_gap = true;
        }
        session->last_input_seq = frame.seq;
        prev_mask = session->buttons_mask;
        session->buttons_mask = frame.buttons_mask;
        if (session->info) buttons = session->info->buttons;
    }
    if (has_gap && on_input_gap) {
        on_input_gap(device_id);
    }
    const std::uint32_t diff = prev_mask ^ frame.buttons_mask;
    if (diff == 0 || !on_button_event) return;
    for (std::size_t bit = 0; bit < buttons.size() && bit < 32; ++bit) {
        const std::uint32_t mask = 1u << bit;
        if ((diff & mask) != 0) {
            const bool is_down = (frame.buttons_mask & mask) != 0;
            on_button_event(device_id, buttons[bit], is_down, frame.timestamp_ms);
        }
    }
}

winrt::fire_and_forget BleCentralWin::UpdateFirmwareAsync(
    std::shared_ptr<DeviceSession> session,
    std::shared_ptr<FirmwareUpdateSession> update_session) {
    const auto self = shared_from_this();
    std::size_t offset = 0;
    try {
        if (!session || !update_session || !session->ota_rx_characteristic) {
            FinishFirmwareUpdate(update_session, false, "The connected firmware does not expose BLE OTA.");
            co_return;
        }
        if (update_session->finished || shutting_down_) co_return;

        // ATT Write uses three bytes and the VoiceStick OTA data frame uses twelve.
        const std::size_t max_pdu = session->gatt_session ? session->gatt_session.MaxPduSize() : 23;
        constexpr std::size_t packet_overhead = 15;
        if (max_pdu <= packet_overhead) {
            FinishFirmwareUpdate(update_session, false, "The Bluetooth link cannot carry a firmware packet.");
            co_return;
        }
        const auto chunk_size = std::min<std::size_t>(max_pdu - packet_overhead, 244);

        const auto ota_rx = session->ota_rx_characteristic;
        const auto ble_device = session->ble_device;
        const auto write_payload = [&ota_rx](const ByteVector& payload,
                                            GattWriteOption option = GattWriteOption::WriteWithResponse) {
            return ota_rx.WriteValueAsync(BufferFromBytes(payload), option);
        };

        co_await winrt::resume_background();
        if (update_session->finished || shutting_down_) co_return;
        OtaConnectionPreference preference;
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (ble_device && ApiInformation::IsMethodPresent(
                L"Windows.Devices.Bluetooth.BluetoothLEDevice", L"RequestPreferredConnectionParameters")) {
            try {
                const auto previous = ble_device.GetConnectionParameters();
                preference.request = ble_device.RequestPreferredConnectionParameters(
                    BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
                co_await winrt::resume_after(std::chrono::milliseconds(250));
                if (update_session->finished || shutting_down_) co_return;
                const auto current = ble_device.GetConnectionParameters();
                LogBleLine("OTA connection VS-" + update_session->device_id +
                           " throughput_status=" + std::to_string(static_cast<int>(preference.request.Status())) +
                           " previous_interval_us=" + std::to_string(previous.ConnectionInterval() * 1250) +
                           " interval_us=" + std::to_string(current.ConnectionInterval() * 1250) +
                           " timeout_ms=" + std::to_string(current.LinkTimeout() * 10));
            } catch (const winrt::hresult_error& error) {
                LogBleLine("OTA throughput request unavailable VS-" + update_session->device_id +
                           " hr=" + FormatHresult(error.code()));
            }
        }

        LogBleLine("OTA begin VS-" + update_session->device_id +
                   " transfer=" + std::to_string(update_session->transfer_id) +
                   " size=" + std::to_string(update_session->image.size()));
        auto begin = BleProtocol::OtaBeginPayload(
            static_cast<std::uint32_t>(update_session->image.size()),
            update_session->transfer_id);
        auto status = co_await write_payload(begin);
        if (update_session->finished || shutting_down_) co_return;
        if (status != GattCommunicationStatus::Success) {
            FinishFirmwareUpdate(update_session, false, "BLE OTA begin failed: " + GattStatusName(status));
            co_return;
        }

        // Limit outstanding commands by ending each batch with a write request.
        // The firmware rejects that request unless it has written every earlier offset.
        constexpr int kPacketsPerBatch = 3;
        const int packets_per_ack = HasWriteWithoutResponse(ota_rx) ? kPacketsPerBatch : 1;
        LogBleLine("OTA data VS-" + update_session->device_id +
                   " chunk_size=" + std::to_string(chunk_size) +
                   " max_pdu=" + std::to_string(max_pdu) +
                   " packets_per_ack=" + std::to_string(packets_per_ack));
        std::size_t last_logged_offset = 0;
        int packets_since_ack = 0;
        const auto data_started = std::chrono::steady_clock::now();
        auto next_progress = data_started;
        while (offset < update_session->image.size()) {
            if (update_session->finished || shutting_down_) co_return;
            const auto end = std::min(offset + chunk_size, update_session->image.size());
            const bool acknowledge = ++packets_since_ack == packets_per_ack || end == update_session->image.size();
            auto payload = BleProtocol::OtaDataPayload(
                update_session->transfer_id,
                static_cast<std::uint32_t>(offset),
                std::span<const std::uint8_t>(update_session->image.data() + offset, end - offset));
            status = co_await write_payload(payload, acknowledge
                ? GattWriteOption::WriteWithResponse : GattWriteOption::WriteWithoutResponse);
            if (update_session->finished || shutting_down_) co_return;
            if (status != GattCommunicationStatus::Success) {
                LogBleLine("OTA write failed VS-" + update_session->device_id +
                           " offset=" + std::to_string(offset) +
                           " status=" + GattStatusName(status));
                FinishFirmwareUpdate(update_session, false, "BLE OTA write failed at offset " +
                                     std::to_string(offset) + ": " + GattStatusName(status));
                co_return;
            }
            offset = end;
            if (!acknowledge) continue;
            packets_since_ack = 0;
            update_session->confirmed_bytes = offset;
            const auto now = std::chrono::steady_clock::now();
            if (offset - last_logged_offset >= 32 * 1024 || offset == update_session->image.size()) {
                last_logged_offset = offset;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - data_started).count();
                LogBleLine("OTA write confirmed VS-" + update_session->device_id +
                           " written=" + std::to_string(offset) +
                           "/" + std::to_string(update_session->image.size()) +
                           " elapsed_ms=" + std::to_string(elapsed));
            }
            if (update_session->progress && (now >= next_progress || offset == update_session->image.size())) {
                next_progress = now + std::chrono::milliseconds(500);
                update_session->progress(FirmwareUpdateProgress{
                    static_cast<int>(offset),
                    static_cast<int>(update_session->image.size()),
                    true});
            }
        }

        if (update_session->finished || shutting_down_) co_return;
        LogBleLine("OTA end VS-" + update_session->device_id +
                   " transfer=" + std::to_string(update_session->transfer_id) +
                   " size=" + std::to_string(update_session->image.size()));
        auto end = BleProtocol::OtaEndPayload(
            update_session->transfer_id,
            static_cast<std::uint32_t>(update_session->image.size()));
        status = co_await write_payload(end);
        if (update_session->finished || shutting_down_) co_return;
        if (status != GattCommunicationStatus::Success) {
            LogBleLine("OTA end failed VS-" + update_session->device_id +
                       " status=" + GattStatusName(status));
            FinishFirmwareUpdate(update_session, false, "BLE OTA end failed: " + GattStatusName(status));
            co_return;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!update_session->finished && !shutting_down_ &&
               std::chrono::steady_clock::now() < deadline) {
            co_await winrt::resume_after(std::chrono::milliseconds(100));
        }
        if (!update_session->finished && !shutting_down_) {
            FinishFirmwareUpdate(update_session, false, "The device did not confirm that the firmware update completed.");
        }
    } catch (const winrt::hresult_error& error) {
        if (!update_session || update_session->finished || shutting_down_) co_return;
        LogBleLine("OTA Bluetooth exception VS-" + update_session->device_id +
                   " offset=" + std::to_string(offset) +
                   " confirmed=" + std::to_string(update_session->confirmed_bytes.load()) +
                   " hr=" + FormatHresult(error.code()));
        if (error.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            FinishFirmwareUpdate(update_session, false, "Windows interrupted the Bluetooth firmware transfer.");
            co_return;
        }
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
    if (!update_session || update_session->finished || update_session->device_id != device_id) return;
    if (event.transfer_id.has_value() && *event.transfer_id != update_session->transfer_id) return;

    if (event.event == "progress") {
        if (event.written.has_value() && event.size.has_value()) {
            LogBleLine("OTA device progress VS-" + device_id +
                       " written=" + std::to_string(*event.written) +
                       "/" + std::to_string(*event.size));
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
        update_session->finished = true;
        firmware_update_session_.reset();
    }
    DispatchToUiThread([this, completion = std::move(update_session->completion), success, message] {
        StartScan();
        if (completion) completion(success, message);
    });
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
        LogBleLine("OTA link lost VS-" + device_id +
                   " confirmed=" + std::to_string(update_session->confirmed_bytes.load()) +
                   "/" + std::to_string(update_session->image.size()));
        FinishFirmwareUpdate(update_session, false, "Device disconnected during firmware update.");
    }
    if (removed) CloseSession(std::move(removed));
    LogBleLine("device disconnected VS-" + device_id + "; restarting scan for reconnection");
    DispatchToUiThread([this, restart_scan = !update_session] {
        PublishConnections();
        if (restart_scan) StartScan();
    });
}

void BleCentralWin::CloseSession(std::shared_ptr<DeviceSession> session) {
    if (!session) return;
    if (session->stream_tx_characteristic && session->stream_tx_value_changed_token.value != 0) {
        try { session->stream_tx_characteristic.ValueChanged(session->stream_tx_value_changed_token); } catch (...) {}
    }
    if (session->control_tx_characteristic && session->control_tx_value_changed_token.value != 0) {
        try { session->control_tx_characteristic.ValueChanged(session->control_tx_value_changed_token); } catch (...) {}
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
    session->stream_tx_characteristic = nullptr;
    session->control_tx_characteristic = nullptr;
    session->control_rx_characteristic = nullptr;
    session->ota_rx_characteristic = nullptr;
    session->ota_state_characteristic = nullptr;
    session->ready = false;
    session->initialized = false;
    session->pending_rpcs.clear();
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
