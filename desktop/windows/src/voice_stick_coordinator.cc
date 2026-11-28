#include "voice_stick_coordinator.h"

#include "log.h"
#include "version.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <format>
#include <chrono>
#include <utility>

namespace voicestick {

namespace {

void LogCoordinatorLine(const std::string& message) {
    LogCoordinator(message);
}

bool IsFirmwareManifestCompatible(const DeviceFirmwareInfo& info, const FirmwareManifest& manifest) {
    return IsFirmwareHardwareCompatible(info.hardware, info.current_version, manifest.hardware);
}

std::string NewConfirmationId() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    return std::format("{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-"
                       "{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                       bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                       bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                       bytes[12], bytes[13], bytes[14], bytes[15]);
}

std::string AsrStartFailureMessage(const AsrClient& asr) {
    const auto message = asr.LastStartError();
    return message.empty() ? "Failed to start ASR" : message;
}

} // namespace

VoiceStickCoordinator::VoiceStickCoordinator(AppConfig config,
                                             std::shared_ptr<BleCentral> ble,
                                             std::unique_ptr<AsrClient> asr,
                                             VoiceStickUi* ui,
                                             InputInjector* input_injector,
                                             std::function<std::unique_ptr<AsrClient>(const AppConfig&)> asr_factory,
                                             std::function<void(std::function<void()>)> post_to_ui)
    : config_(std::move(config)),
      ble_(std::move(ble)),
      asr_(std::move(asr)),
      asr_factory_(std::move(asr_factory)),
      translator_(config_),
      post_to_ui_(std::move(post_to_ui)),
      ui_(ui),
      input_injector_(input_injector),
      debug_audio_recorder_(config_.debug_audio_cache, config_.debug_audio_directory),
      paired_device_ids_(config_.paired_device_ids) {
    for (const auto& entry : config_.paired_devices) {
        if (entry.device_id.empty()) continue;
        auto& info = firmware_info_by_device_id_[entry.device_id];
        info.hardware = entry.hardware;
        info.current_version = entry.firmware_version;
    }
}

VoiceStickCoordinator::~VoiceStickCoordinator() {
    alive_->store(false);
    Shutdown();
    for (auto& [_, feed] : firmware_feeds_) {
        if (feed.worker.joinable()) feed.worker.join();
    }
    if (firmware_download_thread_.joinable()) {
        firmware_download_thread_.join();
    }
}

void VoiceStickCoordinator::Start() {
    ble_->on_connection_change = [this](std::vector<ConnectedDevice> devices) {
        if (is_shutdown_) return;
        const auto is_disconnected = [&devices](const std::string& id) {
            return std::none_of(devices.begin(), devices.end(), [&id](const ConnectedDevice& device) {
                return device.id == id;
            });
        };
        {
            std::lock_guard lock(firmware_mutex_);
            for (auto& [device_id, info] : firmware_info_by_device_id_) {
                if (is_disconnected(device_id)) {
                    info.reported_on_connection = false;
                    info.buttons.reset();
                }
            }
        }
        std::erase_if(secondary_down_ms_, [&](const auto& entry) { return is_disconnected(entry.first); });
        std::erase_if(active_capture_utterance_, [&](const auto& entry) { return is_disconnected(entry.first); });
        std::erase_if(device_imu_caps_, [&](const auto& entry) { return is_disconnected(entry.first); });
        if (shake_device_id_ && is_disconnected(*shake_device_id_)) {
            StopShakeConfirmation();
        }
        ui_->SetConnectedDevices(devices);
        CancelActiveCycleIfDeviceDisconnected();
        RefreshFirmwareAvailability();
        if (session_state_ == SessionState::kPausedConfirmation && active_device_id_) {
            if (ble_->IsConnected(*active_device_id_)) RefreshDeviceUiState(*active_device_id_);
        } else {
            ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
        }
    };
    ble_->on_connection_error = [this](std::string device_id, std::string message) {
        if (is_shutdown_) return;
        ui_->SetPairingError(device_id, message);
    };
    ble_->on_scan_error = [this](std::string message) {
        if (is_shutdown_) return;
        LogCoordinatorLine("BLE scan error: " + message);
        ui_->SetStatus("Turn on Bluetooth");
    };
    ble_->on_device_info = [this](std::string device_id, DeviceInfoResult info) {
        if (is_shutdown_) return;
        HandleDeviceInfo(info, device_id);
    };
    ble_->on_device_ready = [this](std::string device_id, DeviceInfoResult info) {
        if (is_shutdown_) return;
        HandleDeviceReady(info, device_id);
    };
    ble_->on_legacy_device_info = [this](std::string device_id, LegacyDeviceInfo info) {
        if (is_shutdown_) return;
        HandleLegacyDeviceInfo(info, device_id);
    };
    ble_->on_battery_changed = [this](std::string device_id, DeviceBatteryStatus battery) {
        if (is_shutdown_) return;
        ui_->SetDeviceBatteryStatus(device_id, battery);
    };
    ble_->on_button_event = [this](std::string device_id, std::string button, bool is_down, std::uint32_t timestamp_ms) {
        if (is_shutdown_) return;
        HandleButtonEvent(device_id, button, is_down, timestamp_ms);
    };
    ble_->on_input_gap = [this](std::string device_id) {
        if (is_shutdown_) return;
        secondary_down_ms_.erase(device_id);
    };
    ble_->on_audio_frame = [this](std::string device_id, AudioStreamFrame frame) {
        if (is_shutdown_) return;
        HandleAudioFrame(frame, device_id);
    };
    ble_->on_imu_frame = [this](std::string device_id, ImuStreamFrame frame) {
        if (is_shutdown_) return;
        HandleImuFrame(frame, device_id);
    };
    ble_->on_stream_closed = [this](std::string device_id, std::uint16_t stream_id, std::string reason) {
        if (is_shutdown_) return;
        HandleStreamClosed(device_id, stream_id, reason);
    };
    ConfigureAsrCallbacks();
    ble_->Start();
    CheckFirmwareUpdates();
}

void VoiceStickCoordinator::Shutdown() {
    if (is_shutdown_) return;
    is_shutdown_ = true;
    StopShakeConfirmation();
    ble_->on_connection_change = nullptr;
    ble_->on_connection_error = nullptr;
    ble_->on_scan_error = nullptr;
    ble_->on_device_info = nullptr;
    ble_->on_device_ready = nullptr;
    ble_->on_legacy_device_info = nullptr;
    ble_->on_battery_changed = nullptr;
    ble_->on_button_event = nullptr;
    ble_->on_input_gap = nullptr;
    ble_->on_audio_frame = nullptr;
    ble_->on_imu_frame = nullptr;
    ble_->on_stream_closed = nullptr;
    asr_->Cancel();
    for (auto& [_, cycle] : subtitle_cycles_) {
        if (cycle->asr) cycle->asr->Cancel();
        cycle->debug_audio_recorder.Discard();
    }
    subtitle_cycles_.clear();
    active_subtitle_sessions_.clear();
    active_capture_utterance_.clear();
    ui_->HideSubtitles();
    CancelAudioEndTimeout();
    active_session_id_.reset();
    active_device_id_.reset();
    active_session_started_at_ = {};
    pending_paste_state_ = {};
    debug_audio_recorder_.Discard();
    ble_->Shutdown();
}

void VoiceStickCoordinator::UpdateConfig(AppConfig config) {
    const bool was_recognizing = asr_started_ || active_session_id_.has_value() ||
                                 !pending_paste_state_.IsIdle() || !subtitle_cycles_.empty();
    if (was_recognizing) {
        StopShakeConfirmation();
        asr_->Cancel();
        for (auto& [_, cycle] : subtitle_cycles_) {
            if (cycle->asr) cycle->asr->Cancel();
            cycle->debug_audio_recorder.Discard();
        }
        subtitle_cycles_.clear();
        active_subtitle_sessions_.clear();
        active_capture_utterance_.clear();
        ui_->HideSubtitles();
        active_session_id_.reset();
        pending_paste_state_ = {};
        debug_audio_recorder_.Discard();
        FinishRecognitionCycle();
        EnterReady("config_update_cancel");
    }

    const bool interaction_changed = config_.interaction_mode != config.interaction_mode;
    config_ = std::move(config);
    translator_ = LLMTranslationClient(config_);
    if (interaction_changed || was_recognizing) {
        for (const auto& device_id : ble_->ConnectedReadyDeviceIds()) {
            RestoreVoiceMode(device_id);
        }
    }
    debug_audio_recorder_ = DebugAudioRecorder(config_.debug_audio_cache, config_.debug_audio_directory);
    if (asr_factory_) {
        asr_ = asr_factory_(config_);
        ConfigureAsrCallbacks();
    }
    if (paired_device_ids_ != config_.paired_device_ids) {
        paired_device_ids_ = config_.paired_device_ids;
        ui_->SetPairedDeviceIds(paired_device_ids_);
        ble_->UpdatePairedDeviceIds(paired_device_ids_);
        CheckFirmwareUpdates();
    }
}

void VoiceStickCoordinator::ReconnectPairedDevices() {
    ble_->UpdatePairedDeviceIds(paired_device_ids_);
}

void VoiceStickCoordinator::ConnectPairedDevice(const std::string& device_id,
                                                std::uint64_t bluetooth_address,
                                                BluetoothAddressKind address_kind,
                                                const std::string& name) {
    ble_->ConnectPairedDevice(device_id, bluetooth_address, address_kind, name);
}

void VoiceStickCoordinator::PairDevice(const std::string& device_id,
                                       std::uint64_t bluetooth_address,
                                       BluetoothAddressKind address_kind,
                                       const std::string& name) {
    ble_->PairDevice(device_id, bluetooth_address, address_kind, name);
}

void VoiceStickCoordinator::CancelPendingConnect(const std::string& device_id) {
    ble_->CancelPendingConnect(device_id);
}

void VoiceStickCoordinator::ConfirmPairedDeviceIds(const std::vector<std::string>& device_ids,
                                                   const std::string& device_id) {
    paired_device_ids_ = device_ids;
    config_.paired_device_ids = device_ids;
    ui_->SetPairedDeviceIds(paired_device_ids_);
    ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
    std::string hardware;
    {
        std::lock_guard lock(firmware_mutex_);
        if (const auto info = firmware_info_by_device_id_.find(device_id); info != firmware_info_by_device_id_.end()) {
            hardware = info->second.hardware;
        }
    }
    if (hardware.empty()) {
        CheckFirmwareUpdates();
    } else {
        CheckFirmwareFeed(hardware, true);
        RefreshFirmwareAvailability();
    }
}

void VoiceStickCoordinator::RemovePairedDevice(const std::string& device_id) {
    auto it = std::find(paired_device_ids_.begin(), paired_device_ids_.end(), device_id);
    if (it == paired_device_ids_.end()) return;
    paired_device_ids_.erase(it);
    config_.paired_device_ids = paired_device_ids_;
    config_.device_output_profiles.erase(device_id);
    if (std::ranges::any_of(subtitle_cycles_, [&device_id](const auto& entry) {
            return entry.first.first == device_id;
        })) {
        CancelSubtitleCyclesForDevice(device_id, "forget_device");
    }
    if (last_recoverable_device_id_ == device_id) last_recoverable_device_id_.reset();
    {
        std::lock_guard lock(firmware_mutex_);
        firmware_info_by_device_id_.erase(device_id);
    }
    if (active_device_id_.has_value() && *active_device_id_ == device_id) {
        // The active recording cycle was tied to the device we just forgot;
        // reset transient session state so a stale frame can't run the rest
        // of the pipeline against a torn-down session.
        asr_->Cancel();
        debug_audio_recorder_.Discard();
        active_session_id_.reset();
        pending_paste_state_ = {};
        FinishRecognitionCycle();
        EnterReady("forget_active_device");
    }
    LogCoordinatorLine("forget paired device VS-" + device_id);
    ui_->SetPairedDeviceIds(paired_device_ids_);
    ble_->UpdatePairedDeviceIds(paired_device_ids_);
    RefreshFirmwareAvailability();
    ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
}

bool VoiceStickCoordinator::RestoreLastInputConfirmation() {
    return RestoreLastInputConfirmation(last_recoverable_device_id_);
}

bool VoiceStickCoordinator::IsIdleForFirmwareReminder() const {
    return !is_shutdown_ && session_state_ == SessionState::kReady && !active_device_id_ &&
           pending_paste_state_.IsIdle() && subtitle_cycles_.empty();
}

void VoiceStickCoordinator::UpdateFirmwareFromLatest(
    const std::string& device_id,
    std::function<void(FirmwareUpdateProgress)> progress,
    std::function<void(bool, std::string)> completion) {
    std::optional<FirmwareManifest> manifest;
    DeviceFirmwareInfo device_info;
    std::string validation_error;
    {
        std::lock_guard lock(firmware_mutex_);
        const auto info = firmware_info_by_device_id_.find(device_id);
        if (info != firmware_info_by_device_id_.end()) {
            device_info = info->second;
            const auto url = FirmwareManifestClient::ManifestUrlForHardware(device_info.hardware);
            if (const auto feed = firmware_feeds_.find(url); feed != firmware_feeds_.end() &&
                !feed->second.is_checking && feed->second.error_message.empty()) {
                manifest = feed->second.manifest;
            }
        }
        if (!device_info.reported_on_connection) {
            validation_error = "Connect the device and wait for it to report its firmware version.";
        } else if (!manifest) {
            validation_error = "Firmware update manifest is not loaded yet.";
        } else if (!IsFirmwareManifestCompatible(device_info, *manifest) ||
                   !FirmwareVersion::IsOlderThan(device_info.current_version, manifest->version)) {
            validation_error = "No newer firmware is available for this device.";
        } else if (device_info.requires_input_firmware && FirmwareVersion::IsOlderThan(
                       manifest->version, AppConfig::minimum_compatible_firmware_version)) {
            validation_error = "A firmware update that supports this version of VoiceStick is not available yet.";
        } else if (manifest->CheckWindowsAppVersion(VOICESTICK_VERSION_STR) ==
                   FirmwareAppCompatibility::kAppUpdateRequired) {
            validation_error = "Update VoiceStick to " + manifest->minimum_windows_app_version +
                               " or later before updating firmware.";
        }
    }
    if (!validation_error.empty()) {
        completion(false, std::move(validation_error));
        return;
    }
    if (!ble_->IsConnected(device_id)) {
        completion(false, "Connect the device before updating firmware.");
        return;
    }
    bool already_downloading = false;
    {
        std::lock_guard lock(firmware_transfer_mutex_);
        already_downloading = firmware_download_in_flight_;
        if (!already_downloading) {
            firmware_download_in_flight_ = true;
            firmware_download_cancelled_ = false;
        }
    }
    if (already_downloading) {
        completion(false, "A firmware download is already running.");
        return;
    }
    if (firmware_download_thread_.joinable()) firmware_download_thread_.join();

    firmware_download_thread_ = std::thread([
        this, alive = alive_, device_id, device_info = std::move(device_info), manifest = std::move(*manifest),
        progress = std::move(progress), completion = std::move(completion)]() mutable {
        std::string error;
        FirmwareManifestClient client(FirmwareManifestClient::ManifestUrlForHardware(manifest.hardware));
        auto image = client.DownloadOtaSync(manifest, error);
        bool same_device = false;
        {
            std::lock_guard lock(firmware_mutex_);
            const auto current = firmware_info_by_device_id_.find(device_id);
            same_device = current != firmware_info_by_device_id_.end() && current->second.reported_on_connection &&
                          current->second.device_uuid == device_info.device_uuid &&
                          current->second.current_version == device_info.current_version &&
                          IsFirmwareManifestCompatible(current->second, manifest);
        }
        {
            std::lock_guard lock(firmware_transfer_mutex_);
            firmware_download_in_flight_ = false;
            if (!alive->load()) return;
            if (firmware_download_cancelled_) {
                error = "Firmware update cancelled.";
            } else if (!image) {
                if (error.empty()) error = "Failed to download firmware.";
            } else if (!same_device || !ble_->IsConnected(device_id)) {
                error = "The device changed or disconnected while the firmware was downloading.";
            } else {
                ble_->UpdateFirmware(std::move(*image), device_id, std::move(progress), std::move(completion));
                return;
            }
        }
        completion(false, std::move(error));
    });
}

void VoiceStickCoordinator::CancelFirmwareUpdate() {
    {
        std::lock_guard lock(firmware_transfer_mutex_);
        if (firmware_download_in_flight_) firmware_download_cancelled_ = true;
    }
    ble_->CancelFirmwareUpdate();
}

void VoiceStickCoordinator::ConfigureAsrCallbacks() {
    asr_->on_partial = [this](std::string text) {
        if (session_state_ != SessionState::kRecording && !IsWaitingForFinalText()) return;
        ui_->ShowPartial(text, active_device_id_);
        if (ShouldSendPartialToDevice()) {
            SendUiStateForActiveDevice("thinking", text);
        }
    };
    asr_->on_segment = [this](AsrSegment segment) {
        HandleDefiniteSegment(segment);
    };
    asr_->on_final = [this](AsrFinalResult result) {
        FinishWithFinalText(result);
    };
    asr_->on_error = [this](std::string message, std::string partial_text) {
        std::lock_guard lock(audio_mutex_);
        if (is_shutdown_ || !asr_started_ || pasted_final_text_) return;
        FinishWithAsrError(message, partial_text);
    };
    asr_->on_upgrade_url = [this](std::string url, std::string message) {
        if (!is_showing_asr_error_) return;
        const auto device_id = active_device_id_;
        RecoverFromAsrError(false);
        ui_->ShowCloudUpgrade(message, url, device_id);
    };
}

void VoiceStickCoordinator::ConfigureSubtitleAsrCallbacks(SubtitleCycle* cycle) {
    if (!cycle || !cycle->asr) return;
    const auto device_id = cycle->device_id;
    const auto session_id = cycle->session_id;
    cycle->asr->on_partial = [this, device_id, session_id](std::string text) {
        auto* cycle = FindSubtitleCycle(device_id, session_id);
        if (!cycle || !CanUpdateOverlayForSubtitleCycle(device_id, session_id)) return;
        ui_->ShowPartial(text, device_id);
        if (ShouldSendSubtitlePartialToDevice(cycle)) {
            SendDisplayState("thinking", text, device_id);
        }
    };
    cycle->asr->on_segment = [this, device_id, session_id](AsrSegment segment) {
        if (!IsActiveSubtitleCycle(device_id, session_id)) return;
        HandleSubtitleDefiniteSegment(segment, device_id);
    };
    cycle->asr->on_final = [this, device_id, session_id](AsrFinalResult result) {
        FinishSubtitleCycleWithFinalText(device_id, session_id, result);
    };
    cycle->asr->on_error = [this, device_id, session_id](std::string message, std::string) {
        FinishSubtitleCycleWithError(device_id, session_id, message);
    };
    cycle->asr->on_upgrade_url = [this, device_id](std::string url, std::string message) {
        SendDisplayState("ready", "", device_id);
        ui_->ShowCloudUpgrade(message, url, device_id);
    };
}

void VoiceStickCoordinator::HandleDeviceInfo(const DeviceInfoResult& info, const std::string& device_id) {
    if (info.imu) {
        device_imu_caps_[device_id] = *info.imu;
    } else {
        device_imu_caps_.erase(device_id);
    }
    if (info.battery) {
        ui_->SetDeviceBatteryStatus(device_id, *info.battery);
    }
    DeviceInfo device;
    {
        std::lock_guard lock(firmware_mutex_);
        auto& entry = firmware_info_by_device_id_[device_id];
        if (!info.hardware.empty()) entry.hardware = info.hardware;
        if (!info.firmware_version.empty()) entry.current_version = info.firmware_version;
        if (!info.device_uuid.empty()) entry.device_uuid = info.device_uuid;
        entry.reported_on_connection = true;
        entry.requires_input_firmware = !info.SupportsProtocolV2();
        if (entry.requires_input_firmware) {
            entry.buttons.reset();
        } else {
            entry.buttons = info.buttons;
        }
        device = DeviceInfo{device_id, entry.hardware, entry.current_version, entry.buttons};
    }
    ui_->SetDeviceInfo(device);
    if (!info.hardware.empty()) {
        CheckFirmwareFeed(info.hardware, false);
        RefreshFirmwareAvailability();
    }
}

void VoiceStickCoordinator::HandleLegacyDeviceInfo(const LegacyDeviceInfo& info, const std::string& device_id) {
    device_imu_caps_.erase(device_id);
    DeviceInfo device;
    {
        std::lock_guard lock(firmware_mutex_);
        auto& entry = firmware_info_by_device_id_[device_id];
        if (!info.hardware.empty()) entry.hardware = info.hardware;
        if (!info.firmware_version.empty()) entry.current_version = info.firmware_version;
        if (!info.device_uuid.empty()) entry.device_uuid = info.device_uuid;
        entry.reported_on_connection = true;
        entry.requires_input_firmware = true;
        entry.buttons.reset();
        device = DeviceInfo{device_id, entry.hardware, entry.current_version, std::nullopt};
    }
    ui_->SetDeviceInfo(device);
    if (!info.hardware.empty()) {
        CheckFirmwareFeed(info.hardware, false);
        RefreshFirmwareAvailability();
    }
}

void VoiceStickCoordinator::HandleDeviceReady(const DeviceInfoResult&, const std::string& device_id) {
    RestoreVoiceMode(device_id);
}

void VoiceStickCoordinator::RestoreVoiceMode(const std::string& device_id) {
    if ((session_state_ == SessionState::kPendingConfirmation ||
         session_state_ == SessionState::kPausedConfirmation) &&
        active_device_id_ == device_id) {
        ble_->StopAudioIn(device_id);
        RefreshDeviceUiState(device_id);
        if (session_state_ == SessionState::kPausedConfirmation) {
            StartShakeConfirmation(device_id);
        }
        return;
    }
    ble_->StartAudioIn(device_id, config_.interaction_mode);
    SendDisplayState("ready", "", device_id);
}

bool VoiceStickCoordinator::CanUseDeviceInput(const std::string& device_id) {
    std::lock_guard lock(firmware_mutex_);
    const auto info = firmware_info_by_device_id_.find(device_id);
    return info != firmware_info_by_device_id_.end() && info->second.reported_on_connection &&
           !info->second.requires_input_firmware;
}

void VoiceStickCoordinator::HandleButtonEvent(const std::string& device_id,
                                              const std::string& button,
                                              bool is_down,
                                              std::uint32_t timestamp_ms) {
    if (!CanUseDeviceInput(device_id)) return;
    if (button == "primary") {
        if (is_down) {
            if (!HandleFrontButtonDuringPendingPaste(device_id) &&
                session_state_ != SessionState::kReady &&
                !active_capture_utterance_.contains(device_id)) {
                RefreshDeviceUiState(device_id);
            }
        }
    } else if (button == "secondary") {
        if (is_down) {
            secondary_down_ms_[device_id] = timestamp_ms;
        } else if (const auto press = secondary_down_ms_.extract(device_id);
                   !press.empty() && timestamp_ms >= press.mapped() &&
                   timestamp_ms - press.mapped() < 3000) {
            HandleSecondaryButtonClick(device_id);
        }
    }
}

void VoiceStickCoordinator::HandleSecondaryButtonClick(const std::string& device_id) {
    if (HasActiveSubtitleSession(device_id)) {
        CancelSubtitleCycle(device_id, "secondary_cancel");
        return;
    }
    if (std::any_of(subtitle_cycles_.begin(), subtitle_cycles_.end(),
                    [&](const auto& entry) { return entry.first.first == device_id; })) {
        CancelSubtitleCyclesForDevice(device_id, "secondary_cancel");
        return;
    }
    CancelPendingPaste(device_id);
}

void VoiceStickCoordinator::HandlePrimaryButtonDown(std::optional<std::uint32_t> session_id,
                                                       const std::string& device_id) {
    if (config_.default_output_profile.target == OutputTarget::kSubtitle) {
        HandleSubtitlePrimaryButtonDown(session_id, device_id);
        return;
    }
    if (HandleFrontButtonDuringPendingPaste(device_id)) return;
    if (session_state_ != SessionState::kReady || active_device_id_.has_value()) {
        RefreshDeviceUiState(device_id);
        return;
    }
    if (!session_id.has_value() || *session_id == 0) {
        SendDisplayState("ready", "", device_id);
        return;
    }

    {
        std::lock_guard lock(audio_mutex_);
        active_session_id_ = session_id;
        active_device_id_ = device_id;
        active_session_started_at_ = std::chrono::steady_clock::now();
        received_audio_frames_ = 0;
        last_audio_seq_.reset();
        buffered_ogg_chunks_.clear();
        asr_started_ = false;
        sent_final_audio_chunk_ = false;
        pasted_final_text_ = false;
        pending_paste_state_ = {};
        is_showing_asr_error_ = false;
        ogg_muxer_.Reset();
        debug_audio_recorder_.Start(device_id, session_id);
        SetSessionState(SessionState::kRecording, "primary_down");
    }
    ui_->ShowListening(OutputProfileForDevice(active_device_id_), active_device_id_);
    SendUiStateForActiveDevice("recording");
}

void VoiceStickCoordinator::HandlePrimaryButtonUp(std::uint32_t session_id, const std::string& device_id) {
    if (const auto subtitle = active_subtitle_sessions_.find(device_id); subtitle != active_subtitle_sessions_.end()) {
        if (subtitle->second == session_id) HandleSubtitlePrimaryButtonUp(device_id);
        return;
    }
    std::lock_guard lock(audio_mutex_);
    if (active_session_id_ != session_id || active_device_id_ != device_id) return;
    const double duration = CurrentRecordingDurationSeconds();
    if (duration < kMinimumRecordingDurationSeconds) {
        CancelShortRecording();
    } else {
        BeginWaitingForAudioEnd("button_up");
    }
}

void VoiceStickCoordinator::HandleAudioFrame(const AudioStreamFrame& frame, const std::string& device_id) {
    if (!CanUseDeviceInput(device_id)) return;
    const auto t0 = std::chrono::steady_clock::now();

    if (frame.IsStart()) {
        std::uint32_t utterance_id = next_utterance_id_++;
        if (next_utterance_id_ == 0) next_utterance_id_ = 1;
        active_capture_utterance_[device_id] = utterance_id;
        HandlePrimaryButtonDown(utterance_id, device_id);
        if (frame.payload.empty() && !frame.IsEnd()) return;
    }

    const auto it = active_capture_utterance_.find(device_id);
    if (it == active_capture_utterance_.end()) return;
    const std::uint32_t utterance_id = it->second;
    if (frame.IsEnd()) {
        active_capture_utterance_.erase(it);
    }

    if (FindSubtitleCycle(device_id, utterance_id)) {
        HandleSubtitleAudioFrame(frame, utterance_id, device_id);
        return;
    }
    std::lock_guard lock(audio_mutex_);
    if (!active_session_id_.has_value() || utterance_id != *active_session_id_ || active_device_id_ != device_id) {
        return;
    }
    if (frame.IsEnd() && frame.payload.empty()) {
        CancelAudioEndTimeout();
        SendFinalOggChunkIfNeeded(CurrentRecordingDurationSeconds());
        return;
    }
    if (frame.payload.empty()) return;
    if (last_audio_seq_.has_value() && frame.seq != *last_audio_seq_ + 1) {
        LogCoordinatorLine("audio seq gap dev=VS-" + device_id +
                           " session=" + std::to_string(*active_session_id_) +
                           " expected=" + std::to_string(*last_audio_seq_ + 1) +
                           " got=" + std::to_string(frame.seq));
    }
    last_audio_seq_ = frame.seq;
    ++received_audio_frames_;
    auto ogg_chunk = ogg_muxer_.Append(frame.payload, frame.IsEnd());
    debug_audio_recorder_.Append(ogg_chunk);
    const double duration = CurrentRecordingDurationSeconds();
    SendOrBufferOggChunk(
        ogg_chunk,
        frame.IsEnd(),
        duration >= kMinimumRecordingDurationSeconds &&
            (!waiting_for_audio_end_.load() || frame.IsEnd()));
    if (frame.IsEnd()) {
        CancelAudioEndTimeout();
        sent_final_audio_chunk_ = true;
        if (!asr_started_ && duration < kMinimumRecordingDurationSeconds) {
            CancelShortRecording();
            return;
        }
        active_session_id_.reset();
        debug_audio_recorder_.Finish();
        EnterFinalizing("audio_end");
    }
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (elapsed_us > 1000) {
        LogCoordinatorLine("audio frame slow dev=VS-" + device_id +
                           " seq=" + std::to_string(frame.seq) +
                           " elapsed_us=" + std::to_string(elapsed_us));
    }
}

void VoiceStickCoordinator::HandleImuFrame(const ImuStreamFrame& frame, const std::string& device_id) {
    if (pending_paste_state_.kind != PendingPasteKind::kPaused || shake_device_id_ != device_id) return;
    const auto it = device_imu_caps_.find(device_id);
    if (it == device_imu_caps_.end() || it->second.accel_lsb_per_g == 0) return;
    if (shake_detector_.Sample(frame.ax, frame.ay, frame.az, it->second.accel_lsb_per_g,
                               static_cast<std::int64_t>(frame.timestamp_ms) * 1000LL)) {
        StopShakeConfirmation();
        DismissPendingConfirmation();
    }
}

void VoiceStickCoordinator::HandleStreamClosed(const std::string& device_id,
                                               std::uint16_t stream_id,
                                               const std::string& reason) {
    LogCoordinatorLine("stream closed dev=VS-" + device_id +
                       " stream_id=" + std::to_string(stream_id) +
                       " reason=" + reason);
    if (const auto it = active_capture_utterance_.find(device_id); it != active_capture_utterance_.end()) {
        const auto utterance_id = it->second;
        active_capture_utterance_.erase(it);
        HandlePrimaryButtonUp(utterance_id, device_id);
    }
    if (shake_device_id_ == device_id && reason != "ota") {
        StopShakeConfirmation();
    }
}

void VoiceStickCoordinator::StartShakeConfirmation(const std::string& device_id) {
    const auto it = device_imu_caps_.find(device_id);
    if (it == device_imu_caps_.end() || it->second.accel_lsb_per_g == 0) return;
    if (shake_device_id_ == device_id) return;
    StopShakeConfirmation();
    shake_device_id_ = device_id;
    shake_detector_.Reset();
    const auto generation = shake_renew_generation_.fetch_add(1) + 1;
    ble_->StartImu(device_id, "accel", 50, 3000);
    std::thread([this, alive = alive_, generation, device_id] {
        while (alive->load() && shake_renew_generation_.load() == generation) {
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            if (!alive->load() || shake_renew_generation_.load() != generation) return;
            auto renew = [this, alive, generation, device_id] {
                if (!alive->load() || shake_renew_generation_.load() != generation) return;
                if (shake_device_id_ == device_id) {
                    ble_->RenewImu(device_id, 3000);
                }
            };
            if (post_to_ui_) post_to_ui_(std::move(renew));
            else renew();
        }
    }).detach();
}

void VoiceStickCoordinator::StopShakeConfirmation() {
    shake_renew_generation_.fetch_add(1);
    if (shake_device_id_) {
        const auto device_id = *shake_device_id_;
        shake_device_id_.reset();
        shake_detector_.Reset();
        ble_->StopImu(device_id);
    }
}

void VoiceStickCoordinator::HandleSubtitlePrimaryButtonDown(std::optional<std::uint32_t> session_id,
                                                            const std::string& device_id) {
    if (!session_id.has_value() || *session_id == 0) {
        SendDisplayState("ready", "", device_id);
        return;
    }
    if (auto active = active_subtitle_sessions_.find(device_id);
        (active != active_subtitle_sessions_.end() && active->second == *session_id) ||
        subtitle_cycles_.contains({device_id, *session_id})) {
        return;
    }
    if (auto previous = active_subtitle_sessions_.find(device_id);
        previous != active_subtitle_sessions_.end()) {
        ClearActiveSubtitleSession(device_id, previous->second);
    }
    if (!asr_factory_) {
        ui_->ShowError("Subtitle ASR is not available", device_id, [this, device_id] {
            SendDisplayState("ready", "", device_id);
        });
        return;
    }
    auto cycle = std::make_unique<SubtitleCycle>();
    cycle->device_id = device_id;
    cycle->session_id = *session_id;
    cycle->started_at = std::chrono::steady_clock::now();
    cycle->asr = asr_factory_(config_);
    cycle->debug_audio_recorder = DebugAudioRecorder(config_.debug_audio_cache, config_.debug_audio_directory);
    ConfigureSubtitleAsrCallbacks(cycle.get());
    cycle->debug_audio_recorder.Start(device_id, session_id);
    subtitle_cycles_[{device_id, *session_id}] = std::move(cycle);
    active_subtitle_sessions_[device_id] = *session_id;
    ui_->ShowListening(OutputProfileForDevice(device_id), device_id);
    SendDisplayState("recording", "", device_id);
}

void VoiceStickCoordinator::HandleSubtitlePrimaryButtonUp(const std::string& device_id) {
    auto* cycle = FindActiveSubtitleCycle(device_id);
    if (!cycle) return;
    const double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cycle->started_at).count();
    if (duration < kMinimumRecordingDurationSeconds) {
        CancelSubtitleCycle(device_id, "short_recording");
    } else {
        BeginWaitingForSubtitleAudioEnd(cycle, "button_up");
        FinishSubtitleAudioInput(cycle);
    }
}

