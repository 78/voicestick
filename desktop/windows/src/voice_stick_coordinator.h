#pragma once

#include "app_config.h"
#include "asr_protocol.h"
#include "ble_protocol.h"
#include "debug_audio_recorder.h"
#include "firmware_manifest.h"
#include "llm_translation_client.h"
#include "ogg_opus_muxer.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace voicestick {

struct ConnectedDevice {
    std::string id;
    std::string name;
};

struct DeviceInfo {
    std::string device_id;
    std::string hardware;
    std::string firmware_version;
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
    virtual void SendUiState(const std::string& state,
                               const std::string& text,
                               const std::optional<std::string>& device_id) = 0;
    virtual void SendInteractionMode(InteractionMode mode,
                                     const std::optional<std::string>& device_id) = 0;
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
    std::function<void(std::string, StateEvent)> on_state_event;
    std::function<void(std::string, AudioFrame)> on_audio_frame;
};

class AsrClient {
public:
    virtual ~AsrClient() = default;
    virtual bool Start(AsrSessionOptions options = {}) = 0;
    virtual void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) = 0;
    virtual void Cancel() = 0;

    std::function<void(std::string)> on_partial;
    std::function<void(AsrSegment)> on_segment;
    std::function<void(std::string)> on_final;
    std::function<void(std::string)> on_error;
    std::function<void(std::string, std::string)> on_upgrade_url;
};

class VoiceStickUi {
public:
    virtual ~VoiceStickUi() = default;
    virtual void SetStatus(const std::string& status) = 0;
    virtual void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) = 0;
    virtual void SetDeviceInfo(const DeviceInfo& info) = 0;
    virtual void SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id) = 0;
    virtual void SetPairingError(const std::string& device_id, const std::string& message) = 0;
    virtual void ShowFirmwareUpdatePrompt(const std::string& device_id,
                                          const std::string& current_version,
                                          const std::string& latest_version,
                                          bool is_below_minimum) = 0;
    virtual void SetPairedDeviceIds(const std::vector<std::string>& ids) = 0;
    virtual void SetHasRecoverableInput(bool has_recoverable_input) = 0;
    virtual void ShowListening(const std::optional<std::string>& device_id) = 0;
    virtual void ShowPartial(const std::string& text, const std::optional<std::string>& device_id) = 0;
    virtual void ShowFinalCountdown(const std::string& text,
                                    const std::optional<std::string>& device_id,
                                    std::function<void()> on_complete) = 0;
    virtual void ShowPausedFinal(const std::string& text, const std::optional<std::string>& device_id) = 0;
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
    virtual void Paste(const std::string& text, bool press_enter) = 0;
};

class VoiceStickCoordinator {
public:
    VoiceStickCoordinator(AppConfig config,
                          std::unique_ptr<BleCentral> ble,
                          std::unique_ptr<AsrClient> asr,
                          VoiceStickUi* ui,
                          InputInjector* input_injector,
                          std::function<std::unique_ptr<AsrClient>(const AppConfig&)> asr_factory = {});
    ~VoiceStickCoordinator();

