#include "firmware_update_dialog.h"

#include "dpi_util.h"

#include <CommCtrl.h>

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace voicestick {

namespace {

constexpr UINT kCancelId = 101;
constexpr UINT kCloseId = 102;
constexpr UINT kWebFlasherId = 103;

std::wstring Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), len);
    return wide;
}

void AlignDialogData(std::vector<BYTE>* buffer, std::size_t alignment) {
    while (buffer->size() % alignment != 0) buffer->push_back(0);
}

void AppendDialogData(std::vector<BYTE>* buffer, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

void AppendDialogWord(std::vector<BYTE>* buffer, WORD value) {
    AppendDialogData(buffer, &value, sizeof(value));
}

void AppendDialogWideString(std::vector<BYTE>* buffer, const wchar_t* text) {
    while (text && *text) {
        AppendDialogWord(buffer, static_cast<WORD>(*text));
        ++text;
    }
    AppendDialogWord(buffer, 0);
}

} // namespace

FirmwareUpdateDialog::FirmwareUpdateDialog(HINSTANCE instance, HWND owner, std::string version)
    : instance_(instance), owner_(owner), version_(std::move(version)) {}

FirmwareUpdateDialog::~FirmwareUpdateDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (ui_font_) DeleteObject(ui_font_);
}

void FirmwareUpdateDialog::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);

    hwnd_ = CreateDialogIndirectParamW(instance_, BuildDialogTemplate(), owner_,
                                       FirmwareUpdateDialog::DialogProc,
                                       reinterpret_cast<LPARAM>(this));
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void FirmwareUpdateDialog::UpdateProgress(const FirmwareUpdateProgress& progress) {
    if (!hwnd_ || progress.total_bytes <= 0) return;
    const int percent = std::max(
        displayed_percent_,
        std::clamp((progress.written_bytes * 100) / progress.total_bytes, 0, 100));
    displayed_percent_ = percent;
    SendMessageW(progress_bar_, PBM_SETPOS, percent, 0);
    SetText(percent_label_, std::to_wstring(percent) + L"%");
    if (percent >= 100) {
        SetText(detail_label_, L"Finalizing firmware update...");
    } else if (progress.written_bytes > 0) {
        SetText(detail_label_, L"Writing over Bluetooth... " +
            std::to_wstring(progress.written_bytes / 1024) + L" / " +
            std::to_wstring(progress.total_bytes / 1024) + L" KiB confirmed");
    } else {
        SetText(detail_label_, L"Preparing the device for the firmware transfer...");
    }
}

void FirmwareUpdateDialog::Finish(bool success, const std::string& message) {
    if (!hwnd_) return;
    EnableWindow(cancel_button_, FALSE);
    EnableWindow(close_button_, TRUE);
    if (success) {
        displayed_percent_ = 100;
        SetText(title_label_, L"Firmware Updated");
        SetText(detail_label_, L"The device is rebooting into the new firmware.");
        SendMessageW(progress_bar_, PBM_SETPOS, 100, 0);
        SetText(percent_label_, L"100%");
    } else {
        const bool cancelled = message == "Firmware update cancelled.";
        SetText(title_label_, cancelled ? L"Update Cancelled" : L"Update Failed");
        std::wstring detail = Utf16(message.empty() ? "Firmware update failed." : message);
        if (!cancelled) {
            detail += L"\nYou can also flash firmware over USB on the VoiceStick website.";
        }
        SetText(detail_label_, detail);
        if (web_flasher_button_ && !cancelled) {
            ShowWindow(web_flasher_button_, SW_SHOW);
        }
    }
}

INT_PTR CALLBACK FirmwareUpdateDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* dialog = reinterpret_cast<FirmwareUpdateDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<FirmwareUpdateDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        dialog->BuildUi();
        dialog->CenterWindow();
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR FirmwareUpdateDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(w_param) == kWebFlasherId) {
            if (on_open_web_flasher) on_open_web_flasher();
            return TRUE;
        }
        if (LOWORD(w_param) == kCancelId) {
            EnableWindow(cancel_button_, FALSE);
            SetText(title_label_, L"Cancelling Firmware Update");
            SetText(detail_label_, L"Stopping transfer and asking the device to abort.");
            if (on_cancel) on_cancel();
            return TRUE;
        }
        if (LOWORD(w_param) == kCloseId) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            return TRUE;
        }
        break;
    case WM_CLOSE:
        if (!close_button_ || IsWindowEnabled(close_button_)) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        return TRUE;
    case WM_DPICHANGED: {
        UINT new_dpi = HIWORD(w_param);
        if (new_dpi != 0 && new_dpi != dpi_) {
            dpi_ = new_dpi;
            auto* rect = reinterpret_cast<const RECT*>(l_param);
            SetWindowPos(hwnd_, nullptr, rect->left, rect->top,
                         rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            BuildUi();
        }
        return TRUE;
    }
    case WM_DESTROY:
        hwnd_ = nullptr;
        DestroyControls();
        return TRUE;
    }
    return FALSE;
}

