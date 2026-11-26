#pragma once

#include "voice_stick_coordinator.h"

#include <Windows.h>
#include <ole2.h>
#include <UIAutomationClient.h>
#include <wrl/client.h>

#include <string>

namespace voicestick {

class InputInjectorWin : public InputInjector {
public:
    bool Paste(const std::string& text, bool press_enter) override;
    bool PasteConfirmation(const std::string& text, bool press_enter, bool use_current_field,
                           const std::function<void()>& conceal) override;
    void CaptureForegroundTarget();
    void CaptureConfirmationTarget() override;
    void RememberTarget() override { last_target_ = target_; }
    void RestoreTarget() override { target_ = last_target_; }
    void SetClipboardOwner(HWND owner) { clipboard_owner_ = owner; }

private:
    enum class FocusPolicy { kWindow, kOriginalField, kCurrentField };

    struct Target {
        HWND window = nullptr;
        HWND control = nullptr;
        DWORD process = 0;
        Microsoft::WRL::ComPtr<IUIAutomationElement> field;
    };

    IUIAutomation* Automation();
    Target ForegroundTarget();
    bool IsTargetFocused(const Target& target);
    bool RestoreFocus(const Target& target);
    bool PasteTo(const Target& target, const std::string& text, bool press_enter, FocusPolicy focus);
    static std::wstring Utf16FromUtf8(const std::string& text);
    Target target_;
    Target last_target_;
    Microsoft::WRL::ComPtr<IUIAutomation2> automation_;
    HWND clipboard_owner_ = nullptr;
};

} // namespace voicestick