void VoiceStickCoordinator::HandleSubtitleAudioFrame(const AudioStreamFrame& frame,
                                                     std::uint32_t utterance_id,
                                                     const std::string& device_id) {
    auto* cycle = FindSubtitleCycle(device_id, utterance_id);
    if (!cycle) return;
    if (frame.IsEnd() && frame.payload.empty()) {
        CancelSubtitleAudioEndTimeout(cycle);
        SendSubtitleFinalOggChunkIfNeeded(device_id, utterance_id);
        return;
    }
    if (frame.payload.empty()) return;
    if (cycle->last_audio_seq.has_value() && frame.seq != *cycle->last_audio_seq + 1) {
        LogCoordinatorLine("subtitle audio seq gap dev=VS-" + device_id +
                           " session=" + std::to_string(cycle->session_id) +
                           " expected=" + std::to_string(*cycle->last_audio_seq + 1) +
                           " got=" + std::to_string(frame.seq));
    }
    cycle->last_audio_seq = frame.seq;
    ++cycle->received_audio_frames;
    const double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cycle->started_at).count();
    auto ogg_chunk = cycle->ogg_muxer.Append(frame.payload, frame.IsEnd());
    cycle->debug_audio_recorder.Append(ogg_chunk);
    SendOrBufferSubtitleOggChunk(cycle, ogg_chunk, frame.IsEnd(),
                                 duration >= kMinimumRecordingDurationSeconds &&
                                     (!cycle->waiting_for_audio_end || frame.IsEnd()));
    if (frame.IsEnd()) {
        cycle = FindSubtitleCycle(device_id, utterance_id);
        if (!cycle) return;
        CancelSubtitleAudioEndTimeout(cycle);
        cycle->sent_final_audio_chunk = true;
        if (!cycle->asr_started && duration < kMinimumRecordingDurationSeconds) {
            CancelSubtitleCycle(device_id, "short_recording");
            return;
        }
        cycle->debug_audio_recorder.Finish();
        FinishSubtitleAudioInput(cycle);
    }
}

