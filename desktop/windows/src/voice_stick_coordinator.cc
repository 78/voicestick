#include "voice_stick_coordinator.h"

#include "log.h"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <utility>

namespace voicestick {

namespace {

void LogCoordinatorLine(const std::string& message) {
    LogCoordinator(message);
}

bool IsFirmwareManifestCompatible(const DeviceFirmwareInfo& info, const FirmwareManifest& manifest) {
    return IsFirmwareHardwareCompatible(info.hardware, info.current_version, manifest.hardware);
}

} // namespace

VoiceStickCoordinator::VoiceStickCoordinator(AppConfig config,
                                             std::unique_ptr<BleCentral> ble,
                                             std::unique_ptr<AsrClient> asr,
                                             VoiceStickUi* ui,
                                             InputInjector* input_injector,
                                             std::function<std::unique_ptr<AsrClient>(const AppConfig&)> asr_factory)
    : config_(std::move(config)),
      ble_(std::move(ble)),
      asr_(std::move(asr)),
      asr_factory_(std::move(asr_factory)),
      translator_(config_),
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
    if (firmware_manifest_thread_.joinable()) {
        firmware_manifest_thread_.join();
    }
}

void VoiceStickCoordinator::Start() {
    ble_->on_connection_change = [this](std::vector<ConnectedDevice> devices) {
        if (is_shutdown_) return;
        ui_->SetConnectedDevices(devices);
        CancelActiveCycleIfDeviceDisconnected();
        RefreshFirmwareAvailability();
        ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
        ble_->SendInteractionMode(config_.interaction_mode, std::nullopt);
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
    ble_->on_state_event = [this](std::string device_id, StateEvent event) {
        if (is_shutdown_) return;
        HandleStateEvent(event, device_id);
    };
    ble_->on_audio_frame = [this](std::string device_id, AudioFrame frame) {
        if (is_shutdown_) return;
        HandleAudioFrame(frame, device_id);
    };
    ConfigureAsrCallbacks();
    ble_->Start();
    CheckFirmwareUpdatesIfNeeded(false, false);
    std::thread([this, alive = alive_] {
        while (alive->load()) {
            std::this_thread::sleep_for(std::chrono::hours(1));
            if (!alive->load()) return;
            CheckFirmwareUpdatesIfNeeded(false, false);
        }
    }).detach();
}

void VoiceStickCoordinator::Shutdown() {
    if (is_shutdown_) return;
    is_shutdown_ = true;
    ble_->on_connection_change = nullptr;
    ble_->on_connection_error = nullptr;
    ble_->on_scan_error = nullptr;
    ble_->on_state_event = nullptr;
    ble_->on_audio_frame = nullptr;
    asr_->Cancel();
    for (auto& [_, cycle] : subtitle_cycles_) {
        if (cycle->asr) cycle->asr->Cancel();
        cycle->debug_audio_recorder.Discard();
    }
    subtitle_cycles_.clear();
    active_subtitle_sessions_.clear();
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
        asr_->Cancel();
        for (auto& [_, cycle] : subtitle_cycles_) {
            if (cycle->asr) cycle->asr->Cancel();
            cycle->debug_audio_recorder.Discard();
        }
        subtitle_cycles_.clear();
        active_subtitle_sessions_.clear();
        ui_->HideSubtitles();
        active_session_id_.reset();
        pending_paste_state_ = {};
        debug_audio_recorder_.Discard();
        FinishRecognitionCycle();
        EnterReady("config_update_cancel");
    }

    config_ = std::move(config);
    translator_ = LLMTranslationClient(config_);
    ble_->SendInteractionMode(config_.interaction_mode, std::nullopt);
    debug_audio_recorder_ = DebugAudioRecorder(config_.debug_audio_cache, config_.debug_audio_directory);
    if (asr_factory_) {
        asr_ = asr_factory_(config_);
        ConfigureAsrCallbacks();
    }
    if (paired_device_ids_ != config_.paired_device_ids) {
        paired_device_ids_ = config_.paired_device_ids;
        ui_->SetPairedDeviceIds(paired_device_ids_);
        ble_->UpdatePairedDeviceIds(paired_device_ids_);
        CheckFirmwareUpdatesIfNeeded(false, false);
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

void VoiceStickCoordinator::CancelPendingConnect(const std::string& device_id) {
    ble_->CancelPendingConnect(device_id);
}

void VoiceStickCoordinator::ConfirmPairedDeviceIds(const std::vector<std::string>& device_ids) {
    paired_device_ids_ = device_ids;
    config_.paired_device_ids = device_ids;
    ui_->SetPairedDeviceIds(paired_device_ids_);
    ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
    for (const auto& entry : config_.paired_devices) {
        if (std::find(paired_device_ids_.begin(), paired_device_ids_.end(), entry.device_id) == paired_device_ids_.end()) {
            continue;
        }
        auto& info = firmware_info_by_device_id_[entry.device_id];
        if (info.hardware.empty()) info.hardware = entry.hardware;
        if (info.current_version.empty()) info.current_version = entry.firmware_version;
    }
    CheckFirmwareUpdatesIfNeeded(false, false);
}

void VoiceStickCoordinator::RemovePairedDevice(const std::string& device_id) {
    auto it = std::find(paired_device_ids_.begin(), paired_device_ids_.end(), device_id);
    if (it == paired_device_ids_.end()) return;
    paired_device_ids_.erase(it);
    config_.paired_device_ids = paired_device_ids_;
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
    ui_->SetStatus(paired_device_ids_.empty() ? "Pair a VoiceStick" : "Ready");
}

bool VoiceStickCoordinator::RestoreLastInputConfirmation() {
    return RestoreLastInputConfirmation(last_recoverable_device_id_);
}

void VoiceStickCoordinator::CheckFirmwareUpdatesNow() {
    CheckFirmwareUpdatesIfNeeded(true, true);
}

void VoiceStickCoordinator::CheckFirmwareAfterPairing(const std::string& device_id) {
    {
        std::lock_guard lock(firmware_mutex_);
        pending_firmware_update_prompt_device_ids_.insert(device_id);
    }
    CheckFirmwareUpdatesIfNeeded(false, false);
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::UpdateFirmwareFromLatest(
    const std::string& device_id,
    std::function<void(FirmwareUpdateProgress)> progress,
    std::function<void(bool, std::string)> completion) {
    std::optional<FirmwareManifest> manifest;
    {
        std::lock_guard lock(firmware_mutex_);
        manifest = latest_firmware_manifest_;
    }
    if (!manifest.has_value()) {
        completion(false, "Firmware update manifest is not loaded yet.");
        return;
    }

    auto client = firmware_manifest_client_;
    std::thread([this, alive = alive_, client = std::move(client), device_id, manifest = std::move(*manifest),
                 progress = std::move(progress), completion = std::move(completion)]() mutable {
        std::string error;
        auto image = client.DownloadOtaSync(manifest, error);
        if (!alive->load()) return;
        if (!image.has_value()) {
            completion(false, error.empty() ? "Failed to download firmware." : error);
            return;
        }
        ble_->UpdateFirmware(std::move(*image), device_id, std::move(progress), std::move(completion));
    }).detach();
}

void VoiceStickCoordinator::CancelFirmwareUpdate() {
    ble_->CancelFirmwareUpdate();
}

void VoiceStickCoordinator::ConfigureAsrCallbacks() {
    asr_->on_partial = [this](std::string text) {
        ui_->ShowPartial(text, active_device_id_);
        if (ShouldSendPartialToDevice()) {
            SendUiStateForActiveDevice("thinking", text);
        }
    };
    asr_->on_segment = [this](AsrSegment segment) {
        HandleDefiniteSegment(segment);
    };
    asr_->on_final = [this](std::string text) {
        FinishWithFinalText(text);
    };
    asr_->on_error = [this](std::string message) {
        FinishWithAsrError(message);
    };
    asr_->on_upgrade_url = [this](std::string url, std::string message) {
        ui_->ShowCloudUpgrade(message, url, active_device_id_);
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
            ble_->SendUiState("thinking", text, device_id);
        }
    };
    cycle->asr->on_segment = [this, device_id, session_id](AsrSegment segment) {
        if (!IsActiveSubtitleCycle(device_id, session_id)) return;
        HandleSubtitleDefiniteSegment(segment, device_id);
    };
    cycle->asr->on_final = [this, device_id, session_id](std::string text) {
        FinishSubtitleCycleWithFinalText(device_id, session_id, text);
    };
    cycle->asr->on_error = [this, device_id, session_id](std::string message) {
        FinishSubtitleCycleWithError(device_id, session_id, message);
    };
    cycle->asr->on_upgrade_url = [this, device_id](std::string url, std::string message) {
        ui_->ShowCloudUpgrade(message, url, device_id);
    };
}

void VoiceStickCoordinator::HandleStateEvent(const StateEvent& event, const std::string& device_id) {
    if (event.event == "device_info") {
        ui_->SetDeviceInfo(DeviceInfo{device_id, event.hardware, event.firmware_version});
        UpdateDeviceFirmwareInfo(event, device_id);
    } else if (event.event == "button_down") {
        HandleButtonDown(event, device_id);
    } else if (event.event == "button_up") {
        HandleButtonUp(event, device_id);
    } else if (event.event == "button_click") {
        HandleButtonClick(event, device_id);
    }
}

void VoiceStickCoordinator::HandleButtonDown(const StateEvent& event, const std::string& device_id) {
    if (event.button == "primary") {
        HandlePrimaryButtonDown(event.session_id, device_id);
    }
}

void VoiceStickCoordinator::HandleButtonUp(const StateEvent& event, const std::string& device_id) {
    if (event.button == "primary") {
        HandlePrimaryButtonUp(device_id);
    } else if (event.button == "secondary") {
        HandleSecondaryButtonClick(device_id);
    }
}

void VoiceStickCoordinator::HandleButtonClick(const StateEvent& event, const std::string& device_id) {
    if (event.button == "primary") {
        if (config_.default_output_profile.target == OutputTarget::kSubtitle) {
            if (config_.interaction_mode != InteractionMode::kClickToTalk) {
                ble_->SendUiState("ready", "", device_id);
                return;
            }
            if (HasActiveSubtitleSession(device_id)) {
                HandleSubtitlePrimaryButtonUp(device_id);
            } else {
                HandleSubtitlePrimaryButtonDown(event.session_id, device_id);
            }
            return;
        }
        if (HandleFrontButtonDuringPendingPaste(device_id)) return;
        if (config_.interaction_mode != InteractionMode::kClickToTalk) {
            ble_->SendUiState("ready", "", device_id);
            return;
        }
        if (session_state_ == SessionState::kRecording && active_device_id_ == device_id) {
            HandlePrimaryButtonUp(device_id);
        } else if (session_state_ == SessionState::kFinalizing && active_device_id_ == device_id) {
            ble_->SendUiState("thinking", "", device_id);
        } else {
            HandlePrimaryButtonDown(event.session_id, device_id);
        }
    } else if (event.button == "secondary") {
        HandleSecondaryButtonClick(device_id);
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
        ble_->SendUiState("ready", "", device_id);
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
    ui_->ShowListening(active_device_id_);
    SendUiStateForActiveDevice("recording");
}

void VoiceStickCoordinator::HandlePrimaryButtonUp(const std::string& device_id) {
    if (HasActiveSubtitleSession(device_id)) {
        HandleSubtitlePrimaryButtonUp(device_id);
        return;
    }
    std::lock_guard lock(audio_mutex_);
    if (!active_session_id_.has_value() || active_device_id_ != device_id) return;
    const double duration = CurrentRecordingDurationSeconds();
    if (duration < kMinimumRecordingDurationSeconds) {
        CancelShortRecording();
    } else {
        BeginWaitingForAudioEnd("button_up");
    }
}

void VoiceStickCoordinator::HandleAudioFrame(const AudioFrame& frame, const std::string& device_id) {
    const auto t0 = std::chrono::steady_clock::now();
    if (FindSubtitleCycle(device_id, frame.session_id)) {
        HandleSubtitleAudioFrame(frame, device_id);
        return;
    }
    std::lock_guard lock(audio_mutex_);
    if (!active_session_id_.has_value() || frame.session_id != *active_session_id_ || active_device_id_ != device_id) {
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
    SendOrBufferOggChunk(
        ogg_chunk,
        frame.IsEnd(),
        CurrentRecordingDurationSeconds() >= kMinimumRecordingDurationSeconds &&
            (!waiting_for_audio_end_.load() || frame.IsEnd()));
    if (frame.IsEnd()) {
        CancelAudioEndTimeout();
        sent_final_audio_chunk_ = true;
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

void VoiceStickCoordinator::HandleSubtitlePrimaryButtonDown(std::optional<std::uint32_t> session_id,
                                                            const std::string& device_id) {
    if (!session_id.has_value() || *session_id == 0) {
        ble_->SendUiState("ready", "", device_id);
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
            ble_->SendUiState("ready", "", device_id);
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
    ui_->ShowListening(device_id);
    ble_->SendUiState("recording", "", device_id);
}

void VoiceStickCoordinator::HandleSubtitlePrimaryButtonUp(const std::string& device_id) {
    auto* cycle = FindActiveSubtitleCycle(device_id);
    if (!cycle) return;
    const auto session_id = cycle->session_id;
    const double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cycle->started_at).count();
    if (duration < kMinimumRecordingDurationSeconds) {
        CancelSubtitleCycle(device_id, "short_recording");
    } else {
        BeginWaitingForSubtitleAudioEnd(cycle, "button_up");
        FinishSubtitleAudioInput(cycle);
    }
}

void VoiceStickCoordinator::HandleSubtitleAudioFrame(const AudioFrame& frame, const std::string& device_id) {
    auto* cycle = FindSubtitleCycle(device_id, frame.session_id);
    if (!cycle) return;
    if (frame.IsEnd() && frame.payload.empty()) {
        CancelSubtitleAudioEndTimeout(cycle);
        SendSubtitleFinalOggChunkIfNeeded(device_id, frame.session_id);
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
        CancelSubtitleAudioEndTimeout(cycle);
        cycle->sent_final_audio_chunk = true;
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
        ble_->SendUiState("thinking", "", cycle->device_id);
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
        ble_->SendUiState("ready", "", cycle->device_id);
    } else {
        ui_->SetStatus("Processing");
        ble_->SendUiState("thinking", "", cycle->device_id);
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
            ble_->SendUiState("ready", "", device_id);
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
    if (can_start_asr && !StartSubtitleAsrAndFlushBufferedChunks(cycle, is_last)) {
        FinishSubtitleCycleWithError(cycle->device_id, cycle->session_id, "Failed to start ASR");
    }
}

bool VoiceStickCoordinator::StartSubtitleAsrAndFlushBufferedChunks(SubtitleCycle* cycle,
                                                                   bool last_chunk_is_final) {
    if (!cycle || !cycle->asr) return false;
    if (cycle->asr_started) return true;
    AsrSessionOptions options;
    options.hotwords = config_.asr_hotwords;
    const bool use_definite_segments = ShouldUseDefiniteSegments(OutputProfileForDevice(cycle->device_id));
    options.show_utterances = use_definite_segments;
    options.result_type = use_definite_segments ? AsrResultType::kSingle : AsrResultType::kFull;
    if (!cycle->asr->Start(options)) {
        cycle->buffered_ogg_chunks.clear();
        return false;
    }
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
    SendOrBufferOggChunk(final_chunk, true, true);
    EnterFinalizing("final_audio_sent");
}

void VoiceStickCoordinator::SendOrBufferOggChunk(const ByteVector& chunk, bool is_last, bool can_start_asr) {
    if (asr_started_) {
        asr_->SendOggOpusChunk(chunk, is_last);
        return;
    }
    buffered_ogg_chunks_.push_back(chunk);
    if (can_start_asr && !StartAsrAndFlushBufferedChunks(is_last)) {
        FinishWithAsrError("Failed to start ASR");
    }
}

bool VoiceStickCoordinator::StartAsrAndFlushBufferedChunks(bool last_chunk_is_final) {
    if (asr_started_) return true;
    AsrSessionOptions options;
    options.hotwords = config_.asr_hotwords;
    const auto profile = OutputProfileForDevice(active_device_id_);
    const bool use_definite_segments = ShouldUseDefiniteSegments(profile);
    options.show_utterances = use_definite_segments;
    options.result_type = use_definite_segments ? AsrResultType::kSingle : AsrResultType::kFull;
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

void VoiceStickCoordinator::FinishWithFinalText(const std::string& text) {
    if (pasted_final_text_) return;
    pasted_final_text_ = true;
    const auto profile = OutputProfileForDevice(active_device_id_);
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
    if (profile.transform == TextTransform::kTranslate) {
        ui_->SetStatus("Translating");
        TransformText(text, profile, [this](bool ok, std::string result) {
            if (ok) {
                last_recoverable_text_ = result;
                last_recoverable_device_id_ = active_device_id_;
                ui_->SetHasRecoverableInput(true);
                EnterPendingConfirmation(result, "translation_final");
            } else {
                FinishWithAsrError(result);
            }
        });
        return;
    }
    last_recoverable_text_ = text;
    last_recoverable_device_id_ = active_device_id_;
    ui_->SetHasRecoverableInput(true);
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
                                                             const std::string& text) {
    auto it = subtitle_cycles_.find({device_id, session_id});
    if (it == subtitle_cycles_.end() || it->second->finished_final_text) return;
    it->second->finished_final_text = true;
    const auto profile = OutputProfileForDevice(device_id);
    if (!text.empty()) {
        ShowSubtitleText(text, profile, device_id, [this, device_id, session_id](bool did_show_subtitle) {
            FinishSubtitleCycle(
                device_id,
                session_id,
                did_show_subtitle && ShouldHideOverlayForFinishedSubtitleCycle(device_id, session_id));
        });
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
        ble_->SendUiState("ready", "", device_id);
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
    ble_->SendUiState("ready", "", device_id);
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
        ble_->SendUiState("ready", "", device_id);
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
    ble_->SendUiState("ready", "", device_id);
}

void VoiceStickCoordinator::ShowSubtitleText(const std::string& text,
                                             const OutputProfile& profile,
                                             const std::string& device_id,
                                             std::function<void(bool)> completion) {
    TransformText(text, profile, [this, device_id, completion = std::move(completion)](bool ok, std::string result) mutable {
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
    if (profile.transform != TextTransform::kTranslate) {
        completion(true, text);
        return;
    }
    translator_.Translate(text, profile.translation_target, config_.asr_hotwords, std::move(completion));
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

void VoiceStickCoordinator::FinishWithAsrError(const std::string& message) {
    CancelAudioEndTimeout();
    asr_->Cancel();
    pending_paste_state_ = {};
    active_session_id_.reset();
    debug_audio_recorder_.Discard();
    FinishRecognitionCycle();
    EnterError(message, "asr_error");
}

void VoiceStickCoordinator::RecoverFromAsrError(bool hide_overlay) {
    if (!is_showing_asr_error_) return;
    is_showing_asr_error_ = false;
    EnterReady("error_recovered", hide_overlay);
}

void VoiceStickCoordinator::CommitPendingPaste(const std::string& text) {
    if (pending_paste_state_.text == text) CompletePendingPaste(text);
}

void VoiceStickCoordinator::CompletePendingPaste(const std::string& text) {
    const bool should_press_enter = config_.auto_enter;
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("paste_complete", false);
    input_injector_->Paste(text, should_press_enter);
}

bool VoiceStickCoordinator::RestoreLastInputConfirmation(std::optional<std::string> device_id) {
    if (!pending_paste_state_.IsIdle() || session_state_ != SessionState::kReady ||
        active_session_id_.has_value() || !last_recoverable_text_.has_value()) {
        return false;
    }
    active_device_id_ = std::move(device_id);
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
    ui_->HideOverlay([this, text = pending_paste_state_.text] { CommitPendingPaste(text); });
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
    pending_paste_state_ = {};
    FinishRecognitionCycle();
    EnterReady("cancel_pending_paste");
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
    if (active_device_id_.has_value() && !ble_->IsConnected(*active_device_id_)) {
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
    CancelAudioEndTimeout();
    asr_started_ = false;
    sent_final_audio_chunk_ = false;
    pasted_final_text_ = false;
    buffered_ogg_chunks_.clear();
}

void VoiceStickCoordinator::UpdateDeviceFirmwareInfo(const StateEvent& event, const std::string& device_id) {
    std::string hardware_to_save;
    std::string version_to_save;
    {
        std::lock_guard lock(firmware_mutex_);
        auto& info = firmware_info_by_device_id_[device_id];
        if (!event.hardware.empty()) {
            info.hardware = event.hardware;
            hardware_to_save = event.hardware;
        }
        if (!event.firmware_version.empty()) {
            info.current_version = event.firmware_version;
            version_to_save = event.firmware_version;
        }
        info.error_message.clear();
    }
    if (!hardware_to_save.empty() || !version_to_save.empty()) {
        config_.SavePairedDeviceInfo(device_id, hardware_to_save, version_to_save);
    }
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::CheckFirmwareUpdatesIfNeeded(bool force, bool show_errors) {
    if (paired_device_ids_.empty() && !force) return;
    {
        std::lock_guard lock(firmware_mutex_);
        if (firmware_manifest_check_in_flight_) return;
        if (!force && has_last_firmware_manifest_check_at_ &&
            std::chrono::steady_clock::now() - last_firmware_manifest_check_at_ < kFirmwareManifestCacheDuration) {
            // Use the cached manifest to refresh any newly connected device info.
        } else {
            firmware_manifest_check_in_flight_ = true;
            SetFirmwareChecking(true);
            if (firmware_manifest_thread_.joinable()) {
                firmware_manifest_thread_.join();
            }
            auto alive = alive_;
            firmware_manifest_thread_ = std::thread([this, alive, show_errors] {
                std::string error;
                auto manifest = firmware_manifest_client_.FetchManifestSync(error);
                if (!alive->load()) return;
                {
                    std::lock_guard callback_lock(firmware_mutex_);
                    firmware_manifest_check_in_flight_ = false;
                    for (auto& [_, info] : firmware_info_by_device_id_) {
                        info.is_checking = false;
                    }
                    if (manifest.has_value()) {
                        LogCoordinatorLine("firmware manifest version=" + manifest->version +
                                           " hardware=" + manifest->hardware);
                        latest_firmware_manifest_ = std::move(manifest);
                        last_firmware_manifest_check_at_ = std::chrono::steady_clock::now();
                        has_last_firmware_manifest_check_at_ = true;
                        for (auto& [_, info] : firmware_info_by_device_id_) {
                            info.error_message.clear();
                        }
                    } else {
                        LogCoordinatorLine("firmware manifest check failed: " + error);
                        for (const auto& device_id : paired_device_ids_) {
                            if (show_errors || firmware_info_by_device_id_.contains(device_id)) {
                                firmware_info_by_device_id_[device_id].error_message = error;
                            }
                        }
                    }
                }
                RefreshFirmwareAvailability();
            });
            return;
        }
    }
    RefreshFirmwareAvailability();
}

void VoiceStickCoordinator::RefreshFirmwareAvailability() {
    std::map<std::string, DeviceFirmwareInfo> snapshot;
    std::vector<std::tuple<std::string, std::string, std::string, bool>> update_prompts;
    {
        std::lock_guard lock(firmware_mutex_);
        for (auto& [device_id, info] : firmware_info_by_device_id_) {
            info.latest_version.clear();
            info.update_available = false;
            if (!latest_firmware_manifest_.has_value() ||
                info.hardware.empty() ||
                info.current_version.empty()) {
                snapshot[device_id] = info;
                continue;
            }
            if (!IsFirmwareManifestCompatible(info, *latest_firmware_manifest_)) {
                LogCoordinatorLine("firmware availability VS-" + device_id +
                                   " hardware=" + info.hardware +
                                   " current=" + info.current_version +
                                   " latest=" + latest_firmware_manifest_->version +
                                   " update=false reason=hardware_mismatch manifest_hardware=" +
                                   latest_firmware_manifest_->hardware);
            } else {
                info.latest_version = latest_firmware_manifest_->version;
                info.update_available = FirmwareVersion::IsOlderThan(
                    info.current_version, latest_firmware_manifest_->version);
                if (ShouldShowFirmwareUpdatePromptAfterPairing(device_id, info)) {
                    update_prompts.emplace_back(
                        device_id,
                        info.current_version,
                        info.latest_version,
                        FirmwareVersion::IsOlderThan(
                            info.current_version,
                            AppConfig::minimum_compatible_firmware_version));
                }
                LogCoordinatorLine("firmware availability VS-" + device_id +
                                   " hardware=" + info.hardware +
                                   " current=" + info.current_version +
                                   " latest=" + info.latest_version +
                                   " update=" + (info.update_available ? "true" : "false"));
            }
            snapshot[device_id] = info;
        }
    }
    ui_->SetFirmwareInfo(snapshot);
    for (const auto& [device_id, current_version, latest_version, is_below_minimum] : update_prompts) {
        ui_->ShowFirmwareUpdatePrompt(device_id, current_version, latest_version, is_below_minimum);
    }
}

bool VoiceStickCoordinator::ShouldShowFirmwareUpdatePromptAfterPairing(const std::string& device_id,
                                                                       const DeviceFirmwareInfo& info) {
    if (!pending_firmware_update_prompt_device_ids_.contains(device_id) ||
        info.current_version.empty() || info.latest_version.empty()) {
        return false;
    }
    if (!info.update_available) {
        pending_firmware_update_prompt_device_ids_.erase(device_id);
        return false;
    }
    pending_firmware_update_prompt_device_ids_.erase(device_id);
    return true;
}

void VoiceStickCoordinator::SetFirmwareChecking(bool is_checking) {
    std::map<std::string, DeviceFirmwareInfo> snapshot;
    {
        for (const auto& device_id : paired_device_ids_) {
            auto& info = firmware_info_by_device_id_[device_id];
            info.is_checking = is_checking;
            if (is_checking) info.error_message.clear();
            snapshot[device_id] = info;
        }
    }
    ui_->SetFirmwareInfo(snapshot);
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
    SetSessionState(SessionState::kReady, reason);
    ui_->SetStatus("Ready");
    SendUiStateForActiveDevice("ready");
    if (hide_overlay) ui_->HideOverlay();
    active_device_id_.reset();
}

void VoiceStickCoordinator::EnterFinalizing(std::string_view reason) {
    SetSessionState(SessionState::kFinalizing, reason);
    ui_->SetStatus("Processing");
    SendUiStateForActiveDevice("thinking");
}

void VoiceStickCoordinator::EnterPendingConfirmation(const std::string& text, std::string_view reason) {
    pending_paste_state_ = {PendingPasteKind::kWaitingToPaste, text};
    SetSessionState(SessionState::kPendingConfirmation, reason);
    ui_->ShowFinalCountdown(text, active_device_id_, [this, text] { CommitPendingPaste(text); });
    SendUiStateForActiveDevice("pending_confirmation", text);
}

void VoiceStickCoordinator::EnterPausedConfirmation(const std::string& text, std::string_view reason) {
    pending_paste_state_ = {PendingPasteKind::kPaused, text};
    SetSessionState(SessionState::kPausedConfirmation, reason);
    ui_->ShowPausedFinal(text, active_device_id_);
    SendUiStateForActiveDevice("pending_confirmation", text);
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
        ble_->SendUiState(active_device_id_ == device_id ? "recording" : "ready", "", device_id);
        break;
    case SessionState::kFinalizing:
        ble_->SendUiState(active_device_id_ == device_id ? "thinking" : "ready", "", device_id);
        break;
    case SessionState::kPendingConfirmation:
    case SessionState::kPausedConfirmation:
        if (active_device_id_ == device_id) {
            ble_->SendUiState("pending_confirmation", pending_paste_state_.text, device_id);
        } else {
            ble_->SendUiState("ready", "", device_id);
        }
        break;
    case SessionState::kError:
        ble_->SendUiState(active_device_id_ == device_id ? "error" : "ready", "", device_id);
        break;
    case SessionState::kReady:
        ble_->SendUiState("ready", "", device_id);
        break;
    }
}

void VoiceStickCoordinator::SendUiStateForActiveDevice(const std::string& state, const std::string& text) {
    ble_->SendUiState(state, text, active_device_id_);
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
           config_.interaction_mode == InteractionMode::kClickToTalk;
}

double VoiceStickCoordinator::CurrentRecordingDurationSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - active_session_started_at_).count();
}

} // namespace voicestick
