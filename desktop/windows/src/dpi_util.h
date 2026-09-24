#pragma once

#include <Windows.h>
#include <ShellScalingApi.h>

#pragma comment(lib, "Shcore.lib")

namespace voicestick {

inline UINT GetDpiForHwnd(HWND hwnd) {
    if (hwnd) {
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi != 0) return dpi;
    }
    HDC dc = GetDC(nullptr);
    UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return dpi != 0 ? dpi : 96;
}

inline int ScalePx(int px, UINT dpi) {
    return MulDiv(px, static_cast<int>(dpi), 96);
}

inline float ScaleF(int px, UINT dpi) {
    return static_cast<float>(ScalePx(px, dpi));
}

// Per-Monitor DPI-aware work area for the monitor that contains `hwnd`.
// Falls back to the primary monitor work area if the monitor cannot be determined.
inline RECT GetWorkAreaForWindow(HWND hwnd) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        return mi.rcWork;
    }
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

inline HFONT CreateUiFont(UINT dpi) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    return reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

} // namespace voicestick