void VoiceStickCoordinator::BeginWaitingForSubtitleAudioEnd(SubtitleCycle* cycle, std::string_view reason) {
    if (!cycle || cycle->waiting_for_audio_end) return;
    cycle->waiting_for_audio_end = true;
    LogCoordinatorLine("waiting for subtitle audio END VS-" + cycle->device_id +
                       (reason.empty() ? std::string() : " reason=" + std::string(reason)));
    if (config_.interaction_mode != InteractionMode::kHoldToTalk) {
        ui_->SetStatus("Processing");
        SendDisplayState("thinking", "", cycle->device_id);
    }
    ScheduleSubtitleAudioEndTimeout(cycle->device_id, cycle->session_id);
}

void VoiceStickCoordinator::ScheduleSubtitleAudioEndTimeout(const std::string& device_id,
                                                            std::uint32_t session_id) {
    auto* cycle = FindSubtitleCycle(device_id, session_id);
    if (!cycle) return;
    const auto generation = ++cycle->audio_end_wait_generation;
    std::thread([this, alive = alive_, device_id, session_id, generation] {
        std::this_thread::sleep_for(kAudioEndTimeout);
        if (!alive->load()) return;
        auto* cycle = FindSubtitleCycle(device_id, session_id);
        if (!cycle || !cycle->waiting_for_audio_end ||
            cycle->audio_end_wait_generation != generation) {
            return;
        }
        LogCoordinatorLine("subtitle audio END timeout VS-" + device_id +
                           "; finalizing buffered audio");
        SendSubtitleFinalOggChunkIfNeeded(device_id, session_id);
    }).detach();
}

