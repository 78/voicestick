#include "win32_app.h"

#include "asr_client_win.h"
#include "ble_central_win.h"
#include "log.h"
#include "resource.h"
#include "version.h"

#include <Shellapi.h>
#include <winsparkle.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>

namespace voicestick {

namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kUiDispatchMessage = WM_APP + 2;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kFirmwareReminderTimer = 1;
constexpr UINT_PTR kFirmwareCheckTimer = 2;
constexpr UINT kMenuRestore = 1001;
constexpr UINT kMenuSettings = 1002;
constexpr UINT kMenuQuit = 1005;
constexpr UINT kMenuPairScan = 1006;
constexpr UINT kMenuCheckAppUpdates = 1008;
constexpr UINT kMenuHoldToTalk = 1009;
constexpr UINT kMenuClickToTalk = 1010;
constexpr UINT kMenuAutoEnter = 1011;
constexpr UINT kMenuOutputFocusedApp = 1012;
constexpr UINT kMenuOutputSubtitle = 1013;
constexpr UINT kMenuConfirmationBase = 1014;
constexpr UINT kMenuForgetBase = 2100;
constexpr UINT kMenuForgetEnd = 2199;
constexpr UINT kMenuUpdateFirmwareBase = 2200;
constexpr UINT kMenuUpdateFirmwareEnd = 2299;
constexpr UINT kMenuThemeColorBase = 2300;
constexpr UINT kMenuThemeColorEnd = 2899;
constexpr UINT kMenuOverlayPositionBase = 2900;
constexpr UINT kMenuOverlayPositionEnd = 3399;
constexpr UINT kMenuTranslationBase = 3400;
constexpr UINT kMenuTranslationEnd = 5799;
constexpr UINT kMenuCheckFirmwareBase = 5800;
constexpr UINT kMenuCheckFirmwareEnd = 5899;
constexpr UINT kMenuOptionsPerDevice = 6;
constexpr UINT kMenuTranslationsPerDevice = 24;

#ifndef VOICESTICK_APPCAST_URL
#define VOICESTICK_APPCAST_URL "https://78.github.io/voicestick/appcast.xml"
#endif

std::atomic<HWND> updater_window{nullptr};

void RequestShutdownForAppUpdate() {
    if (const HWND hwnd = updater_window.load()) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}

void LogLine(std::string_view message) {
    voicestick::LogApp(message);
}

std::wstring BatteryStatusText(const DeviceBatteryStatus& status) {
    auto text = std::to_wstring(status.percent) + L"%";
    if (status.charging) {
        text += L" (Charging)";
    } else if (status.usb_powered) {
        text += L" (USB power)";
    }
    return text;
}

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::wstring FirmwareIdentityText(const std::string& hardware, const std::string& version) {
    if (!hardware.empty() && !version.empty()) {
        return Utf16FromUtf8(hardware + " " + version);
    }
    if (!hardware.empty()) {
        return Utf16FromUtf8(hardware);
    }
    if (!version.empty()) {
        return L"Firmware " + Utf16FromUtf8(version);
    }
    return L"Firmware Unknown";
}

constexpr OverlayThemeColor kOverlayThemeColors[] = {
    OverlayThemeColor::kWhite,
    OverlayThemeColor::kPink,
    OverlayThemeColor::kGreen,
    OverlayThemeColor::kYellow,
    OverlayThemeColor::kBlue,
    OverlayThemeColor::kPurple,
};

constexpr OverlayPosition kOverlayPositions[] = {
    OverlayPosition::kCenter,
    OverlayPosition::kTopLeft,
    OverlayPosition::kTopRight,
    OverlayPosition::kBottomLeft,
    OverlayPosition::kBottomRight,
};

struct TranslationTarget {
    const char* code;
    const wchar_t* name;
};

constexpr TranslationTarget kTranslationTargets[] = {
    {"en", L"English"},
    {"zh-Hans", L"Chinese (Simplified)"},
    {"zh-Hant", L"Chinese (Traditional)"},
    {"ja", L"Japanese"},
    {"ko", L"Korean"},
    {"ru", L"Russian"},
    {"fr", L"French"},
    {"de", L"German"},
    {"es", L"Spanish"},
    {"it", L"Italian"},
    {"pt", L"Portuguese"},
    {"nl", L"Dutch"},
    {"sv", L"Swedish"},
    {"pl", L"Polish"},
    {"tr", L"Turkish"},
    {"ar", L"Arabic"},
    {"hi", L"Hindi"},
    {"id", L"Indonesian"},
    {"vi", L"Vietnamese"},
    {"th", L"Thai"},
};

std::string OverlayOutputMode(const OutputProfile& profile) {
    if (profile.transform == TextTransform::kRefine) return TextTransformDisplayName(profile.transform);
    if (profile.transform != TextTransform::kTranslate) return {};
    const auto* target = std::ranges::find_if(kTranslationTargets, [&profile](const auto& entry) {
        return std::ranges::equal(profile.translation_target, std::string_view(entry.code),
            [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    });
    const auto name = target == std::end(kTranslationTargets)
        ? profile.translation_target : winrt::to_string(target->name);
    return "\xE2\x86\x92 " + name;
}

} // namespace

Win32App::Win32App(HINSTANCE instance) : instance_(instance), config_(AppConfig::Load()) {
    paired_device_ids_ = config_.paired_device_ids;
    for (const auto& entry : config_.paired_devices) {
        if (entry.device_id.empty()) continue;
        if (!entry.hardware.empty() || !entry.firmware_version.empty()) {
            device_info_map_[entry.device_id] = DeviceInfo{
                entry.device_id,
                entry.hardware,
                entry.firmware_version,
                std::nullopt,
            };
        }
    }
}

int Win32App::Run() {
    try {
        LogLine("VoiceStickApp starting");
        ui_thread_id_ = GetCurrentThreadId();
        LogLine("Creating main window");
        if (!CreateWindowInternal()) {
            LogLine("CreateWindowInternal failed");
            return 1;
        }
        input_injector_.SetClipboardOwner(hwnd_);
        LogLine("Main window created");

        RegisterTaskbarMessage();
        AddTrayIcon();

        LogLine("Initializing WinSparkle");
        win_sparkle_set_appcast_url(VOICESTICK_APPCAST_URL);
        win_sparkle_set_automatic_check_for_updates(1);
        win_sparkle_set_update_check_interval(86400);
        updater_window.store(hwnd_);
        win_sparkle_set_shutdown_request_callback(RequestShutdownForAppUpdate);
        win_sparkle_init();
        LogLine("WinSparkle initialized");

        LogLine("Creating BLE coordinator");
        auto ble = std::make_shared<BleCentralWin>(config_.paired_device_ids, hwnd_);
        ble_central_ = ble.get();
        const auto post_to_ui = [this](std::function<void()> action) {
            PostToUi(std::move(action));
        };
        coordinator_ = std::make_unique<VoiceStickCoordinator>(
            config_,
            std::move(ble),
            std::make_unique<AsrClientWin>(config_, post_to_ui),
            this,
            &input_injector_,
            [post_to_ui](const AppConfig& config) {
                return std::make_unique<AsrClientWin>(config, post_to_ui);
            },
            post_to_ui);
        LogLine("Starting coordinator");
        coordinator_->Start();
        LogLine("Coordinator started");

        for (const auto& entry : config_.paired_devices) {
            if (entry.bluetooth_address != 0) {
                LogLine("Queueing paired device connect VS-" + entry.device_id);
                coordinator_->ConnectPairedDevice(entry.device_id, entry.bluetooth_address,
                                                 entry.address_kind, entry.name);
            }
        }

        LogLine("Checking onboarding");
        if (!ShowOnboardingIfNeeded()) {
            LogLine("Onboarding did not complete; exiting");
            ShutdownAndQuit();
            win_sparkle_cleanup();
            RemoveTrayIcon();
            return 0;
        }
        SetTimer(hwnd_, kFirmwareReminderTimer, 1000, nullptr);
        SetTimer(hwnd_, kFirmwareCheckTimer, 60 * 60 * 1000, nullptr);
        LogLine("Startup complete; entering message loop");

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (overlay_ && overlay_->HandleDialogMessage(&message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        win_sparkle_cleanup();
        RemoveTrayIcon();
        return static_cast<int>(message.wParam);
    } catch (const winrt::hresult_error& error) {
        LogLine("Fatal WinRT error: hr=" + std::to_string(static_cast<std::int32_t>(error.code())) +
                " message=" + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        LogLine(std::string("Fatal exception: ") + error.what());
    } catch (...) {
        LogLine("Fatal unknown exception");
    }
    RemoveTrayIcon();
    return 1;
}

void Win32App::SetStatus(const std::string& status) {
    DispatchToUi([this, status] {
        status_ = status;
        RebuildTooltip();
    });
}

void Win32App::SetConnectedDevices(const std::vector<ConnectedDevice>& devices) {
    DispatchToUi([this, devices] {
        connected_devices_ = devices;
        for (const auto& device : devices) reconnecting_devices_.erase(device.id);
        std::erase_if(battery_status_by_device_id_, [&devices](const auto& entry) {
            return std::none_of(devices.begin(), devices.end(), [&entry](const ConnectedDevice& device) {
                return device.id == entry.first;
            });
        });
        if (pair_device_dialog_) pair_device_dialog_->SetConnectedDevices(devices);
        RebuildTooltip();
    });
}

void Win32App::SetDeviceInfo(const DeviceInfo& info) {
    DispatchToUi([this, info] {
        LogLine("SetDeviceInfo VS-" + info.device_id +
                " hardware=" + (info.hardware.empty() ? "<empty>" : info.hardware) +
                " firmware=" + (info.firmware_version.empty() ? "<empty>" : info.firmware_version));
        const auto previous = device_info_map_.find(info.device_id);
        const bool changed = previous == device_info_map_.end() ||
                             previous->second.hardware != info.hardware ||
                             previous->second.firmware_version != info.firmware_version;
        device_info_map_[info.device_id] = info;
        if (changed && !info.hardware.empty() && !info.firmware_version.empty() &&
            (!pending_pairing_entry_ || pending_pairing_entry_->device_id != info.device_id) &&
            std::find(paired_device_ids_.begin(), paired_device_ids_.end(), info.device_id) != paired_device_ids_.end()) {
            try {
                config_.SavePairedDeviceInfo(info.device_id, info.hardware, info.firmware_version);
            } catch (const std::exception& error) {
                LogLine(std::string("Could not save device firmware information: ") + error.what());
            }
        }
        if (pair_device_dialog_) {
            pair_device_dialog_->SetDeviceInfo(info);
        }
    });
}

void Win32App::SetDeviceBatteryStatus(const std::string& device_id, const DeviceBatteryStatus& status) {
    DispatchToUi([this, device_id, status] {
        if (std::none_of(connected_devices_.begin(), connected_devices_.end(), [&device_id](const ConnectedDevice& device) {
                return device.id == device_id;
            })) return;
        if (const auto previous = battery_status_by_device_id_.find(device_id);
            previous != battery_status_by_device_id_.end() && previous->second == status) return;
        battery_status_by_device_id_[device_id] = status;
        RebuildTooltip();
    });
}

void Win32App::SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id,
                              std::uint64_t revision) {
    DispatchToUi([this, info_by_device_id, revision] {
        if (revision <= firmware_info_revision_) return;
        firmware_info_revision_ = revision;
        firmware_info_map_ = info_by_device_id;
    });
}

void Win32App::HandlePairingCompleted(const std::string& device_id, std::optional<DeviceInfo> info) {
    LogLine("Pairing completed VS-" + device_id +
            (info && !info->firmware_version.empty()
                 ? " firmware=" + info->firmware_version
                 : " firmware=<unknown>"));
    if (pending_pairing_entry_ && pending_pairing_entry_->device_id == device_id) {
        if (info) {
            pending_pairing_entry_->hardware = info->hardware;
            pending_pairing_entry_->firmware_version = info->firmware_version;
        }
        config_.SavePairedDevice(*pending_pairing_entry_);
        pending_pairing_entry_.reset();
        paired_device_ids_ = config_.paired_device_ids;
        if (coordinator_) coordinator_->ConfirmPairedDeviceIds(config_.paired_device_ids, device_id);
        LogLine("Confirmed paired device VS-" + device_id);
    }
    std::string detail = "VS-" + device_id + " paired";
    if (info && !info->hardware.empty()) detail += " (" + info->hardware + ")";
    if (info && !info->firmware_version.empty()) {
        detail += ", firmware " + info->firmware_version;
    }
    ShowNotification("VoiceStick paired", detail);
    RebuildTooltip();
}

void Win32App::SetPairingError(const std::string& device_id, const std::string& message) {
    DispatchToUi([this, device_id, message] {
        if (pending_pairing_entry_ && pending_pairing_entry_->device_id == device_id) {
            pending_pairing_entry_.reset();
        }
        if (message != "cancelled" &&
            std::ranges::find(paired_device_ids_, device_id) != paired_device_ids_.end()) {
            reconnecting_devices_.insert(device_id);
        }
        if (pair_device_dialog_) pair_device_dialog_->SetPairingError(device_id, message);
        LogLine("Pairing error VS-" + device_id + ": " + message);
    });
}

bool Win32App::RecordFirmwareReminder(const std::string& device_key, const std::string& version) {
    if (!seen_firmware_reminders_.emplace(device_key, version).second) return false;
    const auto path = L"Software\\TenClass\\VoiceStick\\FirmwareReminders\\" + Utf16(device_key);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return true;
    }
    const auto value_name = Utf16(version);
    const bool already_shown =
        RegQueryValueExW(key, value_name.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    if (!already_shown) {
        const DWORD shown = 1;
        RegSetValueExW(key, value_name.c_str(), 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&shown), sizeof(shown));
    }
    RegCloseKey(key);
    return !already_shown;
}

void Win32App::CheckFirmwareReminders() {
    if (is_shutting_down_ || is_tray_menu_open_ || is_showing_firmware_prompt_ ||
        manual_firmware_check_device_ || !IsWindowEnabled(hwnd_) ||
        !coordinator_ || !coordinator_->IsIdleForFirmwareReminder() ||
        (pair_device_dialog_ && pair_device_dialog_->hwnd()) ||
        (settings_dialog_ && IsWindowVisible(settings_dialog_->hwnd())) ||
        (firmware_update_dialog_ && IsWindowVisible(firmware_update_dialog_->hwnd()))) return;

    for (const auto& device : connected_devices_) {
        if (!device.supports_firmware_update ||
            std::find(paired_device_ids_.begin(), paired_device_ids_.end(), device.id) == paired_device_ids_.end()) {
            continue;
        }
        const auto entry = firmware_info_map_.find(device.id);
        if (entry == firmware_info_map_.end()) continue;
        const auto& info = entry->second;
        if (!info.reported_on_connection || info.is_checking || !info.error_message.empty() ||
            info.latest_version.empty() || !info.update_available) continue;
        const auto& device_key = info.device_uuid.empty() ? device.id : info.device_uuid;
        if (!RecordFirmwareReminder(device_key, info.latest_version)) continue;
        ShowFirmwareUpdatePrompt(device.id, info);
        return;
    }
}

const wchar_t* Win32App::FirmwareUpdateUnavailableReason(const std::string& device_id,
                                                          const DeviceFirmwareInfo& info) const {
    const auto connected = std::ranges::find(connected_devices_, device_id, &ConnectedDevice::id);
    if (connected == connected_devices_.end()) return L"Connect this device to install the update.";
    if (!info.reported_on_connection) return L"Wait for the device to report its firmware version.";
    if (!connected->supports_firmware_update) return L"This Bluetooth connection does not support firmware updates.";
    return nullptr;
}

void Win32App::ShowFirmwareUpdatePrompt(std::string device_id, DeviceFirmwareInfo info) {
    if (is_shutting_down_ || is_showing_firmware_prompt_) return;
    if (firmware_update_dialog_ && IsWindowVisible(firmware_update_dialog_->hwnd())) {
        firmware_update_dialog_->Show();
        return;
    }
    auto message = L"VS-" + Utf16(device_id) + L" is running firmware " +
                   Utf16(info.current_version) + L".\n\n";
    if (info.requires_input_firmware) message += L"Update the device to use voice input.\n\n";
    const auto* unavailable = FirmwareUpdateUnavailableReason(device_id, info);
    if (unavailable) {
        message += L"Firmware " + Utf16(info.latest_version) + L" is available. " + unavailable;
    } else {
        message += L"Update to " + Utf16(info.latest_version) + L"?";
    }
    is_showing_firmware_prompt_ = true;
    const int result = MessageBoxW(
        hwnd_, message.c_str(),
        info.requires_input_firmware ? L"Firmware update required" : L"Firmware update available",
        MB_ICONINFORMATION | MB_SETFOREGROUND | (unavailable ? MB_OK : (MB_YESNO | MB_DEFBUTTON2)));
    is_showing_firmware_prompt_ = false;
    if (result == IDYES && !is_shutting_down_) StartFirmwareUpdate(device_id, info.latest_version);
}

void Win32App::ShowAppUpdateRequiredForFirmware(const std::string& minimum_version) {
    if (is_shutting_down_ || is_showing_firmware_prompt_) return;
    const auto message = L"You are running VoiceStick " + Utf16(VOICESTICK_VERSION_STR) +
                         L". Update to " + Utf16(minimum_version) +
                         L" or later before updating the firmware.\n\nCheck for app updates now?";
    is_showing_firmware_prompt_ = true;
    const int result = MessageBoxW(hwnd_, message.c_str(), L"Update VoiceStick First",
                                   MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON2 | MB_SETFOREGROUND);
    is_showing_firmware_prompt_ = false;
    if (result == IDYES && !is_shutting_down_) win_sparkle_check_update_with_ui();
}

void Win32App::SetPairedDeviceIds(const std::vector<std::string>& ids) {
    DispatchToUi([this, ids] {
        paired_device_ids_ = ids;
    });
}

void Win32App::SetHasRecoverableInput(bool has_recoverable_input) {
    DispatchToUi([this, has_recoverable_input] {
        has_recoverable_input_ = has_recoverable_input;
    });
}

void Win32App::ShowListening(const OutputProfile& profile, const std::optional<std::string>& device_id) {
    DispatchToUi([this, profile, device_id] {
        if (profile.target == OutputTarget::kFocusedApp) {
            input_injector_.CaptureForegroundTarget();
        }
        ApplyOverlayStyle(device_id);
        if (overlay_) overlay_->ShowListening(OverlayOutputMode(profile));
    });
}

void Win32App::ShowPartial(const std::string& text, const std::optional<std::string>& device_id) {
    DispatchToUi([this, text, device_id] {
        ApplyOverlayStyle(device_id);
        if (overlay_) overlay_->ShowPartial(text, OverlayOutputMode(config_.OutputProfileForDevice(device_id)));
    });
}

void Win32App::ShowFinalCountdown(const std::string& text,
                                  const std::optional<std::string>& device_id,
                                  int duration_ms, std::function<void()> on_complete) {
    DispatchToUi([this, text, device_id, duration_ms, on_complete = std::move(on_complete)]() mutable {
        ApplyOverlayStyle(device_id);
        if (overlay_) overlay_->ShowFinalCountdown(text, duration_ms, std::move(on_complete));
    });
}

void Win32App::ShowPausedFinal(const std::string& text, const std::optional<std::string>& device_id) {
    DispatchToUi([this, text, device_id] {
        ApplyOverlayStyle(device_id);
        const auto info = device_id ? device_info_map_.find(*device_id) : device_info_map_.end();
        const std::string hardware = info != device_info_map_.end() ? info->second.hardware : "";
        const bool stick_s3 = hardware == "stick_s3" || hardware == "sticks3" || hardware == "m5stack_stick_s3";
        const bool supports_shake = hardware == "magiclick_2p5" ||
            (stick_s3 && !info->second.firmware_version.empty() &&
             !FirmwareVersion::IsOlderThan(info->second.firmware_version, "0.4.4"));
        const auto* buttons = info != device_info_map_.end() && info->second.buttons ? &*info->second.buttons : nullptr;
        const bool has_primary = buttons ? std::ranges::find(*buttons, "primary") != buttons->end()
                                         : !hardware.empty();
        const bool has_secondary = buttons ? std::ranges::find(*buttons, "secondary") != buttons->end()
                                           : stick_s3 || hardware == "voicestick_c3";
        if (overlay_) overlay_->ShowPausedFinal(text, supports_shake, has_secondary, has_primary);
    });
}

std::optional<std::string> Win32App::EditedConfirmationText() const {
    return overlay_ ? overlay_->ConfirmationDraftForSending() : std::nullopt;
}

void Win32App::ConcealConfirmation() {
    DispatchToUi([this] {
        if (overlay_) overlay_->ConcealConfirmation();
    });
}

void Win32App::ShowPasteFailure() {
    DispatchToUi([this] {
        if (overlay_) overlay_->ShowPasteFailure();
    });
}

void Win32App::ShowError(const std::string& text,
                         const std::optional<std::string>& device_id,
                         std::function<void()> on_complete) {
    DispatchToUi([this, text, device_id, on_complete = std::move(on_complete)]() mutable {
        ApplyOverlayStyle(device_id);
        if (overlay_) overlay_->ShowError(text, std::move(on_complete));
    });
}

void Win32App::ShowCloudUpgrade(const std::string& message,
                                const std::string& url,
                                const std::optional<std::string>& device_id) {
    DispatchToUi([this, message, url, device_id] {
        ApplyOverlayStyle(device_id);
        auto show_dialog = [this, message, url] {
            const auto text = Utf16(message + "\n\nOpen the VoiceStick Cloud page?");
            const int result = MessageBoxW(hwnd_, text.c_str(),
                                           L"VoiceStick Cloud needs attention",
                                           MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON1);
            if (result == IDYES) {
                const auto wide_url = Utf16(url);
                ShellExecuteW(hwnd_, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        };
        if (overlay_) {
            overlay_->Hide(std::move(show_dialog));
        } else {
            show_dialog();
        }
    });
}

void Win32App::HideOverlay(std::function<void()> on_hidden) {
    DispatchToUi([this, on_hidden = std::move(on_hidden)]() mutable {
        if (overlay_) overlay_->Hide(std::move(on_hidden));
    });
}

void Win32App::ShowSubtitle(const std::string& text,
                            const std::string& device_id,
                            OverlayThemeColor color) {
    DispatchToUi([this, text, device_id, color] {
        if (subtitles_) subtitles_->ShowSubtitle(text, device_id, color);
    });
}

void Win32App::HideSubtitles() {
    DispatchToUi([this] {
        if (subtitles_) subtitles_->HideAll();
    });
}

LRESULT CALLBACK Win32App::WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* app = reinterpret_cast<Win32App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        app = reinterpret_cast<Win32App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        return TRUE;
    }
    return app ? app->HandleMessage(message, w_param, l_param) : DefWindowProcW(hwnd, message, w_param, l_param);
}


LRESULT Win32App::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == kUiDispatchMessage) {
        std::unique_ptr<std::function<void()>> action(
            reinterpret_cast<std::function<void()>*>(w_param));
        if (action && *action) (*action)();
        return 0;
    }
    if (message == BleCentralWin::WM_BLE_DISPATCH) {
        if (ble_central_) ble_central_->ProcessDispatchedCallbacks();
        return 0;
    }
    if (message == taskbar_created_message_) {
        AddTrayIcon();
        return 0;
    }
    if (message == kTrayCallbackMessage) {
        const auto event = static_cast<UINT>(LOWORD(l_param));
        if (event == WM_RBUTTONUP || event == WM_LBUTTONUP ||
            event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT) {
            ShowTrayMenu();
            return 0;
        }
    }
    switch (message) {
    case WM_TIMER:
        if (w_param == kFirmwareReminderTimer) {
            CheckFirmwareReminders();
            return 0;
        }
        if (w_param == kFirmwareCheckTimer) {
            if (coordinator_) coordinator_->CheckFirmwareUpdates();
            return 0;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kMenuRestore:
            if (coordinator_) coordinator_->RestoreLastInputConfirmation();
            return 0;
        case kMenuPairScan:
            ShowPairDeviceDialog();
            return 0;
        case kMenuCheckAppUpdates:
            win_sparkle_check_update_with_ui();
            return 0;
        case kMenuHoldToTalk:
            config_.interaction_mode = InteractionMode::kHoldToTalk;
            SaveInputOptions();
            return 0;
        case kMenuClickToTalk:
            config_.interaction_mode = InteractionMode::kClickToTalk;
            SaveInputOptions();
            return 0;
        case kMenuAutoEnter:
            config_.auto_enter = !config_.auto_enter;
            SaveInputOptions();
            return 0;
        case kMenuOutputFocusedApp:
            config_.default_output_profile.target = OutputTarget::kFocusedApp;
            SaveInputOptions();
            return 0;
        case kMenuOutputSubtitle:
            config_.default_output_profile.target = OutputTarget::kSubtitle;
            SaveInputOptions();
            return 0;
        case kMenuSettings:
            ShowSettings();
            return 0;
        case kMenuQuit:
            ShutdownAndQuit();
            return 0;
        default: {
            UINT cmd = LOWORD(w_param);
            if (cmd >= kMenuConfirmationBase &&
                cmd - kMenuConfirmationBase < std::size(AppConfig::send_confirmation_options_ms)) {
                const int duration = AppConfig::send_confirmation_options_ms[cmd - kMenuConfirmationBase];
                try {
                    auto updated = config_;
                    updated.send_confirmation_ms = duration;
                    updated.Save();
                    config_ = std::move(updated);
                    if (coordinator_) coordinator_->SetSendConfirmationDuration(duration);
                } catch (const std::exception& error) {
                    LogLine(std::string("Confirmation setting save failed: ") + error.what());
                    SetStatus("Input save failed");
                }
            } else if (cmd >= kMenuForgetBase && cmd <= kMenuForgetEnd) {
                std::size_t index = cmd - kMenuForgetBase;
                if (index < paired_device_ids_.size() && coordinator_) {
                    auto device_id = paired_device_ids_[index];
                    try {
                        auto updated = config_;
                        updated.RemovePairedDevice(device_id);
                        config_ = std::move(updated);
                        coordinator_->RemovePairedDevice(device_id);
                        device_info_map_.erase(device_id);
                        reconnecting_devices_.erase(device_id);
                        battery_status_by_device_id_.erase(device_id);
                        RebuildTooltip();
                        LogLine("Forgot device VS-" + device_id);
                    } catch (const std::exception& error) {
                        LogLine(std::string("Device removal failed: ") + error.what());
                        SetStatus("Device removal failed");
                    }
                }
            } else if (cmd >= kMenuUpdateFirmwareBase && cmd <= kMenuUpdateFirmwareEnd) {
                const std::size_t index = cmd - kMenuUpdateFirmwareBase;
                if (index < paired_device_ids_.size()) {
                    const auto& device_id = paired_device_ids_[index];
                    const auto firmware = firmware_info_map_.find(device_id);
                    if (firmware != firmware_info_map_.end() && firmware->second.update_available) {
                        ShowFirmwareUpdatePrompt(device_id, firmware->second);
                    }
                }
            } else if (cmd >= kMenuCheckFirmwareBase && cmd <= kMenuCheckFirmwareEnd) {
                const std::size_t index = cmd - kMenuCheckFirmwareBase;
                if (index < paired_device_ids_.size()) {
                    CheckDeviceFirmwareUpdates(paired_device_ids_[index]);
                }
            } else if (cmd >= kMenuThemeColorBase && cmd <= kMenuThemeColorEnd) {
                const std::size_t offset = cmd - kMenuThemeColorBase;
                const std::size_t index = offset / kMenuOptionsPerDevice;
                const std::size_t color_index = offset % kMenuOptionsPerDevice;
                if (index < paired_device_ids_.size() &&
                    color_index < (sizeof(kOverlayThemeColors) / sizeof(kOverlayThemeColors[0]))) {
                    SaveDeviceThemeColor(paired_device_ids_[index], kOverlayThemeColors[color_index]);
                }
            } else if (cmd >= kMenuOverlayPositionBase && cmd <= kMenuOverlayPositionEnd) {
                const std::size_t offset = cmd - kMenuOverlayPositionBase;
                const std::size_t index = offset / kMenuOptionsPerDevice;
                const std::size_t position_index = offset % kMenuOptionsPerDevice;
                if (index < paired_device_ids_.size() &&
                    position_index < (sizeof(kOverlayPositions) / sizeof(kOverlayPositions[0]))) {
                    SaveDeviceOverlayPosition(paired_device_ids_[index], kOverlayPositions[position_index]);
                }
            } else if (cmd >= kMenuTranslationBase && cmd <= kMenuTranslationEnd) {
                const std::size_t offset = cmd - kMenuTranslationBase;
                const std::size_t index = offset / kMenuTranslationsPerDevice;
                const std::size_t translation_index = offset % kMenuTranslationsPerDevice;
                if (index < paired_device_ids_.size()) {
                    auto profile = config_.OutputProfileForDevice(paired_device_ids_[index]);
                    if (translation_index == 0) {
                        profile.transform = TextTransform::kOriginal;
                    } else if (translation_index == kMenuTranslationsPerDevice - 1) {
                        profile.transform = TextTransform::kRefine;
                    } else {
                        const std::size_t target_index = translation_index - 1;
                        if (target_index < std::size(kTranslationTargets)) {
                            profile.transform = TextTransform::kTranslate;
                            profile.translation_target = kTranslationTargets[target_index].code;
                        }
                    }
                    SaveDeviceOutputProfile(paired_device_ids_[index], profile);
                }
            }
            return 0;
        }
        }
    case WM_CLOSE:
        ShutdownAndQuit();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, kFirmwareReminderTimer);
        KillTimer(hwnd_, kFirmwareCheckTimer);
        updater_window.store(nullptr);
        pair_device_dialog_.reset();
        ble_central_ = nullptr;
        coordinator_.reset();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, w_param, l_param);
    }
    return DefWindowProcW(hwnd_, message, w_param, l_param);
}

