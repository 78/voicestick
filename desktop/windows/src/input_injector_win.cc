#include "input_injector_win.h"

#include <vector>

namespace voicestick {

void InputInjectorWin::Paste(const std::string& text, bool press_enter) {
    if (text.empty()) return;
    const std::wstring wide = Utf16FromUtf8(text);
    if (wide.empty()) return;

    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            void* target = GlobalLock(memory);
            if (target != nullptr) {
                memcpy(target, wide.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
                memory = nullptr;
            }
            if (memory != nullptr) GlobalFree(memory);
        }
        CloseClipboard();
    }

    Sleep(40);
    SendCtrlV();
    if (press_enter) {
        Sleep(120);
        SendEnter();
    }
}

std::wstring InputInjectorWin::Utf16FromUtf8(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

void InputInjectorWin::SendKey(WORD virtual_key, bool key_down, DWORD flags) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtual_key;
    input.ki.dwFlags = flags | (key_down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(INPUT));
}

void InputInjectorWin::SendCtrlV() {
    SendKey(VK_CONTROL, true);
    SendKey('V', true);
    SendKey('V', false);
    SendKey(VK_CONTROL, false);
}

void InputInjectorWin::SendEnter() {
    SendKey(VK_RETURN, true);
    SendKey(VK_RETURN, false);
}

} // namespace voicestick