void VoiceStickCoordinator::CancelSubtitleAudioEndTimeout(SubtitleCycle* cycle) {
    if (!cycle) return;
    cycle->waiting_for_audio_end = false;
    ++cycle->audio_end_wait_generation;
}

void VoiceStickCoordinator::FinishSubtitleAudioInput(SubtitleCycle* cycle) {
    if (!cycle) return;
    if (config_.interaction_mode == InteractionMode::kHoldToTalk) {
        ClearActiveSubtitleSession(cycle->device_id, cycle->session_id);
        SendDisplayState("ready", "", cycle->device_id);
    } else {
        ui_->SetStatus("Processing");
        SendDisplayState("thinking", "", cycle->device_id);
    }
}

void VoiceStickCoordinator::SendSubtitleFinalOggChunkIfNeeded(const std::string& device_id,
                                                              std::uint32_t session_id) {
    auto* cycle = FindSubtitleCycle(device_id, session_id);
    if (!cycle) return;
    if (cycle->sent_final_audio_chunk) return;
    cycle->sent_final_audio_chunk = true;
    CancelSubtitleAudioEndTimeout(cycle);
    const double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cycle->started_at).count();
    if (!cycle->asr_started && duration < kMinimumRecordingDurationSeconds) {
        const bool was_active = IsActiveSubtitleCycle(device_id, session_id);
        ClearActiveSubtitleSession(device_id, session_id);
        auto it = subtitle_cycles_.find({device_id, session_id});
        if (it != subtitle_cycles_.end()) {
            if (it->second->asr) it->second->asr->Cancel();
            it->second->debug_audio_recorder.Discard();
            subtitle_cycles_.erase(it);
        }
        if (was_active) {
            ui_->HideOverlay();
            ui_->SetStatus("Ready");
            SendDisplayState("ready", "", device_id);
        }
        return;
    }
    if (cycle->received_audio_frames == 0) {
        FinishSubtitleCycleWithError(device_id, session_id, "No audio frames from device");
        return;
    }
    auto final_chunk = cycle->ogg_muxer.Finish();
    cycle->debug_audio_recorder.Append(final_chunk);
    cycle->debug_audio_recorder.Finish();
    SendOrBufferSubtitleOggChunk(cycle, final_chunk, true, true);
    if (auto* updated = FindSubtitleCycle(device_id, session_id)) {
        FinishSubtitleAudioInput(updated);
    }
}