void Win32App::ShutdownAndQuit() {
    if (is_shutting_down_) return;
    is_shutting_down_ = true;
    if (pair_device_dialog_) pair_device_dialog_->Close();
    pair_device_dialog_.reset();
    if (coordinator_) coordinator_->Shutdown();
    DestroyWindow(hwnd_);
}

void Win32App::DispatchToUi(std::function<void()> action) {
    if (!action) return;
    if (ui_thread_id_ == 0 || GetCurrentThreadId() == ui_thread_id_) {
        action();
        return;
    }
    PostToUi(std::move(action));
}

void Win32App::PostToUi(std::function<void()> action) {
    auto* heap_action = new std::function<void()>(std::move(action));
    if (!PostMessageW(hwnd_, kUiDispatchMessage, reinterpret_cast<WPARAM>(heap_action), 0)) {
        std::unique_ptr<std::function<void()>> cleanup(heap_action);
    }
}


bool Win32App::CreateWindowInternal() {
    LogLine("Registering window class");
    WNDCLASSW wc{};
    wc.lpfnWndProc = Win32App::WindowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"VoiceStickWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_VOICESTICK_APP));
    RegisterClassW(&wc);
    LogLine("Creating hidden app window");
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"VoiceStick", 0, 0, 0, 0, 0,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;

    LogLine("Creating overlay window object");
    overlay_ = std::make_unique<OverlayWindow>(instance_, hwnd_);
    overlay_->on_send_confirmation = [this] {
        if (coordinator_) coordinator_->ConfirmPendingPaste();
    };
    overlay_->on_cancel_confirmation = [this] {
        if (coordinator_) coordinator_->DismissPendingConfirmation();
    };
    LogLine("Creating subtitle window object");
    subtitles_ = std::make_unique<SubtitleWindow>(instance_, hwnd_);
    return true;
}

