#include "win32_app.h"

#include "asr_client_win.h"
#include "ble_central_win.h"
#include "log.h"
#include "resource.h"

#include <Shellapi.h>
#include <winsparkle.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <iterator>
#include <optional>

namespace voicestick {

namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kUiDispatchMessage = WM_APP + 2;
constexpr UINT kTrayIconId = 1;
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
constexpr UINT kMenuOptionsPerDevice = 6;
constexpr UINT kMenuTranslationsPerDevice = 24;

#ifndef VOICESTICK_APPCAST_URL
#define VOICESTICK_APPCAST_URL "https://78.github.io/voicestick/appcast.xml"
#endif

void LogLine(std::string_view message) {
    voicestick::LogApp(message);
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
            };
        }
    }
}

int Win32App::Run() {
    LogLine("VoiceStickApp starting");
    ui_thread_id_ = GetCurrentThreadId();
    if (!CreateWindowInternal()) {
        LogLine("CreateWindowInternal failed");
        return 1;
    }
    win_sparkle_set_appcast_url(VOICESTICK_APPCAST_URL);
    win_sparkle_set_automatic_check_for_updates(1);
    win_sparkle_set_update_check_interval(86400);
    win_sparkle_init();

    RegisterTaskbarMessage();
    AddTrayIcon();
    auto ble = std::make_unique<BleCentralWin>(config_.paired_device_ids, hwnd_);
    ble_central_ = ble.get();
    coordinator_ = std::make_unique<VoiceStickCoordinator>(
        config_,
        std::move(ble),
        std::make_unique<AsrClientWin>(config_),
        this,
        &input_injector_,
        [](const AppConfig& config) {
            return std::make_unique<AsrClientWin>(config);
        });
    coordinator_->Start();

    for (const auto& entry : config_.paired_devices) {
        if (entry.bluetooth_address != 0) {
            coordinator_->ConnectPairedDevice(entry.device_id, entry.bluetooth_address,
                                             entry.address_kind, entry.name);
        }
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    win_sparkle_cleanup();
    RemoveTrayIcon();
    return static_cast<int>(message.wParam);
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
        if (pair_device_dialog_) pair_device_dialog_->SetConnectedDevices(devices);
        RebuildTooltip();
    });
}

void Win32App::SetDeviceInfo(const DeviceInfo& info) {
    DispatchToUi([this, info] {
        LogLine("SetDeviceInfo VS-" + info.device_id +
                " hardware=" + (info.hardware.empty() ? "<empty>" : info.hardware) +
                " firmware=" + (info.firmware_version.empty() ? "<empty>" : info.firmware_version));
        device_info_map_[info.device_id] = info;
        if (pair_device_dialog_) {
            pair_device_dialog_->SetDeviceInfo(info);
        }
    });
}

void Win32App::SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id) {
    DispatchToUi([this, info_by_device_id] {
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
        if (coordinator_) coordinator_->ConfirmPairedDeviceIds(config_.paired_device_ids);
        if (coordinator_) coordinator_->CheckFirmwareAfterPairing(device_id);
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
        if (pair_device_dialog_) pair_device_dialog_->SetPairingError(device_id, message);
        LogLine("Pairing error VS-" + device_id + ": " + message);
    });
}