void VoiceStickCoordinator::SendOrBufferSubtitleOggChunk(SubtitleCycle* cycle,
                                                         const ByteVector& chunk,
                                                         bool is_last,
                                                         bool can_start_asr) {
    if (!cycle || !cycle->asr) return;
    if (cycle->asr_started) {
        cycle->asr->SendOggOpusChunk(chunk, is_last);
        return;
    }
    cycle->buffered_ogg_chunks.push_back(chunk);
    const auto device_id = cycle->device_id;
    const auto session_id = cycle->session_id;
    if (can_start_asr && !StartSubtitleAsrAndFlushBufferedChunks(cycle, is_last)) {
        std::string message = "Failed to start ASR";
        if (auto* current_cycle = FindSubtitleCycle(device_id, session_id); current_cycle && current_cycle->asr) {
            message = AsrStartFailureMessage(*current_cycle->asr);
        }
        FinishSubtitleCycleWithError(device_id, session_id, message);
    }
}

bool VoiceStickCoordinator::StartSubtitleAsrAndFlushBufferedChunks(SubtitleCycle* cycle,
                                                                   bool last_chunk_is_final) {
    if (!cycle || !cycle->asr) return false;
    if (cycle->asr_started) return true;
    const auto options = SessionOptions(OutputProfileForDevice(cycle->device_id));
    const auto device_id = cycle->device_id;
    const auto session_id = cycle->session_id;
    auto* asr = cycle->asr.get();
    if (!asr->Start(options)) {
        if (auto* current_cycle = FindSubtitleCycle(device_id, session_id)) {
            current_cycle->buffered_ogg_chunks.clear();
        }
        return false;
    }
    cycle = FindSubtitleCycle(device_id, session_id);
    if (!cycle || cycle->asr.get() != asr) return false;
    cycle->asr_started = true;
    for (std::size_t i = 0; i < cycle->buffered_ogg_chunks.size(); ++i) {
        const bool is_last = (i + 1 == cycle->buffered_ogg_chunks.size()) && last_chunk_is_final;
        cycle->asr->SendOggOpusChunk(cycle->buffered_ogg_chunks[i], is_last);
    }
    cycle->buffered_ogg_chunks.clear();
    return true;
}

void VoiceStickCoordinator::SendFinalOggChunkIfNeeded(double recording_duration_seconds) {
    if (sent_final_audio_chunk_) return;
    sent_final_audio_chunk_ = true;
    CancelAudioEndTimeout();
    if (!asr_started_ && recording_duration_seconds < kMinimumRecordingDurationSeconds) {
        CancelShortRecording();
        return;
    }
    if (received_audio_frames_ == 0) {
        active_session_id_.reset();
        FinishWithAsrError("No audio frames from device");
        return;
    }
    auto final_chunk = ogg_muxer_.Finish();
    debug_audio_recorder_.Append(final_chunk);
    debug_audio_recorder_.Finish();
    active_session_id_.reset();
    EnterFinalizing("final_audio_sent");
    SendOrBufferOggChunk(final_chunk, true, true);
}

void VoiceStickCoordinator::SendOrBufferOggChunk(const ByteVector& chunk, bool is_last, bool can_start_asr) {
    if (asr_started_) {
        asr_->SendOggOpusChunk(chunk, is_last);
        return;
    }
    buffered_ogg_chunks_.push_back(chunk);
    if (can_start_asr && !StartAsrAndFlushBufferedChunks(is_last)) {
        if (!is_showing_asr_error_) FinishWithAsrError(AsrStartFailureMessage(*asr_));
    }
}

bool VoiceStickCoordinator::StartAsrAndFlushBufferedChunks(bool last_chunk_is_final) {
    if (asr_started_) return true;
    const auto options = SessionOptions(OutputProfileForDevice(active_device_id_));
    if (!asr_->Start(options)) {
        buffered_ogg_chunks_.clear();
        return false;
    }
    asr_started_ = true;
    for (std::size_t i = 0; i < buffered_ogg_chunks_.size(); ++i) {
        const bool is_last = (i + 1 == buffered_ogg_chunks_.size()) && last_chunk_is_final;
        asr_->SendOggOpusChunk(buffered_ogg_chunks_[i], is_last);
    }
    buffered_ogg_chunks_.clear();
    return true;
}

void VoiceStickCoordinator::CancelShortRecording() {
    active_session_id_.reset();
    active_session_started_at_ = {};
    CancelAudioEndTimeout();
    buffered_ogg_chunks_.clear();
    asr_->Cancel();
    debug_audio_recorder_.Discard();
    FinishRecognitionCycle();
    EnterReady("short_recording");
}

void VoiceStickCoordinator::FinishWithFinalText(const AsrFinalResult& final_result) {
    if (pasted_final_text_) return;
    pasted_final_text_ = true;
    const auto& text = final_result.text;
    auto profile = OutputProfileForDevice(active_device_id_);
    if (final_result.handled_by_cloud) profile.transform = TextTransform::kOriginal;
    if (profile.target == OutputTarget::kSubtitle) {
        if (!text.empty() && active_device_id_.has_value()) {
            ShowSubtitleText(text, profile, *active_device_id_);
        }
        pending_paste_state_ = {};
        FinishRecognitionCycle();
        EnterReady("subtitle_final_done");
        return;
    }
    if (text.empty()) {
        pending_paste_state_ = {};
        FinishRecognitionCycle();
        ui_->HideOverlay();
        EnterReady("empty_final_done", false);
        return;
    }
    if (profile.transform != TextTransform::kOriginal) {
        if (profile.transform == TextTransform::kRefine) ui_->ShowPartial(text, active_device_id_);
        ui_->SetStatus(profile.transform == TextTransform::kRefine ? "Refining" : "Translating");
        const auto generation = ++text_transform_generation_;
        TransformText(text, profile, [this, generation](bool ok, std::string result) {
            if (generation != text_transform_generation_) return;
            if (ok) {
                RememberInput(result);
                EnterPendingConfirmation(result, "text_transform_final");
            } else {
                FinishWithAsrError(result);
            }
        });
        return;
    }
    RememberInput(text);
    EnterPendingConfirmation(text, "asr_final");
}