void Win32App::AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_VOICESTICK_TRAY), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    if (!data.hIcon) data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_VOICESTICK_APP));
    if (!data.hIcon) data.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"VoiceStick - Not connected");
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        LogLine("Shell_NotifyIcon NIM_ADD failed: " + std::to_string(GetLastError()));
        return;
    }

    data.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
        LogLine("Shell_NotifyIcon NIM_SETVERSION failed: " + std::to_string(GetLastError()));
        return;
    }
    LogLine("Shell_NotifyIcon registered");
}

void Win32App::RemoveTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void Win32App::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (has_recoverable_input_) AppendMenuW(menu, MF_STRING, kMenuRestore, L"Restore Last Input");
    AppendMenuW(menu, MF_STRING, kMenuPairScan, L"Pair Device...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (paired_device_ids_.empty() && connected_devices_.empty()) {
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"No paired VoiceStick devices");
    }

    auto find_connected = [&](const std::string& id) -> const ConnectedDevice* {
        for (const auto& device : connected_devices_) {
            if (device.id == id) return &device;
        }
        return nullptr;
    };

    for (std::size_t i = 0; i < paired_device_ids_.size() && i < 100; ++i) {
        const auto& id = paired_device_ids_[i];
        const auto* connected = find_connected(id);
        const std::string title = connected
            ? (connected->name.empty() ? "VS-" + id : connected->name)
            : "VS-" + id;

        HMENU submenu = CreatePopupMenu();

        // Status
        const wchar_t* status_text = connected ? L"Connected" :
            (reconnecting_devices_.contains(id) ? L"Reconnecting..." : L"Scanning...");
        AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, status_text);
        if (const auto battery = battery_status_by_device_id_.find(id);
            connected && battery != battery_status_by_device_id_.end()) {
            const auto text = L"Battery " + BatteryStatusText(battery->second);
            AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, text.c_str());
        }

        // Hardware + firmware version
        auto info_it = device_info_map_.find(id);
        auto firmware_it = firmware_info_map_.find(id);
        const std::string hardware =
            info_it != device_info_map_.end() && !info_it->second.hardware.empty()
                ? info_it->second.hardware
                : (firmware_it != firmware_info_map_.end() ? firmware_it->second.hardware : std::string{});
        const std::string firmware_version =
            info_it != device_info_map_.end() && !info_it->second.firmware_version.empty()
                ? info_it->second.firmware_version
                : (firmware_it != firmware_info_map_.end() ? firmware_it->second.current_version : std::string{});
        HMENU theme_menu = CreatePopupMenu();
        const auto theme_it = config_.device_theme_colors.find(id);
        const auto current_theme = theme_it != config_.device_theme_colors.end()
            ? theme_it->second
            : OverlayThemeColor::kWhite;
        for (std::size_t color_index = 0;
             color_index < sizeof(kOverlayThemeColors) / sizeof(kOverlayThemeColors[0]);
             ++color_index) {
            const auto color = kOverlayThemeColors[color_index];
            AppendMenuW(
                theme_menu,
                MF_STRING | (current_theme == color ? MF_CHECKED : 0),
                kMenuThemeColorBase + static_cast<UINT>(i * kMenuOptionsPerDevice + color_index),
                Utf16(OverlayThemeColorDisplayName(color)).c_str());
        }
        AppendMenuW(submenu, MF_POPUP, reinterpret_cast<UINT_PTR>(theme_menu), L"Theme Color");

        HMENU position_menu = CreatePopupMenu();
        const auto position_it = config_.device_overlay_positions.find(id);
        const auto current_position = position_it != config_.device_overlay_positions.end()
            ? position_it->second
            : OverlayPosition::kCenter;
        for (std::size_t position_index = 0;
             position_index < sizeof(kOverlayPositions) / sizeof(kOverlayPositions[0]);
             ++position_index) {
            const auto position = kOverlayPositions[position_index];
            AppendMenuW(
                position_menu,
                MF_STRING | (current_position == position ? MF_CHECKED : 0),
                kMenuOverlayPositionBase + static_cast<UINT>(i * kMenuOptionsPerDevice + position_index),
                Utf16(OverlayPositionDisplayName(position)).c_str());
        }
        AppendMenuW(submenu, MF_POPUP, reinterpret_cast<UINT_PTR>(position_menu), L"Overlay Position");

        HMENU translation_menu = CreatePopupMenu();
        const auto current_profile = config_.OutputProfileForDevice(id);
        AppendMenuW(
            translation_menu,
            MF_STRING | (current_profile.transform == TextTransform::kOriginal ? MF_CHECKED : 0),
            kMenuTranslationBase + static_cast<UINT>(i * kMenuTranslationsPerDevice),
            L"Original");
        AppendMenuW(translation_menu,
                    MF_STRING | (current_profile.transform == TextTransform::kRefine ? MF_CHECKED : 0),
                    kMenuTranslationBase + static_cast<UINT>((i + 1) * kMenuTranslationsPerDevice - 1),
                    L"Refine");
        AppendMenuW(translation_menu, MF_SEPARATOR, 0, nullptr);
        for (std::size_t target_index = 0; target_index < std::size(kTranslationTargets); ++target_index) {
            const auto& target = kTranslationTargets[target_index];
            const auto checked = current_profile.transform == TextTransform::kTranslate &&
                                 current_profile.translation_target == target.code;
            auto title = std::wstring(L"Translate to ") + target.name;
            AppendMenuW(
                translation_menu,
                MF_STRING | (checked ? MF_CHECKED : 0),
                kMenuTranslationBase + static_cast<UINT>(i * kMenuTranslationsPerDevice + target_index + 1),
                title.c_str());
        }
        AppendMenuW(submenu, MF_POPUP, reinterpret_cast<UINT_PTR>(translation_menu), L"Text Output");

        AppendMenuW(submenu, MF_SEPARATOR, 0, nullptr);
        const auto identity_text = FirmwareIdentityText(hardware, firmware_version);
        AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, identity_text.c_str());
        const auto* firmware = firmware_it == firmware_info_map_.end() ? nullptr : &firmware_it->second;
        if (firmware && firmware->requires_input_firmware) {
            AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, L"Firmware Update Required for Voice Input");
        }
        if (manual_firmware_check_device_ == id || (firmware && firmware->is_checking)) {
            AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, L"Checking for Firmware Updates...");
        } else if (firmware && firmware->update_available) {
            const auto update_action = L"Update to " + Utf16(firmware->latest_version) + L"...";
            AppendMenuW(submenu, MF_STRING, kMenuUpdateFirmwareBase + static_cast<UINT>(i), update_action.c_str());
        } else {
            AppendMenuW(submenu, manual_firmware_check_device_ ? (MF_STRING | MF_DISABLED) : MF_STRING,
                        kMenuCheckFirmwareBase + static_cast<UINT>(i), L"Check for Firmware Updates...");
            if (firmware && !firmware->required_app_version.empty()) {
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, L"Update VoiceStick First");
            } else if (firmware && !firmware->error_message.empty()) {
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, L"Firmware Check Failed");
            }
        }

        AppendMenuW(submenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(submenu, MF_STRING, kMenuForgetBase + static_cast<UINT>(i),
                    L"Forget This Device");

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), Utf16(title).c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU interaction_menu = CreatePopupMenu();
    AppendMenuW(interaction_menu,
                MF_STRING | (config_.interaction_mode == InteractionMode::kHoldToTalk ? MF_CHECKED : 0),
                kMenuHoldToTalk,
                L"Hold to Talk");
    AppendMenuW(interaction_menu,
                MF_STRING | (config_.interaction_mode == InteractionMode::kClickToTalk ? MF_CHECKED : 0),
                kMenuClickToTalk,
                L"Click to Talk");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(interaction_menu), L"Interaction");

    HMENU output_menu = CreatePopupMenu();
    AppendMenuW(output_menu,
                MF_STRING | (config_.default_output_profile.target == OutputTarget::kFocusedApp ? MF_CHECKED : 0),
                kMenuOutputFocusedApp,
                L"Focused App");
    AppendMenuW(output_menu,
                MF_STRING | (config_.default_output_profile.target == OutputTarget::kSubtitle ? MF_CHECKED : 0),
                kMenuOutputSubtitle,
                L"Subtitle");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(output_menu), L"Output");

    HMENU confirmation_menu = CreatePopupMenu();
    for (UINT index = 0; index < std::size(AppConfig::send_confirmation_options_ms); ++index) {
        const int duration = AppConfig::send_confirmation_options_ms[index];
        std::wstring label;
        if (duration == 0) {
            label = L"Immediately";
        } else if (duration < 0) {
            label = L"Manual";
        } else {
            label = std::to_wstring(duration / 1000) + (duration == 1000 ? L" second" : L" seconds");
        }
        AppendMenuW(confirmation_menu, MF_STRING | (duration == config_.send_confirmation_ms ? MF_CHECKED : 0),
                    kMenuConfirmationBase + index, label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(confirmation_menu), L"Send Confirmation");

    AppendMenuW(menu,
                MF_STRING | (config_.auto_enter ? MF_CHECKED : 0),
                kMenuAutoEnter,
                L"Press Return After Paste");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...");
    AppendMenuW(menu, MF_STRING, kMenuCheckAppUpdates, L"Check for App Updates...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd_);
    is_tray_menu_open_ = true;
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
    is_tray_menu_open_ = false;
    DestroyMenu(menu);
}

void Win32App::SaveInputOptions() {
    try {
        config_.Save();
        if (coordinator_) coordinator_->UpdateConfig(config_);
        LogLine("Input options saved");
    } catch (const std::exception& error) {
        LogLine(std::string("Input options save failed: ") + error.what());
        SetStatus("Input save failed");
    }
}

void Win32App::SaveDeviceThemeColor(const std::string& device_id, OverlayThemeColor color) {
    try {
        if (color == OverlayThemeColor::kWhite) {
            config_.device_theme_colors.erase(device_id);
        } else {
            config_.device_theme_colors[device_id] = color;
        }
        config_.Save();
        ApplyOverlayStyle(device_id);
        LogLine("Theme color saved VS-" + device_id + "=" + OverlayThemeColorName(color));
    } catch (const std::exception& error) {
        LogLine(std::string("Theme color save failed: ") + error.what());
        SetStatus("Theme save failed");
    }
}

void Win32App::SaveDeviceOverlayPosition(const std::string& device_id, OverlayPosition position) {
    try {
        if (position == OverlayPosition::kCenter) {
            config_.device_overlay_positions.erase(device_id);
        } else {
            config_.device_overlay_positions[device_id] = position;
        }
        config_.Save();
        ApplyOverlayStyle(device_id);
        LogLine("Overlay position saved VS-" + device_id + "=" + OverlayPositionName(position));
    } catch (const std::exception& error) {
        LogLine(std::string("Overlay position save failed: ") + error.what());
        SetStatus("Position save failed");
    }
}

void Win32App::SaveDeviceOutputProfile(const std::string& device_id, OutputProfile profile) {
    try {
        profile.target = config_.default_output_profile.target;
        OutputProfile default_profile = config_.default_output_profile;
        default_profile.target = profile.target;
        if (profile.transform == default_profile.transform &&
            profile.translation_target == default_profile.translation_target) {
            config_.device_output_profiles.erase(device_id);
        } else {
            config_.device_output_profiles[device_id] = profile;
        }
        config_.Save();
        if (coordinator_) coordinator_->UpdateConfig(config_);
        LogLine("Output profile saved VS-" + device_id + "=" +
                TextTransformName(profile.transform) + ":" + profile.translation_target);
    } catch (const std::exception& error) {
        LogLine(std::string("Output profile save failed: ") + error.what());
        SetStatus("Output save failed");
    }
}

void Win32App::ApplyOverlayStyle(const std::optional<std::string>& device_id) {
    if (!overlay_) return;
    OverlayThemeColor color = OverlayThemeColor::kWhite;
    OverlayPosition position = OverlayPosition::kCenter;
    if (device_id.has_value()) {
        if (auto color_it = config_.device_theme_colors.find(*device_id);
            color_it != config_.device_theme_colors.end()) {
            color = color_it->second;
        }
        if (auto position_it = config_.device_overlay_positions.find(*device_id);
            position_it != config_.device_overlay_positions.end()) {
            position = position_it->second;
        }
    }
    overlay_->SetThemeColor(color);
    overlay_->SetPosition(position);
}

void Win32App::RebuildTooltip() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    auto tip = Utf16(std::string("VoiceStick - ") +
                     (connected_devices_.empty() ? "Not connected" : "Connected"));
    if (connected_devices_.size() == 1) {
        const auto battery = battery_status_by_device_id_.find(connected_devices_.front().id);
        if (battery != battery_status_by_device_id_.end()) tip += L" - " + BatteryStatusText(battery->second);
    }
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Win32App::RegisterTaskbarMessage() {
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
}

bool Win32App::ShowOnboardingIfNeeded() {
    if (!NeedsOnboarding(config_)) return true;
    if (!ShowOnboarding()) {
        LogLine("Initial onboarding cancelled; exiting");
        return false;
    }
    return true;
}

bool Win32App::ShowOnboarding() {
    OnboardingDialog dialog(instance_, hwnd_, config_);
    dialog.on_pair_device_requested = [this] {
        ShowPairDeviceDialog();
    };
    dialog.on_config_completed = [this](AppConfig new_config) {
        config_ = std::move(new_config);
        paired_device_ids_ = config_.paired_device_ids;
        if (coordinator_) coordinator_->UpdateConfig(config_);
        RebuildTooltip();
        LogLine("Onboarding completed");
    };
    return dialog.Show();
}

void Win32App::ShowPairDeviceDialog() {
    if (is_shutting_down_ || pair_device_dialog_) return;
    auto dialog = std::make_shared<PairDeviceDialog>(
        instance_, hwnd_, config_.paired_device_ids,
        [this](std::string device_id, std::uint64_t bluetooth_address,
               BluetoothAddressKind address_kind, std::string name) {
            PairDevice(device_id, bluetooth_address, address_kind, name);
        },
        [this](std::string device_id, std::optional<DeviceInfo> info) {
            HandlePairingCompleted(device_id, std::move(info));
        });
    dialog->on_pair_timeout = [this](std::string device_id) {
        pending_pairing_entry_.reset();
        if (coordinator_) coordinator_->CancelPendingConnect(device_id);
        LogLine("Pairing timed out VS-" + device_id);
    };
    pair_device_dialog_ = dialog;
    if (ble_central_) ble_central_->SetPairingDialogOpen(true);
    dialog->Show();
    pair_device_dialog_.reset();
    if (ble_central_) ble_central_->SetPairingDialogOpen(false);
}

void Win32App::ShowSettings() {
    if (!settings_dialog_) {
        settings_dialog_ = std::make_unique<SettingsDialog>(instance_, hwnd_, config_);
        settings_dialog_->on_config_changed = [this](AppConfig new_config) {
            config_ = std::move(new_config);
            if (coordinator_) coordinator_->UpdateConfig(config_);
            LogLine("Settings saved");
        };
    }
    settings_dialog_->Show();
}

void Win32App::CheckDeviceFirmwareUpdates(std::string device_id) {
    if (!coordinator_ || manual_firmware_check_device_) return;
    manual_firmware_check_device_ = device_id;
    coordinator_->CheckDeviceFirmwareUpdates(device_id, [this, device_id](DeviceFirmwareInfo result) {
        PostToUi([this, device_id, result = std::move(result)] {
            if (is_shutting_down_ ||
                std::find(paired_device_ids_.begin(), paired_device_ids_.end(), device_id) == paired_device_ids_.end()) {
                manual_firmware_check_device_.reset();
                return;
            }
            if (!result.required_app_version.empty()) {
                ShowAppUpdateRequiredForFirmware(result.required_app_version);
            } else if (result.update_available) {
                if (result.reported_on_connection || !result.device_uuid.empty()) {
                    const auto& device_key = result.device_uuid.empty() ? device_id : result.device_uuid;
                    RecordFirmwareReminder(device_key, result.latest_version);
                }
                ShowFirmwareUpdatePrompt(device_id, std::move(result));
            } else {
                const wchar_t* title;
                std::wstring message;
                UINT icon = MB_ICONINFORMATION;
                if (!result.error_message.empty()) {
                    title = L"Firmware update check failed";
                    message = Utf16(result.error_message);
                    icon = MB_ICONWARNING;
                } else {
                    message = L"VS-" + Utf16(device_id) + L" is running firmware " + Utf16(result.current_version) +
                              L". The latest firmware is " + Utf16(result.latest_version) + L".";
                    title = L"No newer firmware available";
                }
                MessageBoxW(hwnd_, message.c_str(), title, MB_OK | icon | MB_SETFOREGROUND);
            }
            manual_firmware_check_device_.reset();
        });
    });
}

void Win32App::StartFirmwareUpdate(const std::string& device_id, std::string_view expected_version) {
    if (!coordinator_) return;
    if (firmware_update_dialog_ && IsWindowVisible(firmware_update_dialog_->hwnd())) {
        firmware_update_dialog_->Show();
        return;
    }
    const auto firmware_it = firmware_info_map_.find(device_id);
    if (firmware_it == firmware_info_map_.end()) return;
    const auto& firmware = firmware_it->second;
    if (expected_version != firmware.latest_version) return;
    if (!firmware.required_app_version.empty()) {
        ShowAppUpdateRequiredForFirmware(firmware.required_app_version);
        return;
    }
    if (!firmware.update_available || firmware.is_checking || !firmware.error_message.empty()) return;
    if (const auto* unavailable = FirmwareUpdateUnavailableReason(device_id, firmware)) {
        MessageBoxW(hwnd_, unavailable, L"Firmware update unavailable", MB_OK | MB_ICONINFORMATION);
        return;
    }
    firmware_update_dialog_ = std::make_unique<FirmwareUpdateDialog>(
        instance_, hwnd_, firmware.latest_version);
    firmware_update_dialog_->on_cancel = [this] {
        if (coordinator_) coordinator_->CancelFirmwareUpdate();
    };
    firmware_update_dialog_->Show();
    coordinator_->UpdateFirmwareFromLatest(
        device_id,
        [this](FirmwareUpdateProgress progress) {
            DispatchToUi([this, progress] {
                if (firmware_update_dialog_) firmware_update_dialog_->UpdateProgress(progress);
            });
        },
        [this](bool success, std::string message) {
            DispatchToUi([this, success, message] {
                if (firmware_update_dialog_) firmware_update_dialog_->Finish(success, message);
                if (success) {
                    ShowNotification("VoiceStick firmware updated",
                                     "The device is rebooting into the new firmware.");
                }
            });
        });
}

void Win32App::PairDevice(const std::string& device_id, std::uint64_t bluetooth_address,
                          BluetoothAddressKind address_kind, const std::string& name) {
    if (coordinator_) {
        pending_pairing_entry_ = PairedDeviceEntry{device_id, bluetooth_address, address_kind, name};
        coordinator_->PairDevice(device_id, bluetooth_address, address_kind, name);
        LogLine("Pairing device VS-" + device_id);
    }
}

void Win32App::ShowNotification(const std::string& title, const std::string& body) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    const auto title_w = Utf16(title);
    const auto body_w = Utf16(body);
    wcsncpy_s(data.szInfoTitle, title_w.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, body_w.c_str(), _TRUNCATE);
    data.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

std::wstring Win32App::Utf16(const std::string& text) const {
    return Utf16FromUtf8(text);
}

} // namespace voicestick
