#pragma once

#include "voice_stick_coordinator.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

namespace voicestick {

class FirmwareUpdateDialog {
public:
    FirmwareUpdateDialog(HINSTANCE instance, HWND owner, std::string version);
    ~FirmwareUpdateDialog();

    void Show();
    void UpdateProgress(const FirmwareUpdateProgress& progress);
    void Finish(bool success, const std::string& message);

    std::function<void()> on_cancel;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildUi();
    void DestroyControls();
    void CenterWindow();
    void SetText(HWND control, const std::wstring& text);
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    HWND title_label_ = nullptr;
    HWND detail_label_ = nullptr;
    HWND progress_bar_ = nullptr;
    HWND percent_label_ = nullptr;
    HWND cancel_button_ = nullptr;
    HWND close_button_ = nullptr;
    HFONT ui_font_ = nullptr;
    std::string version_;
    UINT dpi_ = 96;
    int displayed_percent_ = 0;
    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;

    static constexpr int kClientWidth = 430;
    static constexpr int kClientHeight = 190;
};

} // namespace voicestick