void VoiceStickCoordinator::HandleDefiniteSegment(const AsrSegment& segment) {
    if (!segment.definite || !active_device_id_.has_value()) return;
    const auto profile = OutputProfileForDevice(active_device_id_);
    if (!ShouldUseDefiniteSegments(profile)) return;
    ui_->HideOverlay();
    ShowSubtitleText(segment.text, profile, *active_device_id_);
}

void VoiceStickCoordinator::HandleSubtitleDefiniteSegment(const AsrSegment& segment,
                                                          const std::string& device_id) {
    if (!segment.definite) return;
    const auto profile = OutputProfileForDevice(device_id);
    if (!ShouldUseDefiniteSegments(profile)) return;
    ui_->HideOverlay();
    ShowSubtitleText(segment.text, profile, device_id);
}

void VoiceStickCoordinator::FinishSubtitleCycleWithFinalText(const std::string& device_id,
                                                             std::uint32_t session_id,
                                                             const AsrFinalResult& result) {
    const auto& text = result.text;
    auto it = subtitle_cycles_.find({device_id, session_id});
    if (it == subtitle_cycles_.end() || it->second->finished_final_text) return;
    it->second->finished_final_text = true;
    auto profile = OutputProfileForDevice(device_id);
    if (result.handled_by_cloud) profile.transform = TextTransform::kOriginal;
    if (!text.empty()) {
        ShowSubtitleText(text, profile, device_id, [this, device_id, session_id](bool did_show_subtitle) {
            FinishSubtitleCycle(
                device_id,
                session_id,
                did_show_subtitle && ShouldHideOverlayForFinishedSubtitleCycle(device_id, session_id));
        }, session_id);
        return;
    }
    FinishSubtitleCycle(device_id, session_id,
                        ShouldHideOverlayForFinishedSubtitleCycle(device_id, session_id));
}

void VoiceStickCoordinator::FinishSubtitleCycleWithError(const std::string& device_id,
                                                         std::uint32_t session_id,
                                                         const std::string& message) {
    auto it = subtitle_cycles_.find({device_id, session_id});
    if (it == subtitle_cycles_.end()) return;
    LogCoordinatorLine("subtitle ASR error VS-" + device_id + ": " + message);
    if (it->second->asr) it->second->asr->Cancel();
    it->second->debug_audio_recorder.Discard();
    CancelSubtitleAudioEndTimeout(it->second.get());
    ClearActiveSubtitleSession(device_id, session_id);
    ui_->ShowError(message, device_id, [this, device_id] {
        SendDisplayState("ready", "", device_id);
    });
    subtitle_cycles_.erase(it);
}

void VoiceStickCoordinator::CancelSubtitleCycle(const std::string& device_id, std::string_view reason) {
    auto* cycle = FindActiveSubtitleCycle(device_id);
    if (!cycle) return;
    const auto session_id = cycle->session_id;
    auto it = subtitle_cycles_.find({device_id, session_id});
    if (it == subtitle_cycles_.end()) return;
    LogCoordinatorLine("cancel subtitle cycle VS-" + device_id + " reason=" + std::string(reason));
    if (it->second->asr) it->second->asr->Cancel();
    it->second->debug_audio_recorder.Discard();
    CancelSubtitleAudioEndTimeout(it->second.get());
    ui_->HideOverlay();
    SendDisplayState("ready", "", device_id);
    ClearActiveSubtitleSession(device_id, session_id);
    subtitle_cycles_.erase(it);
}

void VoiceStickCoordinator::FinishSubtitleCycle(const std::string& device_id,
                                                std::uint32_t session_id,
                                                bool hide_overlay) {
    auto it = subtitle_cycles_.find({device_id, session_id});
    if (it == subtitle_cycles_.end()) return;
    LogCoordinatorLine("finish subtitle cycle VS-" + device_id +
                       " session=" + std::to_string(session_id) +
                       " hide_overlay=" + (hide_overlay ? "true" : "false"));
    if (hide_overlay) ui_->HideOverlay();
    ClearActiveSubtitleSession(device_id, session_id);
    if (!HasActiveSubtitleSession(device_id)) {
        ui_->SetStatus("Ready");
        SendDisplayState("ready", "", device_id);
    }
    subtitle_cycles_.erase(it);
}

bool VoiceStickCoordinator::ShouldHideOverlayForFinishedSubtitleCycle(
    const std::string& device_id,
    std::uint32_t session_id) const {
    return IsActiveSubtitleCycle(device_id, session_id) || !HasActiveSubtitleSession(device_id);
}

bool VoiceStickCoordinator::CanUpdateOverlayForSubtitleCycle(
    const std::string& device_id,
    std::uint32_t session_id) const {
    auto it = active_subtitle_sessions_.find(device_id);
    return it == active_subtitle_sessions_.end() || it->second == session_id;
}

bool VoiceStickCoordinator::ShouldSendPartialToDevice() const {
    return sent_final_audio_chunk_;
}

bool VoiceStickCoordinator::ShouldSendSubtitlePartialToDevice(const SubtitleCycle* cycle) const {
    return cycle && IsActiveSubtitleCycle(cycle->device_id, cycle->session_id) &&
           cycle->sent_final_audio_chunk;
}

VoiceStickCoordinator::SubtitleCycle* VoiceStickCoordinator::FindSubtitleCycle(
    const std::string& device_id,
    std::uint32_t session_id) {
    auto it = subtitle_cycles_.find({device_id, session_id});
    return it == subtitle_cycles_.end() ? nullptr : it->second.get();
}

VoiceStickCoordinator::SubtitleCycle* VoiceStickCoordinator::FindActiveSubtitleCycle(
    const std::string& device_id) {
    auto it = active_subtitle_sessions_.find(device_id);
    if (it == active_subtitle_sessions_.end()) return nullptr;
    return FindSubtitleCycle(device_id, it->second);
}

bool VoiceStickCoordinator::IsActiveSubtitleCycle(const std::string& device_id,
                                                  std::uint32_t session_id) const {
    auto it = active_subtitle_sessions_.find(device_id);
    return it != active_subtitle_sessions_.end() && it->second == session_id;
}

bool VoiceStickCoordinator::HasActiveSubtitleSession(const std::string& device_id) const {
    return active_subtitle_sessions_.contains(device_id);
}

void VoiceStickCoordinator::ClearActiveSubtitleSession(const std::string& device_id,
                                                       std::uint32_t session_id) {
    auto it = active_subtitle_sessions_.find(device_id);
    if (it != active_subtitle_sessions_.end() && it->second == session_id) {
        active_subtitle_sessions_.erase(it);
    }
}

void VoiceStickCoordinator::CancelSubtitleCyclesForDevice(const std::string& device_id,
                                                          std::string_view reason) {
    LogCoordinatorLine("cancel subtitle cycles VS-" + device_id + " reason=" + std::string(reason));
    active_subtitle_sessions_.erase(device_id);
    for (auto it = subtitle_cycles_.begin(); it != subtitle_cycles_.end();) {
        if (it->first.first != device_id) {
            ++it;
            continue;
        }
        if (it->second->asr) it->second->asr->Cancel();
        it->second->debug_audio_recorder.Discard();
        CancelSubtitleAudioEndTimeout(it->second.get());
        it = subtitle_cycles_.erase(it);
    }
    ui_->HideOverlay();
    SendDisplayState("ready", "", device_id);
}

void VoiceStickCoordinator::ShowSubtitleText(const std::string& text,
                                             const OutputProfile& profile,
                                             const std::string& device_id,
                                             std::function<void(bool)> completion,
                                             std::optional<std::uint32_t> session_id) {
    TransformText(text, profile, [this, device_id, session_id,
                                  completion = std::move(completion)](bool ok, std::string result) mutable {
        if (session_id && !FindSubtitleCycle(device_id, *session_id)) return;
        if (ok) {
            if (!HasActiveSubtitleSession(device_id)) ui_->HideOverlay();
            ui_->ShowSubtitle(result, device_id, ThemeColorForDevice(device_id));
            if (completion) completion(true);
        } else {
            ui_->ShowError(result, device_id, [] {});
            if (completion) completion(false);
        }
    });
}

void VoiceStickCoordinator::TransformText(const std::string& text,
                                          const OutputProfile& profile,
                                          std::function<void(bool, std::string)> completion) {
    if (profile.transform == TextTransform::kOriginal) {
        completion(true, text);
        return;
    }
    translator_.Transform(text, profile, config_.asr_hotwords,
                          [alive = alive_, post_to_ui = post_to_ui_, transform = profile.transform,
                           original = text, completion = std::move(completion)](bool ok, std::string result) mutable {
        if (!alive->load()) return;
        if (!ok && transform == TextTransform::kRefine) {
            ok = true;
            result = std::move(original);
        }
        auto deliver = [alive, ok, result = std::move(result), completion = std::move(completion)]() mutable {
            if (alive->load()) completion(ok, std::move(result));
        };
        if (post_to_ui) post_to_ui(std::move(deliver));
        else deliver();
    });
}

void VoiceStickCoordinator::BeginWaitingForAudioEnd(std::string_view reason) {
    if (waiting_for_audio_end_.load()) return;
    waiting_for_audio_end_.store(true);
    LogCoordinatorLine(std::string("waiting for audio END") +
                       (reason.empty() ? std::string() : " reason=" + std::string(reason)));
    EnterFinalizing("waiting_audio_end");
    ScheduleAudioEndTimeout(active_session_id_, active_device_id_);
}

