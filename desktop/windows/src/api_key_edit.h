#pragma once

#include <Windows.h>
#include <CommCtrl.h>

#include <algorithm>
#include <string>

namespace voicestick {

inline LRESULT CALLBACK PasteApiKey(HWND edit, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_CHAR && wp == 0x16) return 0;
    const bool paste_key = message == WM_KEYDOWN &&
        ((wp == 'V' && GetKeyState(VK_CONTROL) < 0) || (wp == VK_INSERT && GetKeyState(VK_SHIFT) < 0));
    if (message != WM_PASTE && !paste_key) return DefSubclassProc(edit, message, wp, lp);

    if (!OpenClipboard(edit)) return 0;
    std::wstring key;
    if (const auto clipboard = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* data = static_cast<const wchar_t*>(GlobalLock(clipboard))) {
            const auto* end = data + GlobalSize(clipboard) / sizeof(wchar_t);
            key.assign(data, std::find(data, end, L'\0'));
            GlobalUnlock(clipboard);
        }
    }
    CloseClipboard();
    // A wrapped API key is still one credential.
    std::erase_if(key, [](wchar_t ch) {
        WORD type = 0;
        GetStringTypeW(CT_CTYPE1, &ch, 1, &type);
        return (type & C1_SPACE) != 0;
    });
    if (!key.empty()) SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(key.c_str()));
    return 0;
}

} // namespace voicestick
