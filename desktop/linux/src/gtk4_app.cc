#include "gtk4_app.h"

#include "asr_client_linux.h"
#include "log.h"
#include "translation_targets.h"
#include "ui_dispatch.h"

#include <gtk/gtk.h>

#include <algorithm>

namespace voicestick {

Gtk4App::Gtk4App(AdwApplication* application)
    : application_(application),
      config_(AppConfig::Load()),
      input_injector_(GTK_APPLICATION(application)) {
    paired_device_ids_ = config_.paired_device_ids;
}

int Gtk4App::Run(int argc, char** argv) {
    g_signal_connect(application_, "activate", reinterpret_cast<GCallback>(+[](GtkApplication*, gpointer data) {
        auto* self = static_cast<Gtk4App*>(data);
        if (!self->tray_.Start()) {
            self->Notify("VoiceStick",
                         "Install gnome-shell-extension-appindicator to show the top-bar icon.");
        }
        self->tray_.on_restore = [self] { if (self->coordinator_) self->coordinator_->RestoreLastInputConfirmation(); };
        self->tray_.on_pair = [self] { self->ShowPair(); };
        self->tray_.on_preferences = [self] { self->ShowPreferences(); };
        self->tray_.on_website = [] {
            g_app_info_launch_default_for_uri(kWebsiteUrl, nullptr, nullptr);
        };
        self->tray_.on_quit = [self] { g_application_quit(G_APPLICATION(self->application_)); };
        self->tray_.on_forget = [self](const std::string& id) {
            self->config_.RemovePairedDevice(id);
            if (self->coordinator_) self->coordinator_->RemovePairedDevice(id);
            self->RefreshTray();
        };
        self->tray_.on_output_target = [self](OutputTarget target) {
            self->config_.default_output_profile.target = target;
            self->PersistConfig();
        };
        self->tray_.on_auto_enter = [self](bool value) {
            self->config_.auto_enter = value;
            self->PersistConfig();
        };
        self->tray_.on_interaction = [self](InteractionMode mode) {
            self->config_.interaction_mode = mode;
            self->PersistConfig();
        };
        self->tray_.on_countdown = [self](int ms) {
            self->config_.confirmation_countdown_ms = ms;
            self->PersistConfig();
        };
        self->tray_.on_theme = [self](std::string id, OverlayThemeColor color) {
            self->config_.device_theme_colors[id] = color;
            self->PersistConfig();
        };
        self->tray_.on_position = [self](std::string id, OverlayPosition position) {
            self->config_.device_overlay_positions[id] = position;
            self->PersistConfig();
        };
        self->tray_.on_translation = [self](std::string id, TextTransform transform, std::string target) {
            OutputProfile profile = self->config_.OutputProfileForDevice(id);
            profile.transform = transform;
            if (!target.empty()) profile.translation_target = std::move(target);
            self->config_.device_output_profiles[id] = profile;
            self->PersistConfig();
        };
        self->tray_.on_update_firmware = [self](const std::string& id) {
            auto it = self->firmware_info_.find(id);
            if (it == self->firmware_info_.end()) return;
            self->firmware_dialog_ = std::make_unique<FirmwareUpdateDialog>(nullptr);
            self->firmware_dialog_->on_start = [self, id] {
                self->coordinator_->UpdateFirmwareFromLatest(id, [self](FirmwareUpdateProgress progress) {
                    if (self->firmware_dialog_) self->firmware_dialog_->SetProgress(progress);
                }, [self](bool ok, std::string message) {
                    if (self->firmware_dialog_) self->firmware_dialog_->Finish(ok, message);
                });
            };
            self->firmware_dialog_->on_cancel = [self] {
                if (self->coordinator_) self->coordinator_->CancelFirmwareUpdate();
            };
            self->firmware_dialog_->Present(id, it->second.current_version, it->second.latest_version);
        };
        self->StartCoordinator();
        self->RefreshTray();
        self->ShowOnboardingIfNeeded();
        // Tray-only apps have no persistent window. Hold the GApplication so GTK
        // does not quit as soon as the last transient dialog closes.
        g_application_hold(G_APPLICATION(self->application_));
    }), this);

    g_signal_connect(application_, "shutdown", reinterpret_cast<GCallback>(+[](GApplication*, gpointer data) {
        auto* self = static_cast<Gtk4App*>(data);
        if (self->coordinator_) self->coordinator_->Shutdown();
        if (self->ble_ptr_) self->ble_ptr_->Shutdown();
        self->tray_.Shutdown();
    }), this);

    return g_application_run(G_APPLICATION(application_), argc, argv);
}

void Gtk4App::StartCoordinator() {
    auto ble = std::make_unique<BleCentralBluez>(config_.paired_device_ids);
    ble_ptr_ = ble.get();
    ble_ = nullptr;
    coordinator_ = std::make_unique<VoiceStickCoordinator>(
        config_,
        std::move(ble),
        std::make_unique<AsrClientLinux>(config_),
        this,
        &input_injector_,
        [](const AppConfig& config) {
            return std::unique_ptr<AsrClient>(std::make_unique<AsrClientLinux>(config));
        });
    coordinator_->Start();
    for (const auto& entry : config_.paired_devices) {
        if (entry.bluetooth_address != 0) {
            coordinator_->ConnectPairedDevice(entry.device_id, entry.bluetooth_address,
                                              entry.address_kind, entry.name);
        }
    }
}

bool Gtk4App::ShowOnboardingIfNeeded() {
    if (!config_.paired_device_ids.empty() && !config_.ActiveApiKey().empty()) {
        return true;
    }
    onboarding_ = std::make_unique<OnboardingDialog>(nullptr, ble_ptr_, config_);
    onboarding_->on_finished = [this](AppConfig config) {
        config_ = std::move(config);
        if (coordinator_) coordinator_->UpdateConfig(config_);
        for (const auto& entry : config_.paired_devices) {
            coordinator_->ConnectPairedDevice(entry.device_id, entry.bluetooth_address,
                                              entry.address_kind, entry.name);
        }
    };
    onboarding_->on_cancelled = [this] {
        g_application_quit(G_APPLICATION(application_));
    };
    onboarding_->Present();
    return true;
}

void Gtk4App::ShowPair() {
    pair_dialog_ = std::make_unique<PairDeviceDialog>(nullptr, ble_ptr_);
    pair_dialog_->on_pair = [this](ScannedBleDevice device) {
        PairedDeviceEntry entry;
        entry.device_id = device.device_id;
        entry.bluetooth_address = device.bluetooth_address;
        entry.address_kind = device.address_kind;
        entry.name = device.name;
        config_.SavePairedDevice(entry);
        if (coordinator_) {
            coordinator_->ConfirmPairedDeviceIds(config_.paired_device_ids);
            coordinator_->ConnectPairedDevice(entry.device_id, entry.bluetooth_address,
                                              entry.address_kind, entry.name);
            coordinator_->CheckFirmwareAfterPairing(entry.device_id);
        }
    };
    pair_dialog_->Present();
}

void Gtk4App::ShowPreferences() {
    preferences_ = std::make_unique<PreferencesWindow>(nullptr, config_);
    preferences_->on_config_changed = [this](AppConfig config) {
        config_ = std::move(config);
        if (coordinator_) coordinator_->UpdateConfig(config_);
        RefreshTray();
    };
    preferences_->Present();
}

void Gtk4App::PersistConfig() {
    config_.Save();
    if (coordinator_) coordinator_->UpdateConfig(config_);
    RefreshTray();
}

void Gtk4App::RefreshTray() {
    tray_.SetInputOptions(config_.interaction_mode, config_.auto_enter,
                          config_.default_output_profile.target,
                          config_.confirmation_countdown_ms);
    RefreshTrayDevices();
}

OverlayWindow& Gtk4App::OverlayFor(const std::optional<std::string>& device_id) {
    const auto key = device_id.value_or("__default__");
    auto& overlay = overlays_[key];
    if (!overlay) overlay = std::make_unique<OverlayWindow>(GTK_APPLICATION(application_));
    ApplyOverlayStyle(device_id);
    return *overlay;
}

void Gtk4App::ApplyOverlayStyle(const std::optional<std::string>& device_id) {
    const auto key = device_id.value_or("__default__");
    auto it = overlays_.find(key);
    if (it == overlays_.end()) return;
    OverlayThemeColor color = OverlayThemeColor::kWhite;
    OverlayPosition position = OverlayPosition::kCenter;
    if (device_id) {
        if (auto color_it = config_.device_theme_colors.find(*device_id);
            color_it != config_.device_theme_colors.end()) {
            color = color_it->second;
        }
        if (auto pos_it = config_.device_overlay_positions.find(*device_id);
            pos_it != config_.device_overlay_positions.end()) {
            position = pos_it->second;
        }
    }
    it->second->SetThemeColor(color);
    it->second->SetPosition(position);
}

void Gtk4App::RefreshTrayDevices() {
    std::vector<StatusItemSni::DeviceRow> rows;
    std::vector<std::string> ids = paired_device_ids_;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const auto& id : ids) {
        StatusItemSni::DeviceRow row;
        row.id = id;
        row.name = "VS-" + id;
        for (const auto& device : connected_devices_) {
            if (device.id == id) {
                row.connected = true;
                if (!device.name.empty()) row.name = device.name;
                break;
            }
        }
        if (auto it = config_.device_theme_colors.find(id); it != config_.device_theme_colors.end()) {
            row.theme = it->second;
        }
        if (auto it = config_.device_overlay_positions.find(id); it != config_.device_overlay_positions.end()) {
            row.position = it->second;
        }
        row.output = config_.OutputProfileForDevice(id);
        if (auto it = firmware_info_.find(id); it != firmware_info_.end()) row.firmware = it->second;
        rows.push_back(row);
    }
    tray_.SetDevices(rows);
}

