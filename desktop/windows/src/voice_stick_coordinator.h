#pragma once

#include "app_config.h"
#include "asr_protocol.h"
#include "ble_protocol.h"
#include "debug_audio_recorder.h"
#include "firmware_manifest.h"
#include "llm_translation_client.h"
#include "ogg_opus_muxer.h"
#include "../../shared/shake_detector.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace voicestick {

struct ConnectedDevice {
    std::string id;
    std::string name;
    bool supports_firmware_update = false;
};

struct DeviceInfo {
    std::string device_id;
    std::string hardware;
    std::string firmware_version;
    std::optional<std::vector<std::string>> buttons;
};

struct FirmwareUpdateProgress {
    int written_bytes = 0;
    int total_bytes = 0;
    bool is_device_confirmed = false;
};

class BleCentral {
public:
    virtual ~BleCentral() = default;
    virtual void Start() = 0;
    virtual void UpdatePairedDeviceIds(const std::vector<std::string>& ids) = 0;
    virtual void ConnectPairedDevice(const std::string& device_id,
                                     std::uint64_t bluetooth_address,
                                     BluetoothAddressKind address_kind,
                                     const std::string& name) = 0;
    // Connects while the device is in its pairing window and may replace
    // this computer's Bluetooth bond.
    virtual void PairDevice(const std::string& device_id,
                            std::uint64_t bluetooth_address,
                            BluetoothAddressKind address_kind,
                            const std::string& name) {
        ConnectPairedDevice(device_id, bluetooth_address, address_kind, name);
    }
    virtual void SetDisplay(const std::string& scene,
                            const std::string& text,
                            const std::optional<std::string>& device_id) = 0;
    virtual void StartAudioIn(const std::string& device_id, InteractionMode mode) = 0;
    virtual void StopAudioIn(const std::string& device_id) = 0;
    virtual void StartImu(const std::string& device_id,
                          const std::string& axes,
                          int rate_hz,
                          int lease_ms) = 0;
    virtual void RenewImu(const std::string& device_id, int lease_ms) = 0;
    virtual void StopImu(const std::string& device_id) = 0;
    virtual std::vector<std::string> ConnectedReadyDeviceIds() const = 0;
    virtual void UpdateFirmware(ByteVector image,
                                const std::string& device_id,
                                std::function<void(FirmwareUpdateProgress)> progress,
                                std::function<void(bool, std::string)> completion) = 0;
    virtual void CancelFirmwareUpdate() = 0;
    virtual bool IsConnected(const std::string& device_id) const = 0;
    virtual void CancelPendingConnect(const std::string& device_id) {}
    virtual void Shutdown() {}

    std::function<void(std::vector<ConnectedDevice>)> on_connection_change;
    std::function<void(std::string, std::string)> on_connection_error;
    std::function<void(std::string)> on_scan_error;
    std::function<void(std::string, DeviceInfoResult)> on_device_info;
    std::function<void(std::string, DeviceInfoResult)> on_device_ready;
    std::function<void(std::string, LegacyDeviceInfo)> on_legacy_device_info;
    std::function<void(std::string, DeviceBatteryStatus)> on_battery_changed;
    std::function<void(std::string, std::string, bool, std::uint32_t)> on_button_event;
    std::function<void(std::string)> on_input_gap;
    std::function<void(std::string, AudioStreamFrame)> on_audio_frame;
    std::function<void(std::string, ImuStreamFrame)> on_imu_frame;
    std::function<void(std::string, std::uint16_t, std::string)> on_stream_closed;
};

class AsrClient {
public:
    virtual ~AsrClient() = default;
    virtual bool Start(AsrSessionOptions options = {}) = 0;
    virtual void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) = 0;
    virtual void Cancel() = 0;
    virtual std::string LastStartError() const { return {}; }

    std::function<void(std::string)> on_partial;
    std::function<void(AsrSegment)> on_segment;
    std::function<void(AsrFinalResult)> on_final;
    std::function<void(std::string message, std::string partial_text)> on_error;
    std::function<void(std::string, std::string)> on_upgrade_url;
};

