#pragma once

#include "app_config.h"
#include "firmware_update_dialog.h"
#include "input_injector_win.h"
#include "overlay_window.h"
#include "pair_device_dialog.h"
#include "settings_dialog.h"
#include "subtitle_window.h"
#include "voice_stick_coordinator.h"

#include <Windows.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace voicestick {

class Win32App : public VoiceStickUi {
public:
    explicit Win32App(HINSTANCE instance);
    int Run();

    void SetStatus(const std::string& status) override;
    void SetConnectedDevices(const std::vector<ConnectedDevice>& devices) override;
    void SetDeviceInfo(const DeviceInfo& info) override;
    void SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id) override;
    void SetPairingError(const std::string& device_id, const std::string& message) override;
    void ShowFirmwareUpdatePrompt(const std::string& device_id,
                                  const std::string& current_version,
                                  const std::string& latest_version,
                                  bool is_below_minimum) override;
    void SetPairedDeviceIds(const std::vector<std::string>& ids) override;
    void SetHasRecoverableInput(bool has_recoverable_input) override;
    void ShowListening(const std::optional<std::string>& device_id) override;
    void ShowPartial(const std::string& text, const std::optional<std::string>& device_id) override;
    void ShowFinalCountdown(const std::string& text,
                            const std::optional<std::string>& device_id,
                            std::function<void()> on_complete) override;
    void ShowPausedFinal(const std::string& text, const std::optional<std::string>& device_id) override;
    void ShowError(const std::string& text,
                   const std::optional<std::string>& device_id,
                   std::function<void()> on_complete) override;
    void ShowCloudUpgrade(const std::string& message,
                          const std::string& url,
                          const std::optional<std::string>& device_id) override;
    void HideOverlay(std::function<void()> on_hidden = {}) override;
    void ShowSubtitle(const std::string& text,
                      const std::string& device_id,
                      OverlayThemeColor color) override;
    void HideSubtitles() override;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    bool CreateWindowInternal();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void RebuildTooltip();
    void RegisterTaskbarMessage();
    void ShowPairDeviceDialog();
    void ShowSettings();
    void SaveInputOptions();
    void SaveDeviceThemeColor(const std::string& device_id, OverlayThemeColor color);
    void SaveDeviceOverlayPosition(const std::string& device_id, OverlayPosition position);
    void SaveDeviceOutputProfile(const std::string& device_id, OutputProfile profile);
    void ApplyOverlayStyle(const std::optional<std::string>& device_id);
    void StartFirmwareUpdate(const std::string& device_id);
    void PairDevice(const std::string& device_id, std::uint64_t bluetooth_address,
                    BluetoothAddressKind address_kind, const std::string& name);
    void HandlePairingCompleted(const std::string& device_id, std::optional<DeviceInfo> info);
    void ShowNotification(const std::string& title, const std::string& body);
    std::wstring Utf16(const std::string& text) const;
    void DispatchToUi(std::function<void()> action);
    void ShutdownAndQuit();

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    DWORD ui_thread_id_ = 0;
    UINT taskbar_created_message_ = 0;
    AppConfig config_;
    InputInjectorWin input_injector_;
    std::unique_ptr<VoiceStickCoordinator> coordinator_;
    std::unique_ptr<PairDeviceDialog> pair_device_dialog_;
    std::unique_ptr<SettingsDialog> settings_dialog_;
    std::unique_ptr<FirmwareUpdateDialog> firmware_update_dialog_;
    std::unique_ptr<OverlayWindow> overlay_;
    std::unique_ptr<SubtitleWindow> subtitles_;
    class BleCentralWin* ble_central_ = nullptr;
    std::string status_ = "Ready";
    std::vector<ConnectedDevice> connected_devices_;
    std::vector<std::string> paired_device_ids_;
    std::map<std::string, DeviceInfo> device_info_map_;
    std::map<std::string, DeviceFirmwareInfo> firmware_info_map_;
    std::optional<PairedDeviceEntry> pending_pairing_entry_;
    bool has_recoverable_input_ = false;
    bool is_shutting_down_ = false;
};

} // namespace voicestick
