#pragma once

#include "app_config.h"

#include <Windows.h>

#include <map>
#include <string>
#include <vector>

namespace voicestick {

class SubtitleWindow {
public:
    explicit SubtitleWindow(HINSTANCE instance, HWND parent);
    ~SubtitleWindow();

    void ShowSubtitle(const std::string& text, const std::string& device_id, OverlayThemeColor color);
    void HideAll();

private:
    struct Lane {
        std::wstring text;
        OverlayThemeColor color = OverlayThemeColor::kWhite;
        int generation = 0;
    };

    // Per-lane resolved layout in window-local coordinates.
    struct LaneLayout {
        std::string device_id;
        const Lane* lane = nullptr;
        float font_size = 0.0f;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    // Resolved window placement and per-lane layouts. Computed once per render
    // so that window sizing and bitmap painting always agree.
    struct WindowLayout {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        std::vector<LaneLayout> lanes;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    void Render();
    WindowLayout ComputeLayout() const;
    void UpdateLayeredBitmap(const WindowLayout& layout);
    bool EnsureFrameBitmap(int width, int height);
    void ReleaseFrameBitmap();
    int Dp(int px) const;
    float DpF(int px) const;
    void RefreshDpi();
    unsigned int ColorValue(OverlayThemeColor color) const;
    static std::wstring Utf16(std::string_view text);
    static std::string NormalizeSubtitleText(std::string_view text);

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    std::map<std::string, Lane> lanes_;
    int generation_ = 0;
    HDC frame_dc_ = nullptr;
    HBITMAP frame_bitmap_ = nullptr;
    HGDIOBJ frame_old_bitmap_ = nullptr;
    void* frame_bits_ = nullptr;
    int frame_width_ = 0;
    int frame_height_ = 0;

    static constexpr UINT_PTR kHideTimerBase = 4000;
    static constexpr int kHoldMs = 7000;

    // Visual constants (in device-independent units, scaled by Dp()).
    // Window-relative paddings.
    static constexpr int kWindowSidePadding = 8;     // gap between window edge and lane edge (left/right)
    static constexpr int kWindowEdgePadding = 8;     // gap between window edge and lane edge (top/bottom)
    static constexpr int kLaneGap = 10;              // vertical gap between adjacent lanes
    static constexpr int kBottomScreenMargin = 32;   // gap between window and bottom of work area

    // Lane-internal layout. Text occupies the area inset by these values from the lane rect.
    static constexpr int kLaneTextLeftInset = 98;    // lane.left -> text.left
    static constexpr int kLaneTextRightInset = 26;   // text.right -> lane.right
    static constexpr int kLaneTextVerticalInset = 18; // both top and bottom inset of the text rect inside the lane

    // Decorations inside the lane.
    static constexpr int kColorBarOffset = 12;       // lane.left -> color bar.left
    static constexpr int kColorBarWidth = 6;
    static constexpr int kColorBarVerticalInset = 14;
    static constexpr int kColorBarRadius = 3;
    static constexpr int kLaneCornerRadius = 14;
    static constexpr int kDeviceLabelOffset = 30;    // lane.left -> device label.left
    static constexpr int kDeviceLabelWidth = 52;

    // Lane sizing bounds and font sizes.
    static constexpr int kMinLaneWidth = 520;
    static constexpr int kMinLaneHeight = 92;
    static constexpr int kTextFontSize = 64;
    static constexpr int kMinTextFontSize = 44;
    static constexpr int kDeviceFontSize = 14;
};

} // namespace voicestick
