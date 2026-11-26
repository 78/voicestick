#pragma once

#include "app_config.h"

#include <Windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Gdiplus {
class Graphics;
class RectF;
}

namespace voicestick {

class OverlayWindow {
public:
    explicit OverlayWindow(HINSTANCE instance, HWND parent);
    ~OverlayWindow();

    void ShowListening(std::string output_mode);
    void ShowPartial(const std::string& text, std::string output_mode);
    void ShowFinalCountdown(const std::string& text, int duration_ms, std::function<void()> on_complete);
    void ShowPausedFinal(const std::string& text, bool supports_shake,
                         bool has_secondary_button, bool has_primary_button);
    std::optional<std::string> EditedConfirmationText() const;
    std::optional<std::string> ConfirmationDraftForSending();
    void ConcealConfirmation();
    void ShowPasteFailure();
    bool HandleDialogMessage(MSG* message);

    std::function<void()> on_send_confirmation;
    std::function<void()> on_cancel_confirmation;
    void ShowError(const std::string& text, std::function<void()> on_complete);
    void Hide(std::function<void()> on_hidden = {});
    void SetThemeColor(OverlayThemeColor color);
    void SetPosition(OverlayPosition position);

    HWND hwnd() const { return hwnd_; }
    void OnTimer(UINT_PTR timer_id);
    void OnPaint();
    void OnDpiChanged(UINT new_dpi, const RECT* suggested_rect);

private:
    enum class Mode { kListening, kCountdown, kPaused, kError, kHidden };

    void Show(Mode mode, const std::string& text, const std::string& output_mode = "");
    void Reposition();
    POINT TargetWindowOrigin(const RECT& work_area, int width, int height) const;
    int VisualOffsetX(int width, int visual_width) const;
    void SetSurfaceBounds(int x, int y, int width, int height);
    void PaintPreview(HDC dc);
    bool EnsureFrameBitmap(int width, int height);
    void ReleaseFrameBitmap();
    void InvalidateStaticLayer();
    void BuildStaticLayer(int width, int height);
    void RefreshDpi();
    int Dp(int px) const;
    float DpF(int px) const;
    void PaintShadow(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds);
    void PaintText(void* bits, int width, int height);
    void PaintIndicator(Gdiplus::Graphics& graphics, int x, int y, int size);

    bool CreateConfirmationControls();
    void SetConfirmationFonts();
    int ConfirmationButtonWidth(std::wstring_view title);
    int MinimumConfirmationWidth();
    void SetConfirmationMode(bool active);
    void LayoutConfirmationControls();
    void UpdateShadow(int x, int y, int width, int height);
    void UpdateConfirmationDraft();
    void PaintConfirmationButton(const DRAWITEMSTRUCT& item);
    COLORREF BackgroundColor() const;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    HWND shadow_window_ = nullptr;
    UINT shadow_dpi_ = 0;
    UINT surface_dpi_ = 0;
    HWND draft_edit_ = nullptr;
    HWND confirmation_status_ = nullptr;
    HWND send_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HFONT draft_font_ = nullptr;
    HFONT action_font_ = nullptr;
    HFONT status_font_ = nullptr;
    UINT confirmation_font_dpi_ = 0;
    bool confirmation_active_ = false;
    bool retrying_paste_ = false;
    Mode mode_ = Mode::kHidden;
    std::wstring text_;
    std::wstring output_mode_;
    int body_text_height_ = 0;
    float paused_countdown_progress_ = 1.0f;
    std::function<void()> pending_callback_;
    int largest_visible_height_ = 0;
    int target_window_width_ = 0;
    int target_window_height_ = 0;
    int target_window_x_ = 0;
    int target_window_y_ = 0;
    OverlayThemeColor theme_color_ = OverlayThemeColor::kWhite;
    OverlayPosition position_ = OverlayPosition::kCenter;
    ULONGLONG countdown_started_at_ms_ = 0;
    int countdown_duration_ms_ = 1000;
    UINT dpi_ = 96;
    bool should_wrap_text_ = false;
    HDC frame_dc_ = nullptr;
    HBITMAP frame_bitmap_ = nullptr;
    HGDIOBJ frame_old_bitmap_ = nullptr;
    void* frame_bits_ = nullptr;
    int frame_width_ = 0;
    int frame_height_ = 0;
    std::vector<BYTE> static_layer_bits_;
    bool static_layer_dirty_ = true;

    static constexpr UINT_PTR kAutoHideTimerId = 50;
    static constexpr UINT_PTR kAnimationTimerId = 52;
    static constexpr int kAnimationStepMs = 16;
    static constexpr int kCornerRadius = 24;
    static constexpr int kMinContentWidth = 300;
    static constexpr int kMinWidthLimit = 645;
    static constexpr int kMaxWidthLimit = 960;
    static constexpr int kMinContentHeight = 112;
    static constexpr int kHorizontalPadding = 32;
    static constexpr int kVerticalPadding = 24;
    static constexpr int kIndicatorSize = 34;
    static constexpr int kContentSpacing = 16;
    static constexpr int kTextFontSize = 30;
    static constexpr float kTextLineHeightMultiplier = 1.2f;
    static constexpr float kTextBaselineMultiplier = 0.92f;
    static constexpr int kMultilineGap = 4;
    static constexpr int kDraftLineSpacing = 22; // Twentieths of a line.
    static constexpr int kOutputModeFontSize = 10;
    static constexpr int kConfirmationFooterHeight = 50;
    static constexpr int kPasteFailureFooterHeight = 68;
    static constexpr BYTE kBackgroundAlpha = 242;
    static constexpr BYTE kInkRgb = 0;
    static constexpr BYTE kTextAlpha = 173;
    static constexpr BYTE kOutputModeAlpha = 92;
    static constexpr BYTE kIndicatorAlpha = 87;
    static constexpr BYTE kIndicatorTrackAlpha = 45;
    static constexpr int kShadowPadding = 12;
    static constexpr int kShadowBlur = 11;
    static constexpr int kShadowYOffset = 2;
    static constexpr int kPositionMargin = 28;
    static constexpr int kMaxAlpha = 255;
};

} // namespace voicestick
