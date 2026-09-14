#pragma once

#include "app_config.h"
#include "ble_central_bluez.h"
#include "firmware_update_dialog.h"
#include "input_injector_linux.h"
#include "onboarding_dialog.h"
#include "overlay_window.h"
#include "pair_device_dialog.h"
#include "preferences_window.h"
#include "status_item_sni.h"
#include "subtitle_window.h"
#include "voice_stick_coordinator.h"

#include <adwaita.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace voicestick {

class Gtk4App : public VoiceStickUi {
public:
    explicit Gtk4App(AdwApplication* application);
    int Run(int argc, char** argv);

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
    OverlayWindow& OverlayFor(const std::optional<std::string>& device_id);
    void ApplyOverlayStyle(const std::optional<std::string>& device_id);
    void RefreshTrayDevices();
    void ShowPair();
    void ShowPreferences();
    bool ShowOnboardingIfNeeded();
    void StartCoordinator();
    void PersistConfig();
    void RefreshTray();
    void Notify(const std::string& title, const std::string& body);

    AdwApplication* application_ = nullptr;
    AppConfig config_;
    StatusItemSni tray_;
    InputInjectorLinux input_injector_;
    std::unique_ptr<BleCentralBluez> ble_;
    BleCentralBluez* ble_ptr_ = nullptr;
    std::unique_ptr<VoiceStickCoordinator> coordinator_;
    std::unique_ptr<PairDeviceDialog> pair_dialog_;
    std::unique_ptr<PreferencesWindow> preferences_;
    std::unique_ptr<OnboardingDialog> onboarding_;
    std::unique_ptr<FirmwareUpdateDialog> firmware_dialog_;
    std::map<std::string, std::unique_ptr<OverlayWindow>> overlays_;
    SubtitleWindow subtitles_;
    std::vector<std::string> paired_device_ids_;
    std::vector<ConnectedDevice> connected_devices_;
    std::map<std::string, DeviceFirmwareInfo> firmware_info_;
    std::map<std::string, DeviceInfo> device_info_;
};

} // namespace voicestick