void VoiceStickCoordinator::ScheduleAudioEndTimeout(std::optional<std::uint32_t> session_id,
                                                    std::optional<std::string> device_id) {
    const auto generation = audio_end_wait_generation_.fetch_add(1) + 1;
    std::thread([this, alive = alive_, generation, session_id, device_id = std::move(device_id)] {
        std::this_thread::sleep_for(kAudioEndTimeout);
        if (!alive->load()) return;
        std::lock_guard lock(audio_mutex_);
        if (audio_end_wait_generation_.load() != generation ||
            !waiting_for_audio_end_.load() ||
            active_session_id_ != session_id ||
            active_device_id_ != device_id) {
            return;
        }
        LogCoordinatorLine("audio END timeout; finalizing buffered audio");
        SendFinalOggChunkIfNeeded(CurrentRecordingDurationSeconds());
    }).detach();
}

void VoiceStickCoordinator::CancelAudioEndTimeout() {
    waiting_for_audio_end_.store(false);
    audio_end_wait_generation_.fetch_add(1);
}

void VoiceStickCoordinator::FinishWithAsrError(const std::string& message, const std::string& partial_text) {
    FinishRecognitionCycle();
    asr_->Cancel();
    pending_paste_state_ = {};
    active_session_id_.reset();
    debug_audio_recorder_.Discard();
    if (!partial_text.empty()) {
        ui_->SetStatus("Awaiting confirmation");
        EnterPausedConfirmation(partial_text, "asr_error_with_partial");
        return;
    }
    EnterError(message, "asr_error");
}

void VoiceStickCoordinator::RecoverFromAsrError(bool hide_overlay) {
    if (!is_showing_asr_error_) return;
    is_showing_asr_error_ = false;
    EnterReady("error_recovered", hide_overlay);
}

void VoiceStickCoordinator::ConfirmPendingPaste() {
    if (pending_paste_state_.kind != PendingPasteKind::kPaused || sending_confirmation_) return;
    const auto edited = ui_->EditedConfirmationText();
    const auto text = edited.value_or(pending_paste_state_.text);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return;
    const auto confirmation_id = pending_paste_state_.confirmation_id;
    const bool use_current_field = pending_paste_state_.use_current_field;
    sending_confirmation_ = true;
    const bool pasted = input_injector_->PasteConfirmation(text, config_.auto_enter && !use_current_field,
        use_current_field, [this] { ui_->ConcealConfirmation(); });
    sending_confirmation_ = false;
    if (pending_paste_state_.confirmation_id != confirmation_id) return;
    if (pasted) {
        FinishPendingPaste(text);
    } else {
        pending_paste_state_.text = text;
        ShowConfirmationPasteFailure();
    }
}

void VoiceStickCoordinator::DismissPendingConfirmation() {
    if (pending_paste_state_.IsIdle()) return;
    StopShakeConfirmation();
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("cancel_pending_paste");
}

void VoiceStickCoordinator::CompletePendingPaste(const std::string& text) {
    if (!input_injector_->Paste(text, config_.auto_enter)) {
        EnterPausedConfirmation(text, "paste_failed");
        ShowConfirmationPasteFailure();
        return;
    }
    FinishPendingPaste(text);
}

void VoiceStickCoordinator::ShowConfirmationPasteFailure() {
    pending_paste_state_.use_current_field = true;
    ui_->ShowPasteFailure();
    if (active_device_id_) RefreshDeviceUiState(*active_device_id_);
}

void VoiceStickCoordinator::FinishPendingPaste(const std::string& text) {
    RememberInput(text);
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("paste_complete");
}

void VoiceStickCoordinator::RememberInput(const std::string& text) {
    last_recoverable_text_ = text;
    last_recoverable_device_id_ = active_device_id_;
    input_injector_->RememberTarget();
    ui_->SetHasRecoverableInput(true);
}

bool VoiceStickCoordinator::RestoreLastInputConfirmation(std::optional<std::string> device_id) {
    if (!pending_paste_state_.IsIdle() || session_state_ != SessionState::kReady ||
        active_session_id_.has_value() || !last_recoverable_text_.has_value()) {
        return false;
    }
    active_device_id_ = std::move(device_id);
    input_injector_->RestoreTarget();
    EnterPausedConfirmation(*last_recoverable_text_, "restore_last_input");
    return true;
}

bool VoiceStickCoordinator::HandleFrontButtonDuringPendingPaste(const std::string& device_id) {
    if (pending_paste_state_.IsIdle()) return false;
    if (active_device_id_ != device_id) return true;
    if (pending_paste_state_.kind == PendingPasteKind::kWaitingToPaste) {
        EnterPausedConfirmation(pending_paste_state_.text, "pause_pending_paste");
        return true;
    }
    ConfirmPendingPaste();
    return true;
}

void VoiceStickCoordinator::CancelPendingPaste(const std::string& device_id) {
    if (active_session_id_.has_value()) {
        if (active_device_id_ == device_id) {
            CancelRecognitionInProgress();
        }
        return;
    }
    if (IsWaitingForFinalText()) {
        if (active_device_id_ == device_id) CancelRecognitionInProgress();
        else RefreshDeviceUiState(device_id);
        return;
    }
    if (pending_paste_state_.IsIdle()) {
        RestoreLastInputConfirmation(device_id);
        return;
    }
    if (active_device_id_ != device_id) return;
    DismissPendingConfirmation();
}

void VoiceStickCoordinator::CancelRecognitionInProgress() {
    active_session_id_.reset();
    active_session_started_at_ = {};
    CancelAudioEndTimeout();
    asr_->Cancel();
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("cancel_recognition");
}

void VoiceStickCoordinator::CancelActiveCycleIfDeviceDisconnected() {
    if (is_shutdown_) return;
    for (auto it = subtitle_cycles_.begin(); it != subtitle_cycles_.end();) {
        const auto& device_id = it->first.first;
        if (!ble_->IsConnected(device_id)) {
            if (it->second->asr) it->second->asr->Cancel();
            it->second->debug_audio_recorder.Discard();
            ui_->HideOverlay();
            active_subtitle_sessions_.erase(device_id);
            it = subtitle_cycles_.erase(it);
        } else {
            ++it;
        }
    }
    if (session_state_ != SessionState::kPausedConfirmation &&
        active_device_id_.has_value() && !ble_->IsConnected(*active_device_id_)) {
        if (waiting_for_audio_end_.load()) {
            SendFinalOggChunkIfNeeded(CurrentRecordingDurationSeconds());
            return;
        }
        asr_->Cancel();
        pending_paste_state_ = {};
        active_session_id_.reset();
        debug_audio_recorder_.Discard();
        FinishRecognitionCycle();
        EnterReady("device_disconnected");
    }
}

void VoiceStickCoordinator::FinishRecognitionCycle() {
    ++text_transform_generation_;
    CancelAudioEndTimeout();
    asr_started_ = false;
    sent_final_audio_chunk_ = false;
    pasted_final_text_ = false;
    buffered_ogg_chunks_.clear();
}

