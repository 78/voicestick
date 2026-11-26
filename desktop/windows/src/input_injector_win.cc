#include "input_injector_win.h"

#include <array>
#include <utility>

namespace voicestick {

namespace {

INPUT Key(WORD key, DWORD flags = 0) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = flags;
    return input;
}

HWND FocusedControl(HWND window) {
    const auto thread = GetWindowThreadProcessId(window, nullptr);
    GUITHREADINFO info{sizeof(info)};
    if (!thread || !GetGUIThreadInfo(thread, &info)) return nullptr;
    const HWND focus = info.hwndFocus;
    return focus && GetAncestor(focus, GA_ROOT) == window ? focus : nullptr;
}

} // namespace

bool InputInjectorWin::Paste(const std::string& text, bool press_enter) {
    const auto target = target_;
    return PasteTo(target, text, press_enter, FocusPolicy::kWindow);
}

IUIAutomation* InputInjectorWin::Automation() {
    if (!automation_ && SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation_)))) {
        automation_->put_ConnectionTimeout(200);
        automation_->put_TransactionTimeout(200);
    }
    return automation_.Get();
}

InputInjectorWin::Target InputInjectorWin::ForegroundTarget() {
    Target target;
    target.window = GetForegroundWindow();
    if (!GetWindowThreadProcessId(target.window, &target.process) ||
        target.process == GetCurrentProcessId()) return {};
    target.control = FocusedControl(target.window);
    if (auto* automation = Automation()) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        int process = 0;
        CONTROLTYPEID type = 0;
        VARIANT text_pattern{};
        if (SUCCEEDED(automation->GetFocusedElement(&element)) && element &&
            SUCCEEDED(element->get_CurrentProcessId(&process)) &&
            static_cast<DWORD>(process) == target.process &&
            SUCCEEDED(element->get_CurrentControlType(&type))) {
            if (type == UIA_EditControlTypeId ||
                (SUCCEEDED(element->GetCurrentPropertyValue(UIA_IsTextPatternAvailablePropertyId,
                                                            &text_pattern)) &&
                 text_pattern.vt == VT_BOOL && text_pattern.boolVal == VARIANT_TRUE)) {
                target.field = std::move(element);
            }
        }
        VariantClear(&text_pattern);
    }
    return GetForegroundWindow() == target.window ? target : Target{};
}

void InputInjectorWin::CaptureForegroundTarget() {
    target_ = ForegroundTarget();
}

void InputInjectorWin::CaptureConfirmationTarget() {
    if (!target_.window) {
        CaptureForegroundTarget();
        return;
    }
    if (target_.window && GetForegroundWindow() == target_.window) {
        const auto current = ForegroundTarget();
        if (current.window == target_.window && current.process == target_.process) target_ = current;
    }
}

bool InputInjectorWin::IsTargetFocused(const Target& target) {
    if (GetForegroundWindow() != target.window) return false;
    if (target.field) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> current;
        BOOL equal = FALSE;
        return automation_ && SUCCEEDED(automation_->GetFocusedElement(&current)) && current &&
               SUCCEEDED(automation_->CompareElements(target.field.Get(), current.Get(), &equal)) && equal &&
               GetForegroundWindow() == target.window;
    }
    return !target.control || FocusedControl(target.window) == target.control;
}

bool InputInjectorWin::RestoreFocus(const Target& target) {
    if (GetForegroundWindow() != target.window) {
        SetForegroundWindow(target.window);
        for (int i = 0; i < 4 && GetForegroundWindow() != target.window; ++i) Sleep(25);
        if (GetForegroundWindow() != target.window) return false;
    }
    if (IsTargetFocused(target)) return true;
    auto field = target.field;
    if (!field && target.control && IsChild(target.window, target.control)) {
        if (auto* automation = Automation()) automation->ElementFromHandle(target.control, &field);
    }
    if (!field || GetForegroundWindow() != target.window || FAILED(field->SetFocus())) return false;
    for (int i = 0; i < 4 && GetForegroundWindow() == target.window; ++i) {
        if (IsTargetFocused(target)) return true;
        Sleep(25);
    }
    return IsTargetFocused(target);
}

bool InputInjectorWin::PasteConfirmation(const std::string& text, bool press_enter, bool use_current_field,
                                        const std::function<void()>& conceal) {
    const auto target = use_current_field ? ForegroundTarget() : target_;
    if (!target.window || text.empty()) return false;
    if (conceal) conceal();
    return PasteTo(target, text, press_enter && !use_current_field,
                   use_current_field ? FocusPolicy::kCurrentField : FocusPolicy::kOriginalField);
}

bool InputInjectorWin::PasteTo(const Target& target, const std::string& text, bool press_enter,
                               FocusPolicy focus) {
    const std::wstring wide = Utf16FromUtf8(text);
    const HWND destination = target.window;
    DWORD process_id = 0;
    if (wide.empty() || !GetWindowThreadProcessId(destination, &process_id) ||
        process_id != target.process || process_id == GetCurrentProcessId()) return false;
    if (focus == FocusPolicy::kOriginalField && !RestoreFocus(target)) return false;
    if (focus == FocusPolicy::kCurrentField && !IsTargetFocused(target)) return false;
    if (GetForegroundWindow() != destination) {
        if (focus != FocusPolicy::kWindow) return false;
        SetForegroundWindow(destination);
        if (GetForegroundWindow() != destination) return false;
    }

    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    void* buffer = GlobalLock(memory);
    if (!buffer) {
        GlobalFree(memory);
        return false;
    }
    memcpy(buffer, wide.c_str(), bytes);
    GlobalUnlock(memory);

    // Opening with a window associates it with the clipboard after EmptyClipboard.
    if (!OpenClipboard(clipboard_owner_)) {
        GlobalFree(memory);
        return false;
    }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!copied) {
        GlobalFree(memory);
        return false;
    }

    const auto still_focused = [&] {
        return focus == FocusPolicy::kWindow ? GetForegroundWindow() == destination : IsTargetFocused(target);
    };
    if (!still_focused()) return false;
    std::array paste{Key(VK_CONTROL), Key('V'), Key('V', KEYEVENTF_KEYUP),
                     Key(VK_CONTROL, KEYEVENTF_KEYUP)};
    if (SendInput(static_cast<UINT>(paste.size()), paste.data(), sizeof(INPUT)) != paste.size()) {
        return false;
    }
    if (press_enter) {
        Sleep(120);
        if (still_focused()) {
            std::array enter{Key(VK_RETURN), Key(VK_RETURN, KEYEVENTF_KEYUP)};
            SendInput(static_cast<UINT>(enter.size()), enter.data(), sizeof(INPUT));
        }
    }
    target_ = target;
    return true;
}

std::wstring InputInjectorWin::Utf16FromUtf8(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

} // namespace voicestick