LPCDLGTEMPLATE FirmwareUpdateDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog{};
    dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog.cdit = 0;
    dialog.x = 0;
    dialog.y = 0;
    dialog.cx = 250;
    dialog.cy = 115;
    AppendDialogData(&dialog_template_, &dialog, sizeof(dialog));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, L"Firmware Update");
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void FirmwareUpdateDialog::BuildUi() {
    DestroyControls();

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), Dp(kClientHeight)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);

    ui_font_ = CreateUiFont(dpi_);
    const HFONT font = ui_font_;
    auto remember = [&](HWND control) {
        if (control) {
            all_controls_.push_back(control);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        return control;
    };

    title_label_ = remember(CreateWindowExW(0, L"STATIC", L"Updating Firmware",
                                            WS_CHILD | WS_VISIBLE, Dp(24), Dp(20), Dp(370), Dp(24),
                                            hwnd_, nullptr, instance_, nullptr));
    const auto detail = L"Downloading OTA firmware " + Utf16(version_) + L"...";
    detail_label_ = remember(CreateWindowExW(0, L"STATIC", detail.c_str(),
                                             WS_CHILD | WS_VISIBLE, Dp(24), Dp(50), Dp(390), Dp(52),
                                             hwnd_, nullptr, instance_, nullptr));
    progress_bar_ = remember(CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                             WS_CHILD | WS_VISIBLE, Dp(24), Dp(112), Dp(330), Dp(20),
                                             hwnd_, nullptr, instance_, nullptr));
    SendMessageW(progress_bar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    percent_label_ = remember(CreateWindowExW(0, L"STATIC", L"0%",
                                              WS_CHILD | WS_VISIBLE | SS_RIGHT, Dp(360), Dp(112), Dp(55), Dp(20),
                                              hwnd_, nullptr, instance_, nullptr));
    web_flasher_button_ = remember(CreateWindowExW(0, L"BUTTON", L"Open Web Flasher",
                                                   WS_CHILD | BS_PUSHBUTTON,
                                                   Dp(24), Dp(156), Dp(140), Dp(28),
                                                   hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWebFlasherId)),
                                                   instance_, nullptr));
    cancel_button_ = remember(CreateWindowExW(0, L"BUTTON", L"Cancel",
                                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                              Dp(245), Dp(156), Dp(80), Dp(28),
                                              hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)),
                                              instance_, nullptr));
    close_button_ = remember(CreateWindowExW(0, L"BUTTON", L"Close",
                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                             Dp(335), Dp(156), Dp(80), Dp(28),
                                             hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseId)),
                                             instance_, nullptr));
    EnableWindow(close_button_, FALSE);
}

void FirmwareUpdateDialog::DestroyControls() {
    for (HWND control : all_controls_) {
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    all_controls_.clear();
    title_label_ = nullptr;
    detail_label_ = nullptr;
    progress_bar_ = nullptr;
    percent_label_ = nullptr;
    web_flasher_button_ = nullptr;
    cancel_button_ = nullptr;
    close_button_ = nullptr;
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void FirmwareUpdateDialog::CenterWindow() {
    RECT window_rect{};
    GetWindowRect(hwnd_, &window_rect);
    const int window_width = window_rect.right - window_rect.left;
    const int window_height = window_rect.bottom - window_rect.top;
    RECT work_area = GetWorkAreaForWindow(hwnd_);
    const int x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
    const int y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void FirmwareUpdateDialog::SetText(HWND control, const std::wstring& text) {
    if (control) SetWindowTextW(control, text.c_str());
}

int FirmwareUpdateDialog::Dp(int px) const {
    return voicestick::ScalePx(px, dpi_);
}

} // namespace voicestick