void Gtk4App::Notify(const std::string& title, const std::string& body) {
    auto* notification = g_notification_new(title.c_str());
    g_notification_set_body(notification, body.c_str());
    g_application_send_notification(G_APPLICATION(application_), "voicestick", notification);
    g_object_unref(notification);
}

void Gtk4App::SetStatus(const std::string& status) {
    RunOnUiThread([this, status] { tray_.SetStatus(status); });
}

void Gtk4App::SetConnectedDevices(const std::vector<ConnectedDevice>& devices) {
    RunOnUiThread([this, devices] {
        connected_devices_ = devices;
        RefreshTrayDevices();
    });
}

void Gtk4App::SetDeviceInfo(const DeviceInfo& info) {
    RunOnUiThread([this, info] {
        device_info_[info.device_id] = info;
        config_.SavePairedDeviceInfo(info.device_id, info.hardware, info.firmware_version);
    });
}

void Gtk4App::SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id) {
    RunOnUiThread([this, info_by_device_id] {
        firmware_info_ = info_by_device_id;
        RefreshTrayDevices();
    });
}

void Gtk4App::SetPairingError(const std::string& device_id, const std::string& message) {
    RunOnUiThread([this, device_id, message] {
        Notify("VoiceStick", "VS-" + device_id + ": " + message);
    });
}