    void Start();
    void Shutdown();
    void UpdateConfig(AppConfig config);
    void ReconnectPairedDevices();
    void ConnectPairedDevice(const std::string& device_id,
                             std::uint64_t bluetooth_address,
                             BluetoothAddressKind address_kind,
                             const std::string& name);
    void ConfirmPairedDeviceIds(const std::vector<std::string>& device_ids);
    void RemovePairedDevice(const std::string& device_id);
    void CancelPendingConnect(const std::string& device_id);
    bool RestoreLastInputConfirmation();
    void CheckFirmwareUpdatesNow();
    void CheckFirmwareAfterPairing(const std::string& device_id);
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
    void HandleStateEvent(const StateEvent& event, const std::string& device_id);
    void HandleButtonDown(const StateEvent& event, const std::string& device_id);
    void HandleButtonUp(const StateEvent& event, const std::string& device_id);
    void HandleButtonClick(const StateEvent& event, const std::string& device_id);
    void HandleSecondaryButtonClick(const std::string& device_id);
    void HandlePrimaryButtonDown(std::optional<std::uint32_t> session_id, const std::string& device_id);
    void HandlePrimaryButtonUp(const std::string& device_id);
    void HandleAudioFrame(const AudioFrame& frame, const std::string& device_id);
    void HandleSubtitlePrimaryButtonDown(std::optional<std::uint32_t> session_id, const std::string& device_id);
    void HandleSubtitlePrimaryButtonUp(const std::string& device_id);
    void HandleSubtitleAudioFrame(const AudioFrame& frame, const std::string& device_id);
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
                                          const std::string& text);
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
                          std::function<void(bool)> completion = {});
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
    void FinishWithFinalText(const std::string& text);
    void FinishWithAsrError(const std::string& message);
    void RecoverFromAsrError(bool hide_overlay = true);
    void CommitPendingPaste(const std::string& text);
    void CompletePendingPaste(const std::string& text);
    bool RestoreLastInputConfirmation(std::optional<std::string> device_id);
    bool HandleFrontButtonDuringPendingPaste(const std::string& device_id);
    void CancelPendingPaste(const std::string& device_id);
    void CancelRecognitionInProgress();
    void CancelActiveCycleIfDeviceDisconnected();
    void FinishRecognitionCycle();
    void UpdateDeviceFirmwareInfo(const StateEvent& event, const std::string& device_id);
    void CheckFirmwareUpdatesIfNeeded(bool force, bool show_errors);
    void RefreshFirmwareAvailability();
    void SetFirmwareChecking(bool is_checking);
    bool ShouldShowFirmwareUpdatePromptAfterPairing(const std::string& device_id,
                                                    const DeviceFirmwareInfo& info);
    bool IsWaitingForFinalText() const;
    void SetSessionState(SessionState state, std::string_view reason);
    void EnterReady(std::string_view reason, bool hide_overlay = true);
    void EnterFinalizing(std::string_view reason);
    void EnterPendingConfirmation(const std::string& text, std::string_view reason);
    void EnterPausedConfirmation(const std::string& text, std::string_view reason);
    void EnterError(const std::string& message, std::string_view reason);
    void RefreshDeviceUiState(const std::string& device_id);
    void SendUiStateForActiveDevice(const std::string& state, const std::string& text = "");
    OutputProfile OutputProfileForDevice(const std::optional<std::string>& device_id) const;
    OverlayThemeColor ThemeColorForDevice(const std::string& device_id) const;
    bool ShouldUseDefiniteSegments(const OutputProfile& profile) const;
    double CurrentRecordingDurationSeconds() const;

    AppConfig config_;
    std::unique_ptr<BleCentral> ble_;
    std::unique_ptr<AsrClient> asr_;
    std::function<std::unique_ptr<AsrClient>(const AppConfig&)> asr_factory_;
    LLMTranslationClient translator_;
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
    std::optional<std::string> last_recoverable_text_;
    std::optional<std::string> last_recoverable_device_id_;
    std::vector<std::string> paired_device_ids_;
    std::map<std::string, DeviceFirmwareInfo> firmware_info_by_device_id_;
    std::optional<FirmwareManifest> latest_firmware_manifest_;
    std::chrono::steady_clock::time_point last_firmware_manifest_check_at_{};
    bool has_last_firmware_manifest_check_at_ = false;
    bool firmware_manifest_check_in_flight_ = false;
    std::set<std::string> pending_firmware_update_prompt_device_ids_;
    FirmwareManifestClient firmware_manifest_client_;
    std::mutex firmware_mutex_;
    std::shared_ptr<std::atomic_bool> alive_{std::make_shared<std::atomic_bool>(true)};
    std::thread firmware_manifest_thread_;
    bool is_showing_asr_error_ = false;
    bool is_shutdown_ = false;
    std::map<std::pair<std::string, std::uint32_t>, std::unique_ptr<SubtitleCycle>> subtitle_cycles_;
    std::map<std::string, std::uint32_t> active_subtitle_sessions_;
    static constexpr double kMinimumRecordingDurationSeconds = 0.5;
    static constexpr std::chrono::milliseconds kAudioEndTimeout{1000};
    static constexpr std::chrono::hours kFirmwareManifestCacheDuration{24};
};

} // namespace voicestick