void VoiceStickCoordinator::CheckFirmwareUpdates() {
    if (is_shutdown_ || paired_device_ids_.empty()) return;
    for (const auto hardware : FirmwareManifestClient::supported_hardware) {
        CheckFirmwareFeed(hardware, false);
    }
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::CheckDeviceFirmwareUpdates(const std::string& device_id,
                                                        std::function<void(DeviceFirmwareInfo)> completion) {
    DeviceFirmwareInfo info;
    {
        std::lock_guard lock(firmware_mutex_);
        if (const auto entry = firmware_info_by_device_id_.find(device_id); entry != firmware_info_by_device_id_.end()) {
            info = entry->second;
        }
    }
    if (info.current_version.empty() || info.hardware.empty()) {
        info.error_message = "Connect to the device and wait for its firmware version to appear.";
        completion(std::move(info));
        return;
    }
    if (FirmwareManifestClient::ManifestUrlForHardware(info.hardware).empty()) {
        info.error_message = "There is no firmware update feed for this device model.";
        completion(std::move(info));
        return;
    }
    const auto hardware = info.hardware;
    CheckFirmwareFeed(hardware, true,
        [info = std::move(info), completion = std::move(completion)]
        (const FirmwareManifest* manifest, std::string_view error) mutable {
            completion(EvaluateFirmwareAvailability(std::move(info), manifest, error, false, VOICESTICK_VERSION_STR));
        });
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::CheckFirmwareFeed(std::string_view hardware, bool force,
                                               FirmwareFeedCompletion completion) {
    const auto url = FirmwareManifestClient::ManifestUrlForHardware(hardware);
    if (url.empty() || is_shutdown_) return;

    std::thread previous_check;
    {
        std::lock_guard lock(firmware_mutex_);
        auto& feed = firmware_feeds_[url];
        if (!force && std::chrono::steady_clock::now() < feed.next_check) return;
        if (completion) feed.completions.push_back(std::move(completion));
        if (feed.is_checking) return;
        feed.is_checking = true;
        feed.error_message.clear();
        previous_check = std::move(feed.worker);
    }
    if (previous_check.joinable()) previous_check.join();

    std::thread worker([this, alive = alive_, hardware = std::string(hardware), url] {
        std::string error;
        auto manifest = FirmwareManifestClient(url).FetchManifestSync(error);
        if (manifest && !IsFirmwareHardwareCompatible(hardware, {}, manifest->hardware)) {
            manifest.reset();
            error = "Firmware manifest does not match the expected device model.";
        }
        if (!alive->load()) return;
        const bool compatible = manifest &&
            manifest->CheckWindowsAppVersion(VOICESTICK_VERSION_STR) == FirmwareAppCompatibility::kSupported;
        if (manifest) {
            LogCoordinatorLine("firmware manifest version=" + manifest->version + " hardware=" + manifest->hardware);
        } else {
            LogCoordinatorLine("firmware manifest check failed for " + hardware + ": " + error);
        }

        std::vector<FirmwareFeedCompletion> completions;
        {
            std::lock_guard lock(firmware_mutex_);
            auto& feed = firmware_feeds_.at(url);
            feed.next_check = std::chrono::steady_clock::now() +
                (compatible ? kFirmwareManifestCacheDuration : kFirmwareManifestRetryDelay);
            feed.manifest = manifest;
            feed.error_message = error;
            feed.is_checking = false;
            completions.swap(feed.completions);
        }
        RefreshFirmwareAvailability();
        for (auto& notify : completions) {
            if (!alive->load()) return;
            notify(manifest ? &*manifest : nullptr, error);
        }
    });
    std::lock_guard lock(firmware_mutex_);
    firmware_feeds_.at(url).worker = std::move(worker);
}

void VoiceStickCoordinator::RefreshFirmwareAvailability() {
    std::map<std::string, DeviceFirmwareInfo> snapshot;
    std::uint64_t revision;
    {
        std::lock_guard lock(firmware_mutex_);
        for (const auto& [device_id, info] : firmware_info_by_device_id_) {
            const auto url = FirmwareManifestClient::ManifestUrlForHardware(info.hardware);
            const auto feed = firmware_feeds_.find(url);
            if (feed == firmware_feeds_.end()) {
                snapshot.emplace(device_id, info);
                continue;
            }
            const auto& value = feed->second;
            snapshot.emplace(device_id, EvaluateFirmwareAvailability(
                info, value.manifest ? &*value.manifest : nullptr,
                value.error_message, value.is_checking, VOICESTICK_VERSION_STR));
        }
        revision = ++firmware_info_revision_;
    }
    ui_->SetFirmwareInfo(snapshot, revision);
}

bool VoiceStickCoordinator::IsWaitingForFinalText() const {
    return session_state_ == SessionState::kFinalizing;
}

void VoiceStickCoordinator::SetSessionState(SessionState state, std::string_view reason) {
    if (session_state_ == state) return;
    auto state_name = [](SessionState value) {
        switch (value) {
        case SessionState::kReady: return "ready";
        case SessionState::kRecording: return "recording";
        case SessionState::kFinalizing: return "finalizing";
        case SessionState::kPendingConfirmation: return "pending_confirmation";
        case SessionState::kPausedConfirmation: return "paused_confirmation";
        case SessionState::kError: return "error";
        }
        return "unknown";
    };
    LogCoordinatorLine("state " + std::string(state_name(session_state_)) +
                       " -> " + state_name(state) +
                       (reason.empty() ? std::string() : " reason=" + std::string(reason)));
    session_state_ = state;
}

void VoiceStickCoordinator::EnterReady(std::string_view reason, bool hide_overlay) {
    StopShakeConfirmation();
    const auto previous_device = active_device_id_;
    SetSessionState(SessionState::kReady, reason);
    ui_->SetStatus("Ready");
    SendDisplayState("ready", "", previous_device);
    if (previous_device) {
        ble_->StartAudioIn(*previous_device, config_.interaction_mode);
    }
    if (hide_overlay) ui_->HideOverlay();
    active_device_id_.reset();
}

void VoiceStickCoordinator::EnterFinalizing(std::string_view reason) {
    SetSessionState(SessionState::kFinalizing, reason);
    ui_->SetStatus("Processing");
    SendUiStateForActiveDevice("thinking");
}

void VoiceStickCoordinator::EnterPendingConfirmation(const std::string& text, std::string_view reason) {
    const int duration_ms = config_.send_confirmation_ms;
    if (duration_ms == 0) {
        CompletePendingPaste(text);
        return;
    }
    input_injector_->CaptureConfirmationTarget();
    if (duration_ms < 0) {
        EnterPausedConfirmation(text, reason);
        return;
    }
    if (active_device_id_) {
        ble_->StopAudioIn(*active_device_id_);
    }
    const auto generation = ++confirmation_generation_;
    pending_paste_state_ = {PendingPasteKind::kWaitingToPaste, text, {}};
    SetSessionState(SessionState::kPendingConfirmation, reason);
    ui_->ShowFinalCountdown(text, active_device_id_, duration_ms, [this, generation, text] {
        if (confirmation_generation_ == generation &&
            pending_paste_state_.kind == PendingPasteKind::kWaitingToPaste) {
            CompletePendingPaste(text);
        }
    });
    SendUiStateForActiveDevice("pending_confirmation", text);
}

void VoiceStickCoordinator::EnterPausedConfirmation(const std::string& text, std::string_view reason) {
    pending_paste_state_ = {PendingPasteKind::kPaused, text, NewConfirmationId()};
    SetSessionState(SessionState::kPausedConfirmation, reason);
    ui_->SetStatus("Awaiting confirmation");
    ui_->ShowPausedFinal(text, active_device_id_);
    if (active_device_id_) {
        ble_->StopAudioIn(*active_device_id_);
        RefreshDeviceUiState(*active_device_id_);
        StartShakeConfirmation(*active_device_id_);
    }
}

void VoiceStickCoordinator::EnterError(const std::string& message, std::string_view reason) {
    is_showing_asr_error_ = true;
    SetSessionState(SessionState::kError, reason);
    SendUiStateForActiveDevice("error", message);
    ui_->ShowError(message, active_device_id_, [this] { RecoverFromAsrError(false); });
}

void VoiceStickCoordinator::RefreshDeviceUiState(const std::string& device_id) {
    switch (session_state_) {
    case SessionState::kRecording:
        SendDisplayState(active_device_id_ == device_id ? "recording" : "ready", "", device_id);
        break;
    case SessionState::kFinalizing:
        SendDisplayState(active_device_id_ == device_id ? "thinking" : "ready", "", device_id);
        break;
    case SessionState::kPendingConfirmation:
    case SessionState::kPausedConfirmation:
        if (active_device_id_ == device_id) {
            SendDisplayState("pending_confirmation", pending_paste_state_.text, device_id);
        } else {
            SendDisplayState("ready", "", device_id);
        }
        break;
    case SessionState::kError:
        SendDisplayState(active_device_id_ == device_id ? "error" : "ready", "", device_id);
        break;
    case SessionState::kReady:
        SendDisplayState("ready", "", device_id);
        break;
    }
}

void VoiceStickCoordinator::SendDisplayState(const std::string& state,
                                             const std::string& text,
                                             const std::optional<std::string>& device_id) {
    if (state == "ready") {
        ble_->SetDisplay("idle", "", device_id);
    } else if (state == "recording") {
        ble_->SetDisplay("recording", "", device_id);
    } else if (state == "thinking") {
        ble_->SetDisplay("busy", text, device_id);
    } else if (state == "pending_confirmation") {
        ble_->SetDisplay("busy", "Press to send", device_id);
    } else if (state == "error") {
        ble_->SetDisplay("error", text, device_id);
    } else {
        ble_->SetDisplay("idle", text, device_id);
    }
}

void VoiceStickCoordinator::SendUiStateForActiveDevice(const std::string& state, const std::string& text) {
    SendDisplayState(state, text, active_device_id_);
}

OutputProfile VoiceStickCoordinator::OutputProfileForDevice(const std::optional<std::string>& device_id) const {
    return config_.OutputProfileForDevice(device_id);
}

OverlayThemeColor VoiceStickCoordinator::ThemeColorForDevice(const std::string& device_id) const {
    auto it = config_.device_theme_colors.find(device_id);
    return it == config_.device_theme_colors.end() ? OverlayThemeColor::kWhite : it->second;
}

bool VoiceStickCoordinator::ShouldUseDefiniteSegments(const OutputProfile& profile) const {
    return profile.target == OutputTarget::kSubtitle &&
           config_.interaction_mode == InteractionMode::kClickToTalk &&
           config_.asr_provider != AsrProvider::kOpenAI &&
           profile.transform != TextTransform::kRefine &&
           !(config_.asr_provider == AsrProvider::kVoiceStickCloud && profile.transform != TextTransform::kOriginal);
}

AsrSessionOptions VoiceStickCoordinator::SessionOptions(const OutputProfile& profile) const {
    AsrSessionOptions options;
    options.hotwords = config_.asr_hotwords;
    options.show_utterances = ShouldUseDefiniteSegments(profile);
    options.result_type = options.show_utterances ? AsrResultType::kSingle : AsrResultType::kFull;
    if (config_.asr_provider == AsrProvider::kVoiceStickCloud && profile.transform != TextTransform::kOriginal) {
        options.cloud_text_profile = profile;
    }
    return options;
}

double VoiceStickCoordinator::CurrentRecordingDurationSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - active_session_started_at_).count();
}

} // namespace voicestick