class VoiceStickUi {
public:
    virtual ~VoiceStickUi() = default;
    virtual void SetStatus(const std::string& status) = 0;
    virtual void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) = 0;
    virtual void SetDeviceInfo(const DeviceInfo& info) = 0;
    virtual void SetDeviceBatteryStatus(const std::string& device_id, const DeviceBatteryStatus& status) = 0;
    virtual void SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id,
                                 std::uint64_t revision) = 0;
    virtual void SetPairingError(const std::string& device_id, const std::string& message) = 0;
    virtual void SetPairedDeviceIds(const std::vector<std::string>& ids) = 0;
    virtual void SetHasRecoverableInput(bool has_recoverable_input) = 0;
    virtual void ShowListening(const OutputProfile& profile, const std::optional<std::string>& device_id) = 0;
    virtual void ShowPartial(const std::string& text, const std::optional<std::string>& device_id) = 0;
    virtual void ShowFinalCountdown(const std::string& text,
                                    const std::optional<std::string>& device_id,
                                    int duration_ms, std::function<void()> on_complete) = 0;
    virtual void ShowPausedFinal(const std::string& text, const std::optional<std::string>& device_id) = 0;
    virtual std::optional<std::string> EditedConfirmationText() const = 0;
    virtual void ConcealConfirmation() = 0;
    virtual void ShowPasteFailure() = 0;
    virtual void ShowError(const std::string& text,
                           const std::optional<std::string>& device_id,
                           std::function<void()> on_complete) = 0;
    virtual void ShowCloudUpgrade(const std::string& message,
                                  const std::string& url,
                                  const std::optional<std::string>& device_id) = 0;
    virtual void HideOverlay(std::function<void()> on_hidden = {}) = 0;
    virtual void ShowSubtitle(const std::string& text,
                              const std::string& device_id,
                              OverlayThemeColor color) = 0;
    virtual void HideSubtitles() = 0;
};

class InputInjector {
public:
    virtual ~InputInjector() = default;
    virtual bool Paste(const std::string& text, bool press_enter) = 0;
    virtual bool PasteConfirmation(const std::string& text, bool press_enter, bool use_current_field,
                                   const std::function<void()>& conceal) = 0;
    virtual void CaptureConfirmationTarget() {}
    virtual void RememberTarget() {}
    virtual void RestoreTarget() {}
};

class VoiceStickCoordinator {
public:
    VoiceStickCoordinator(AppConfig config,
                          std::shared_ptr<BleCentral> ble,
                          std::unique_ptr<AsrClient> asr,
                          VoiceStickUi* ui,
                          InputInjector* input_injector,
                          std::function<std::unique_ptr<AsrClient>(const AppConfig&)> asr_factory = {},
                          std::function<void(std::function<void()>)> post_to_ui = {});
    ~VoiceStickCoordinator();

    void Start();
    void Shutdown();
    void UpdateConfig(AppConfig config);
    void SetSendConfirmationDuration(int duration_ms) { config_.send_confirmation_ms = duration_ms; }
    void ReconnectPairedDevices();
    void ConnectPairedDevice(const std::string& device_id,
                             std::uint64_t bluetooth_address,
                             BluetoothAddressKind address_kind,
                             const std::string& name);
    void PairDevice(const std::string& device_id,
                    std::uint64_t bluetooth_address,
                    BluetoothAddressKind address_kind,
                    const std::string& name);
    void ConfirmPairedDeviceIds(const std::vector<std::string>& device_ids, const std::string& device_id);
    void RemovePairedDevice(const std::string& device_id);
    void CancelPendingConnect(const std::string& device_id);
    bool RestoreLastInputConfirmation();
    void ConfirmPendingPaste();
    void DismissPendingConfirmation();
    void CheckFirmwareUpdates();
    void CheckDeviceFirmwareUpdates(const std::string& device_id, std::function<void(DeviceFirmwareInfo)> completion);
    bool IsIdleForFirmwareReminder() const;
    void UpdateFirmwareFromLatest(const std::string& device_id,
                                  std::function<void(FirmwareUpdateProgress)> progress,
                                  std::function<void(bool, std::string)> completion);
    void CancelFirmwareUpdate();

private:
    enum class PendingPasteKind {
        kIdle,
        kWaitingToPaste,
        kPaused,
    };