void Gtk4App::ShowFirmwareUpdatePrompt(const std::string& device_id,
                                       const std::string& current_version,
                                       const std::string& latest_version,
                                       bool is_below_minimum) {
    RunOnUiThread([this, device_id, current_version, latest_version, is_below_minimum] {
        std::string body = "VS-" + device_id + " can update from " + current_version +
                           " to " + latest_version + ".";
        if (is_below_minimum) body += " This version is below the minimum supported firmware.";
        Notify("Firmware update", body);
    });
}

void Gtk4App::SetPairedDeviceIds(const std::vector<std::string>& ids) {
    RunOnUiThread([this, ids] {
        paired_device_ids_ = ids;
        RefreshTrayDevices();
    });
}

void Gtk4App::SetHasRecoverableInput(bool has_recoverable_input) {
    RunOnUiThread([this, has_recoverable_input] { tray_.SetHasRecoverableInput(has_recoverable_input); });
}

void Gtk4App::ShowListening(const std::optional<std::string>& device_id) {
    RunOnUiThread([this, device_id] { OverlayFor(device_id).ShowListening(); });
}

void Gtk4App::ShowPartial(const std::string& text, const std::optional<std::string>& device_id) {
    RunOnUiThread([this, text, device_id] { OverlayFor(device_id).ShowPartial(text); });
}

void Gtk4App::ShowFinalCountdown(const std::string& text,
                                 const std::optional<std::string>& device_id,
                                 std::function<void()> on_complete) {
    const int duration_ms = config_.confirmation_countdown_ms;
    RunOnUiThread([this, text, device_id, duration_ms, on_complete = std::move(on_complete)]() mutable {
        OverlayFor(device_id).ShowFinalCountdown(text, duration_ms, std::move(on_complete));
    });
}

void Gtk4App::ShowPausedFinal(const std::string& text, const std::optional<std::string>& device_id) {
    RunOnUiThread([this, text, device_id] { OverlayFor(device_id).ShowPausedFinal(text); });
}

void Gtk4App::ShowError(const std::string& text,
                        const std::optional<std::string>& device_id,
                        std::function<void()> on_complete) {
    RunOnUiThread([this, text, device_id, on_complete = std::move(on_complete)]() mutable {
        OverlayFor(device_id).ShowError(text, std::move(on_complete));
    });
}

void Gtk4App::ShowCloudUpgrade(const std::string& message,
                               const std::string& url,
                               const std::optional<std::string>& device_id) {
    RunOnUiThread([this, message, url, device_id] {
        OverlayFor(device_id).ShowMessage(message);
        if (!url.empty()) g_app_info_launch_default_for_uri(url.c_str(), nullptr, nullptr);
    });
}

void Gtk4App::HideOverlay(std::function<void()> on_hidden) {
    RunOnUiThread([this, on_hidden = std::move(on_hidden)]() mutable {
        for (auto& [_, overlay] : overlays_) overlay->Hide();
        if (on_hidden) on_hidden();
    });
}

void Gtk4App::ShowSubtitle(const std::string& text,
                           const std::string& device_id,
                           OverlayThemeColor color) {
    RunOnUiThread([this, text, device_id, color] { subtitles_.ShowLine(text, device_id, color); });
}

void Gtk4App::HideSubtitles() {
    RunOnUiThread([this] { subtitles_.HideAll(); });
}

} // namespace voicestick