void Win32App::ShowFirmwareUpdatePrompt(const std::string& device_id,
                                        const std::string& current_version,
                                        const std::string& latest_version,
                                        bool is_below_minimum) {
    DispatchToUi([this, device_id, current_version, latest_version, is_below_minimum] {
        const auto message = L"VS-" + Utf16(device_id) + L" is running firmware " +
                             Utf16(current_version) + L".\n\nThe latest firmware is " +
                             Utf16(latest_version) + L".";
        const int result = MessageBoxW(
            hwnd_,
            message.c_str(),
            is_below_minimum ? L"Firmware update recommended" : L"Firmware update available",
            MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON1);
        if (result == IDYES) {
            StartFirmwareUpdate(device_id);
        }
    });
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

void Win32App::ShowListening(const std::optional<std::string>& device_id) {
    DispatchToUi([this, device_id] {
        ApplyOverlayStyle(std::optional<std::string>(device_id));
        if (overlay_) overlay_->ShowListening();
    });
}

void Win32App::ShowPartial(const std::string& text, const std::optional<std::string>& device_id) {
    DispatchToUi([this, text, device_id] {
        ApplyOverlayStyle(std::optional<std::string>(device_id));
        if (overlay_) overlay_->ShowPartial(text);
    });
}

void Win32App::ShowFinalCountdown(const std::string& text,
                                  const std::optional<std::string>& device_id,
                                  std::function<void()> on_complete) {
    DispatchToUi([this, text, device_id, on_complete = std::move(on_complete)]() mutable {
        ApplyOverlayStyle(device_id);
        if (overlay_) overlay_->ShowFinalCountdown(text, std::move(on_complete));
    });
}

void Win32App::ShowPausedFinal(const std::string& text, const std::optional<std::string>& device_id) {
    DispatchToUi([this, text, device_id] {
        ApplyOverlayStyle(device_id);
        if (overlay_) overlay_->ShowPausedFinal(text);
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
            if (cmd >= kMenuForgetBase && cmd <= kMenuForgetEnd) {
                std::size_t index = cmd - kMenuForgetBase;
                if (index < paired_device_ids_.size() && coordinator_) {
                    auto device_id = paired_device_ids_[index];
                    coordinator_->RemovePairedDevice(device_id);
                    config_.RemovePairedDevice(device_id);
                    LogLine("Forgot device VS-" + device_id);
                }
            } else if (cmd >= kMenuUpdateFirmwareBase && cmd <= kMenuUpdateFirmwareEnd) {
                std::size_t index = cmd - kMenuUpdateFirmwareBase;
                if (index < paired_device_ids_.size()) {
                    StartFirmwareUpdate(paired_device_ids_[index]);
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
    case WM_DESTROY:
        pair_device_dialog_.reset();
        ble_central_ = nullptr;
        coordinator_.reset();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, w_param, l_param);
    }
}

void Win32App::ShutdownAndQuit() {
    if (is_shutting_down_) return;
    is_shutting_down_ = true;
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

    auto* heap_action = new std::function<void()>(std::move(action));
    if (!PostMessageW(hwnd_, kUiDispatchMessage, reinterpret_cast<WPARAM>(heap_action), 0)) {
        std::unique_ptr<std::function<void()>> cleanup(heap_action);
    }
}


bool Win32App::CreateWindowInternal() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Win32App::WindowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"VoiceStickWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_VOICESTICK_APP));
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"VoiceStick", 0, 0, 0, 0, 0,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;

    overlay_ = std::make_unique<OverlayWindow>(instance_, hwnd_);
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
        const wchar_t* status_text = connected ? L"Connected" : L"Scanning...";
        AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, status_text);

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
        auto identity_text = FirmwareIdentityText(hardware, firmware_version);
        AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, identity_text.c_str());

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
        AppendMenuW(submenu, MF_POPUP, reinterpret_cast<UINT_PTR>(translation_menu), L"Translation");

        if (firmware_it != firmware_info_map_.end()) {
            const auto& firmware = firmware_it->second;
            if (firmware.is_checking) {
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, L"Checking for firmware updates...");
            } else if (!firmware.error_message.empty()) {
                auto error_text = L"Firmware Check Failed";
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, error_text);
            } else if (firmware.update_available && !firmware.latest_version.empty()) {
                auto update_text = L"Update available: " + Utf16(firmware.latest_version);
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, update_text.c_str());
                auto update_action = L"Update to " + Utf16(firmware.latest_version) + L"...";
                AppendMenuW(submenu,
                            connected ? MF_STRING : (MF_STRING | MF_DISABLED),
                            kMenuUpdateFirmwareBase + static_cast<UINT>(i),
                            update_action.c_str());
            } else if (!firmware.latest_version.empty() && !firmware.current_version.empty()) {
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, L"Firmware Up to Date");
            } else if (!firmware.latest_version.empty()) {
                auto latest_text = L"Latest firmware " + Utf16(firmware.latest_version);
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, latest_text.c_str());
                auto update_action = L"Update to " + Utf16(firmware.latest_version) + L"...";
                AppendMenuW(submenu,
                            connected ? MF_STRING : (MF_STRING | MF_DISABLED),
                            kMenuUpdateFirmwareBase + static_cast<UINT>(i),
                            update_action.c_str());
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
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
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
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Win32App::RegisterTaskbarMessage() {
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
}

void Win32App::ShowPairDeviceDialog() {
    pair_device_dialog_ = std::make_unique<PairDeviceDialog>(
        instance_, hwnd_, config_.paired_device_ids,
        [this](std::string device_id, std::uint64_t bluetooth_address,
               BluetoothAddressKind address_kind, std::string name) {
            PairDevice(device_id, bluetooth_address, address_kind, name);
        },
        [this](std::string device_id, std::optional<DeviceInfo> info) {
            HandlePairingCompleted(device_id, std::move(info));
        });
    pair_device_dialog_->on_pair_timeout = [this](std::string device_id) {
        pending_pairing_entry_.reset();
        if (coordinator_) coordinator_->CancelPendingConnect(device_id);
        LogLine("Pairing timed out VS-" + device_id);
    };
    pair_device_dialog_->Show();
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

void Win32App::StartFirmwareUpdate(const std::string& device_id) {
    if (!coordinator_) return;
    auto firmware_it = firmware_info_map_.find(device_id);
    const std::string version = firmware_it != firmware_info_map_.end()
                                    ? firmware_it->second.latest_version
                                    : std::string();
    firmware_update_dialog_ = std::make_unique<FirmwareUpdateDialog>(
        instance_, hwnd_, version.empty() ? "latest" : version);
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
        coordinator_->ConnectPairedDevice(device_id, bluetooth_address, address_kind, name);
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