    enum class SessionState {
        kReady,
        kRecording,
        kFinalizing,
        kPendingConfirmation,
        kPausedConfirmation,
        kError,
    };

    struct PendingPasteState {
        PendingPasteKind kind = PendingPasteKind::kIdle;
        std::string text;
        std::string confirmation_id;
        bool use_current_field = false;

        bool IsIdle() const { return kind == PendingPasteKind::kIdle; }
    };

    struct SubtitleCycle {
        std::string device_id;
        std::uint32_t session_id = 0;
        std::chrono::steady_clock::time_point started_at;
        std::unique_ptr<AsrClient> asr;
        OggOpusMuxer ogg_muxer{16000, 1};
        DebugAudioRecorder debug_audio_recorder{false, {}};
        int received_audio_frames = 0;
        std::optional<std::uint32_t> last_audio_seq;
        std::vector<ByteVector> buffered_ogg_chunks;
        bool asr_started = false;
        bool sent_final_audio_chunk = false;
        bool finished_final_text = false;
        bool waiting_for_audio_end = false;
        std::uint64_t audio_end_wait_generation = 0;
    };

    void ConfigureAsrCallbacks();
    void ConfigureSubtitleAsrCallbacks(SubtitleCycle* cycle);
    void HandleDeviceInfo(const DeviceInfoResult& info, const std::string& device_id);
    void HandleLegacyDeviceInfo(const LegacyDeviceInfo& info, const std::string& device_id);
    void HandleDeviceReady(const DeviceInfoResult& info, const std::string& device_id);
    void RestoreVoiceMode(const std::string& device_id);
    bool CanUseDeviceInput(const std::string& device_id);
    void HandleButtonEvent(const std::string& device_id, const std::string& button,
                           bool is_down, std::uint32_t timestamp_ms);
    void HandleSecondaryButtonClick(const std::string& device_id);
    void HandlePrimaryButtonDown(std::optional<std::uint32_t> session_id, const std::string& device_id);
    void HandlePrimaryButtonUp(std::uint32_t session_id, const std::string& device_id);
    void HandleAudioFrame(const AudioStreamFrame& frame, const std::string& device_id);
    void HandleImuFrame(const ImuStreamFrame& frame, const std::string& device_id);
    void HandleStreamClosed(const std::string& device_id, std::uint16_t stream_id, const std::string& reason);
    void StartShakeConfirmation(const std::string& device_id);
    void StopShakeConfirmation();
    void HandleSubtitlePrimaryButtonDown(std::optional<std::uint32_t> session_id, const std::string& device_id);
    void HandleSubtitlePrimaryButtonUp(const std::string& device_id);
    void HandleSubtitleAudioFrame(const AudioStreamFrame& frame, std::uint32_t utterance_id, const std::string& device_id);
    void BeginWaitingForSubtitleAudioEnd(SubtitleCycle* cycle, std::string_view reason);
    void ScheduleSubtitleAudioEndTimeout(const std::string& device_id, std::uint32_t session_id);
    void CancelSubtitleAudioEndTimeout(SubtitleCycle* cycle);
    void FinishSubtitleAudioInput(SubtitleCycle* cycle);
    void SendSubtitleFinalOggChunkIfNeeded(const std::string& device_id, std::uint32_t session_id);
    void SendOrBufferSubtitleOggChunk(SubtitleCycle* cycle, const ByteVector& chunk,
                                      bool is_last, bool can_start_asr);
    bool StartSubtitleAsrAndFlushBufferedChunks(SubtitleCycle* cycle, bool last_chunk_is_final);
    void HandleDefiniteSegment(const AsrSegment& segment);
    void HandleSubtitleDefiniteSegment(const AsrSegment& segment, const std::string& device_id);
    void FinishSubtitleCycleWithFinalText(const std::string& device_id, std::uint32_t session_id,
                                          const AsrFinalResult& result);
    void FinishSubtitleCycleWithError(const std::string& device_id, std::uint32_t session_id,
                                      const std::string& message);
    void CancelSubtitleCycle(const std::string& device_id, std::string_view reason);
    void FinishSubtitleCycle(const std::string& device_id, std::uint32_t session_id, bool hide_overlay);
    bool ShouldHideOverlayForFinishedSubtitleCycle(const std::string& device_id, std::uint32_t session_id) const;
    bool CanUpdateOverlayForSubtitleCycle(const std::string& device_id, std::uint32_t session_id) const;
    bool ShouldSendPartialToDevice() const;
    bool ShouldSendSubtitlePartialToDevice(const SubtitleCycle* cycle) const;
    SubtitleCycle* FindSubtitleCycle(const std::string& device_id, std::uint32_t session_id);
    SubtitleCycle* FindActiveSubtitleCycle(const std::string& device_id);
    bool IsActiveSubtitleCycle(const std::string& device_id, std::uint32_t session_id) const;
    bool HasActiveSubtitleSession(const std::string& device_id) const;
    void ClearActiveSubtitleSession(const std::string& device_id, std::uint32_t session_id);
    void CancelSubtitleCyclesForDevice(const std::string& device_id, std::string_view reason);
    void ShowSubtitleText(const std::string& text, const OutputProfile& profile, const std::string& device_id,
                          std::function<void(bool)> completion = {},
                          std::optional<std::uint32_t> session_id = std::nullopt);
    void TransformText(const std::string& text, const OutputProfile& profile,
                       std::function<void(bool, std::string)> completion);
    void BeginWaitingForAudioEnd(std::string_view reason);
    void ScheduleAudioEndTimeout(std::optional<std::uint32_t> session_id,
                                 std::optional<std::string> device_id);
    void CancelAudioEndTimeout();
    void SendFinalOggChunkIfNeeded(double recording_duration_seconds);
    void SendOrBufferOggChunk(const ByteVector& chunk, bool is_last, bool can_start_asr);
    bool StartAsrAndFlushBufferedChunks(bool last_chunk_is_final);
    void CancelShortRecording();
    void FinishWithFinalText(const AsrFinalResult& result);
    void FinishWithAsrError(const std::string& message, const std::string& partial_text = {});
    void RecoverFromAsrError(bool hide_overlay = true);
    void CompletePendingPaste(const std::string& text);
    void FinishPendingPaste(const std::string& text);
    void ShowConfirmationPasteFailure();
    void RememberInput(const std::string& text);
    bool RestoreLastInputConfirmation(std::optional<std::string> device_id);
    bool HandleFrontButtonDuringPendingPaste(const std::string& device_id);
    void CancelPendingPaste(const std::string& device_id);
    void CancelRecognitionInProgress();
    void CancelActiveCycleIfDeviceDisconnected();
    void FinishRecognitionCycle();
    using FirmwareFeedCompletion = std::function<void(const FirmwareManifest*, std::string_view)>;
    void CheckFirmwareFeed(std::string_view hardware, bool force, FirmwareFeedCompletion completion = {});
    void RefreshFirmwareAvailability();
    bool IsWaitingForFinalText() const;
    void SetSessionState(SessionState state, std::string_view reason);
    void EnterReady(std::string_view reason, bool hide_overlay = true);
    void EnterFinalizing(std::string_view reason);
    void EnterPendingConfirmation(const std::string& text, std::string_view reason);
    void EnterPausedConfirmation(const std::string& text, std::string_view reason);
    void EnterError(const std::string& message, std::string_view reason);
    void RefreshDeviceUiState(const std::string& device_id);
    void SendDisplayState(const std::string& state, const std::string& text,
                          const std::optional<std::string>& device_id);
    void SendUiStateForActiveDevice(const std::string& state, const std::string& text = "");
    OutputProfile OutputProfileForDevice(const std::optional<std::string>& device_id) const;
    OverlayThemeColor ThemeColorForDevice(const std::string& device_id) const;
    bool ShouldUseDefiniteSegments(const OutputProfile& profile) const;
    AsrSessionOptions SessionOptions(const OutputProfile& profile) const;
    double CurrentRecordingDurationSeconds() const;

    AppConfig config_;
    std::shared_ptr<BleCentral> ble_;
    std::unique_ptr<AsrClient> asr_;
    std::function<std::unique_ptr<AsrClient>(const AppConfig&)> asr_factory_;
    LLMTranslationClient translator_;
    std::function<void(std::function<void()>)> post_to_ui_;
    VoiceStickUi* ui_;
    InputInjector* input_injector_;
    std::mutex audio_mutex_;
    OggOpusMuxer ogg_muxer_{16000, 1};
    DebugAudioRecorder debug_audio_recorder_;
    SessionState session_state_ = SessionState::kReady;
    std::optional<std::uint32_t> active_session_id_;
    std::optional<std::string> active_device_id_;
    std::chrono::steady_clock::time_point active_session_started_at_;
    int received_audio_frames_ = 0;
    std::optional<std::uint32_t> last_audio_seq_;
    std::vector<ByteVector> buffered_ogg_chunks_;
    bool asr_started_ = false;
    bool sent_final_audio_chunk_ = false;
    bool pasted_final_text_ = false;
    std::atomic_bool waiting_for_audio_end_{false};
    std::atomic_uint64_t audio_end_wait_generation_{0};
    PendingPasteState pending_paste_state_;
    bool sending_confirmation_ = false;
    std::uint64_t confirmation_generation_ = 0;
    std::uint64_t text_transform_generation_ = 0;
    std::optional<std::string> last_recoverable_text_;
    std::optional<std::string> last_recoverable_device_id_;
    std::vector<std::string> paired_device_ids_;
    std::map<std::string, std::uint64_t> secondary_down_ms_;
    std::map<std::string, std::uint32_t> active_capture_utterance_;
    std::uint32_t next_utterance_id_ = 1;
    std::map<std::string, DeviceImuCapabilities> device_imu_caps_;
    std::optional<std::string> shake_device_id_;
    ShakeDetector shake_detector_;
    std::atomic_uint64_t shake_renew_generation_{0};
    std::map<std::string, DeviceFirmwareInfo> firmware_info_by_device_id_;
    struct FirmwareFeed {
        std::optional<FirmwareManifest> manifest;
        std::chrono::steady_clock::time_point next_check{};
        bool is_checking = false;
        std::string error_message;
        std::vector<FirmwareFeedCompletion> completions;
        std::thread worker;
    };
    std::map<std::string_view, FirmwareFeed> firmware_feeds_;
    std::uint64_t firmware_info_revision_ = 0;
    std::mutex firmware_mutex_;
    std::shared_ptr<std::atomic_bool> alive_{std::make_shared<std::atomic_bool>(true)};
    std::thread firmware_download_thread_;
    std::mutex firmware_transfer_mutex_;
    bool firmware_download_in_flight_ = false;
    bool firmware_download_cancelled_ = false;
    bool is_showing_asr_error_ = false;
    bool is_shutdown_ = false;
    std::map<std::pair<std::string, std::uint32_t>, std::unique_ptr<SubtitleCycle>> subtitle_cycles_;
    std::map<std::string, std::uint32_t> active_subtitle_sessions_;
    static constexpr double kMinimumRecordingDurationSeconds = 0.5;
    static constexpr std::chrono::milliseconds kAudioEndTimeout{1000};
    static constexpr std::chrono::hours kFirmwareManifestCacheDuration{24};
    static constexpr std::chrono::minutes kFirmwareManifestRetryDelay{5};
};

} // namespace voicestick
