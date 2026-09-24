#include "overlay_window.h"
#include "dpi_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d2d1.h>
#include <dwrite.h>
#include <objidl.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dwrite.lib")
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Windowscodecs.lib")

namespace voicestick {

namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kOverlayFontFamily = L"Microsoft YaHei UI";
constexpr float kTextLayoutMaxHeight = 10000.0f;

struct TextLayoutSize {
    float width = 0.0f;
    float height = 0.0f;
    UINT32 line_count = 1;
};

struct BitmapRenderTarget {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    UINT stride = 0;
    UINT buffer_size = 0;
};

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

ULONG_PTR EnsureGdiplus() {
    static ULONG_PTR token = [] {
        ULONG_PTR startup_token = 0;
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&startup_token, &input, nullptr);
        return startup_token;
    }();
    return token;
}

void AddRoundedRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF rect, float radius) {
    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

IDWriteFactory* SharedDwriteFactory() {
    static ComPtr<IDWriteFactory> factory = [] {
        ComPtr<IDWriteFactory> created;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(created.GetAddressOf()));
        return created;
    }();
    return factory.Get();
}

ID2D1Factory* SharedD2dFactory() {
    static ComPtr<ID2D1Factory> factory = [] {
        ComPtr<ID2D1Factory> created;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, created.GetAddressOf());
        return created;
    }();
    return factory.Get();
}

IWICImagingFactory* SharedWicFactory() {
    static ComPtr<IWICImagingFactory> factory = [] {
        ComPtr<IWICImagingFactory> created;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&created));
        return created;
    }();
    return factory.Get();
}

ComPtr<IDWriteTextFormat> CreateTextFormat(float font_size,
                                           DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
    ComPtr<IDWriteTextFormat> format;
    auto* factory = SharedDwriteFactory();
    if (!factory) return format;

    if (FAILED(factory->CreateTextFormat(kOverlayFontFamily, nullptr, weight,
                                         DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL,
                                         font_size, L"zh-cn", &format))) {
        return {};
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetTrimming(nullptr, nullptr);
    return format;
}

ComPtr<IDWriteTextLayout> CreateTextLayout(const std::wstring& text,
                                           IDWriteTextFormat* format,
                                           float width,
                                           bool should_wrap,
                                           float line_spacing = 0.0f,
                                           float baseline = 0.0f) {
    ComPtr<IDWriteTextLayout> layout;
    auto* factory = SharedDwriteFactory();
    if (!factory || !format) return layout;

    const std::wstring measured = text.empty() ? std::wstring(L" ") : text;
    format->SetWordWrapping(should_wrap ? DWRITE_WORD_WRAPPING_WRAP
                                        : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (FAILED(factory->CreateTextLayout(measured.c_str(),
                                         static_cast<UINT32>(measured.size()),
                                         format,
                                         std::max(1.0f, width),
                                         kTextLayoutMaxHeight,
                                         &layout))) {
        return {};
    }
    if (line_spacing > 0.0f && baseline > 0.0f) {
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                               line_spacing,
                               std::min(baseline, line_spacing));
    }
    return layout;
}

TextLayoutSize MeasureText(const std::wstring& text, float font_size,
                           float width, bool should_wrap,
                           float line_spacing = 0.0f,
                           float baseline = 0.0f) {
    TextLayoutSize result;
    auto format = CreateTextFormat(font_size);
    auto layout = CreateTextLayout(text, format.Get(), width, should_wrap,
                                   line_spacing, baseline);
    if (!layout) {
        result.width = width;
        result.height = font_size;
        return result;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics))) {
        result.width = std::ceil(std::max(metrics.width, metrics.widthIncludingTrailingWhitespace));
        result.height = std::ceil(metrics.height);
        result.line_count = std::max<UINT32>(1, metrics.lineCount);
    }
    return result;
}

BitmapRenderTarget CreateBitmapRenderTarget(void* bits, int width, int height) {
    BitmapRenderTarget result;
    auto* d2d_factory = SharedD2dFactory();
    auto* wic_factory = SharedWicFactory();
    if (!d2d_factory || !wic_factory || !bits || width <= 0 || height <= 0) return result;

    result.stride = static_cast<UINT>(width * 4);
    result.buffer_size = static_cast<UINT>(result.stride * height);
    if (FAILED(wic_factory->CreateBitmapFromMemory(
            static_cast<UINT>(width), static_cast<UINT>(height),
            GUID_WICPixelFormat32bppPBGRA, result.stride, result.buffer_size,
            static_cast<BYTE*>(bits), &result.bitmap))) {
        return {};
    }

    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    if (FAILED(d2d_factory->CreateWicBitmapRenderTarget(result.bitmap.Get(), properties,
                                                        &result.target))) {
        return {};
    }
    return result;
}

void DrawTextLayout(ID2D1RenderTarget* target, IDWriteTextLayout* layout,
                    float x, float y, BYTE alpha, BYTE rgb) {
    if (!target || !layout) return;

    const float c = static_cast<float>(rgb) / 255.0f;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(c, c, c, static_cast<float>(alpha) / 255.0f), &brush))) {
        return;
    }
    target->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

} // namespace

OverlayWindow::OverlayWindow(HINSTANCE instance, HWND parent)
    : instance_(instance), parent_(parent) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayWindow::WndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"VoiceStickOverlayV2";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1, 1, parent_, nullptr, instance_, this);
    EnsureGdiplus();
}

OverlayWindow::~OverlayWindow() {
    if (hwnd_) {
        KillTimer(hwnd_, kAutoHideTimerId);
        KillTimer(hwnd_, kFadeTimerId);
        KillTimer(hwnd_, kAnimationTimerId);
        DestroyWindow(hwnd_);
    }
    ReleaseFrameBitmap();
}

void OverlayWindow::ShowListening() {
    Show(Mode::kListening, "Listening...");
}

void OverlayWindow::ShowPartial(const std::string& text) {
    Show(Mode::kListening, text.empty() ? "Processing..." : text);
}

void OverlayWindow::ShowFinalCountdown(const std::string& text, std::function<void()> on_complete) {
    countdown_duration_ms_ = 1200;
    countdown_started_at_ms_ = GetTickCount64();
    Show(Mode::kCountdown, text);
    pending_callback_ = std::move(on_complete);
    SetTimer(hwnd_, kAutoHideTimerId, countdown_duration_ms_, nullptr);
}

void OverlayWindow::ShowPausedFinal(const std::string& text) {
    Show(Mode::kPaused, text, "Front: Send    Side: Cancel");
}

void OverlayWindow::ShowError(const std::string& text, std::function<void()> on_complete) {
    Show(Mode::kError, text.empty() ? "ASR Error" : text);
    pending_callback_ = std::move(on_complete);
    SetTimer(hwnd_, kAutoHideTimerId, 2000, nullptr);
}

void OverlayWindow::SetTranslationEnabled(bool enabled) {
    if (translation_enabled_ == enabled) return;
    translation_enabled_ = enabled;
    InvalidateStaticLayer();
    if (mode_ != Mode::kHidden) UpdateLayeredBitmap();
}

void OverlayWindow::SetThemeColor(OverlayThemeColor color) {
    if (theme_color_ == color) return;
    theme_color_ = color;
    InvalidateStaticLayer();
    if (mode_ != Mode::kHidden) UpdateLayeredBitmap();
}

void OverlayWindow::SetPosition(OverlayPosition position) {
    if (position_ == position) return;
    position_ = position;
    if (mode_ != Mode::kHidden) Reposition();
}

void OverlayWindow::Hide(std::function<void()> on_hidden) {
    KillTimer(hwnd_, kAutoHideTimerId);
    KillTimer(hwnd_, kAnimationTimerId);
    auto completion = on_hidden ? std::move(on_hidden) : std::move(pending_callback_);
    pending_callback_ = std::move(completion);
    StartFadeOut();
}

void OverlayWindow::OnTimer(UINT_PTR timer_id) {
    if (timer_id == kAutoHideTimerId) {
        KillTimer(hwnd_, kAutoHideTimerId);
        Hide();
    } else if (timer_id == kAnimationTimerId) {
        animation_frame_++;
        const bool window_moved = StepWindowAnimation();
        UpdateLayeredBitmap();
        if (!window_moved && mode_ != Mode::kListening && mode_ != Mode::kCountdown) {
            KillTimer(hwnd_, kAnimationTimerId);
        }
    }
}

void OverlayWindow::OnPaint() {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    EndPaint(hwnd_, &ps);
    UpdateLayeredBitmap();
}

void OverlayWindow::Show(Mode mode, const std::string& text, const std::string& hint) {
    KillTimer(hwnd_, kAutoHideTimerId);
    mode_ = mode;
    text_ = Utf16FromUtf8(text);
    hint_ = hint.empty() ? std::wstring() : Utf16FromUtf8(hint);
    InvalidateStaticLayer();
    Reposition();

    if (mode == Mode::kListening || mode == Mode::kCountdown || NeedsWindowAnimation()) {
        SetTimer(hwnd_, kAnimationTimerId, kAnimationStepMs, nullptr);
    } else {
        KillTimer(hwnd_, kAnimationTimerId);
    }

    current_alpha_ = kMaxAlpha;
    target_alpha_ = kMaxAlpha;
    KillTimer(hwnd_, kFadeTimerId);
    UpdateLayeredBitmap();
}

void OverlayWindow::Reposition() {
    RefreshDpi();

    RECT work_area = GetWorkAreaForWindow(parent_ ? parent_ : hwnd_);
    const int screen_w = work_area.right - work_area.left;
    const int screen_h = work_area.bottom - work_area.top;

    const int shadow_padding = Dp(kShadowPadding);
    const int horizontal_padding = Dp(kHorizontalPadding);
    const int vertical_padding = Dp(kVerticalPadding);
    const int indicator_size = Dp(kIndicatorSize);
    const int content_spacing = Dp(kContentSpacing);
    const int min_content_height = Dp(kMinContentHeight);
    const int side_chrome_width = horizontal_padding + indicator_size +
                                  content_spacing + horizontal_padding;

    // Same layout algorithm for all modes, mirroring the macOS implementation.
    const int available_max_width = std::min(Dp(kMaxContentWidth),
                                             screen_w - Dp(48) - shadow_padding * 2);
    const int max_text_width = std::max(1, available_max_width - side_chrome_width);

    const float text_font_size = DpF(kTextFontSize);
    const float text_line_height = text_font_size * kTextLineHeightMultiplier;
    const float text_baseline = text_font_size * kTextBaselineMultiplier;
    const auto single_line_text = MeasureText(text_, text_font_size,
                                              static_cast<float>(max_text_width) * 4.0f,
                                              false, text_line_height, text_baseline);
    const float measured_text_width = single_line_text.width;
    const float desired_text_width = std::min(measured_text_width,
                                              static_cast<float>(max_text_width));
    const int one_third_text_width = std::max(1, max_text_width / 3);
    const int two_thirds_text_width = std::max(one_third_text_width,
                                               (max_text_width * 2) / 3);
    int text_width = max_text_width;
    if (desired_text_width <= static_cast<float>(one_third_text_width)) {
        text_width = one_third_text_width;
    } else if (desired_text_width <= static_cast<float>(two_thirds_text_width)) {
        text_width = two_thirds_text_width;
    }
    int content_width = text_width + side_chrome_width;

    should_wrap_text_ = measured_text_width > static_cast<float>(max_text_width);
    const auto laid_out_text = should_wrap_text_
        ? MeasureText(text_, text_font_size, static_cast<float>(text_width), true,
                      text_line_height, text_baseline)
        : single_line_text;
    const float text_height = laid_out_text.height;
    const float hint_height = hint_.empty()
        ? 0.0f
        : MeasureText(hint_, DpF(kHintFontSize), static_cast<float>(text_width), false).height +
              DpF(8);

    const int max_content_height = std::max(min_content_height,
                                            screen_h - Dp(120) - shadow_padding * 2);
    const int desired_content_height = static_cast<int>(std::ceil(
        vertical_padding * 2 + std::max(text_height + hint_height,
                                        static_cast<float>(indicator_size))));
    int content_height = std::min(max_content_height,
                                  std::max(min_content_height, desired_content_height));

    // Width and height only grow while visible (reset on hide).
    content_width = std::max(content_width, largest_visible_width_);
    content_height = std::max(content_height, largest_visible_height_);
    largest_visible_width_ = content_width;
    largest_visible_height_ = content_height;

    target_window_width_ = content_width + shadow_padding * 2;
    target_window_height_ = content_height + shadow_padding * 2;
    const POINT origin = TargetWindowOrigin(work_area, target_window_width_, target_window_height_);
    target_window_x_ = origin.x;
    target_window_y_ = origin.y;

    animated_window_width_ = target_window_width_;
    animated_window_height_ = target_window_height_;

    UpdateLayeredBitmap();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

bool OverlayWindow::NeedsWindowAnimation() const {
    return animated_window_width_ != target_window_width_ ||
           animated_window_height_ != target_window_height_;
}

bool OverlayWindow::StepWindowAnimation() {
    if (!NeedsWindowAnimation()) return false;

    auto step = [this](int current, int target, int step_size) {
        const int delta = target - current;
        const int distance = std::abs(delta);
        if (distance <= Dp(kWindowResizeSnap)) return target;
        const int direction = delta > 0 ? 1 : -1;
        return current + direction * std::min(distance, Dp(step_size));
    };

    animated_window_width_ = step(animated_window_width_, target_window_width_,
                                  kWindowWidthResizeStep);
    animated_window_height_ = step(animated_window_height_, target_window_height_,
                                   kWindowHeightResizeStep);
    InvalidateStaticLayer();
    ApplyAnimatedWindowBounds();
    return true;
}

void OverlayWindow::ApplyAnimatedWindowBounds() {
    UpdateLayeredBitmap();
}

POINT OverlayWindow::TargetWindowOrigin(const RECT& work_area, int width, int height) const {
    const int margin = Dp(kPositionMargin);
    const int screen_w = work_area.right - work_area.left;
    const int screen_h = work_area.bottom - work_area.top;
    switch (position_) {
    case OverlayPosition::kTopLeft:
        return POINT{work_area.left + margin, work_area.top + margin};
    case OverlayPosition::kTopRight:
        return POINT{work_area.right - margin - width, work_area.top + margin};
    case OverlayPosition::kBottomLeft:
        return POINT{work_area.left + margin, work_area.bottom - margin - height};
    case OverlayPosition::kBottomRight:
        return POINT{work_area.right - margin - width, work_area.bottom - margin - height};
    case OverlayPosition::kCenter:
    default:
        return POINT{work_area.left + (screen_w - width) / 2,
                     work_area.top + (screen_h - height) / 2};
    }
}

int OverlayWindow::VisualOffsetX(int width, int visual_width) const {
    switch (position_) {
    case OverlayPosition::kTopLeft:
    case OverlayPosition::kBottomLeft:
        return 0;
    case OverlayPosition::kTopRight:
    case OverlayPosition::kBottomRight:
        return width - visual_width;
    case OverlayPosition::kCenter:
    default:
        return (width - visual_width) / 2;
    }
}

int OverlayWindow::VisualOffsetY(int height, int visual_height) const {
    switch (position_) {
    case OverlayPosition::kBottomLeft:
    case OverlayPosition::kBottomRight:
        return height - visual_height;
    case OverlayPosition::kTopLeft:
    case OverlayPosition::kTopRight:
        return 0;
    case OverlayPosition::kCenter:
    default:
        return (height - visual_height) / 2;
    }
}

void OverlayWindow::RefreshDpi() {
    dpi_ = GetDpiForHwnd(parent_ ? parent_ : hwnd_);
}

int OverlayWindow::Dp(int px) const {
    return voicestick::ScalePx(px, dpi_);
}

float OverlayWindow::DpF(int px) const {
    return ScaleF(px, dpi_);
}

void OverlayWindow::UpdateLayeredBitmap() {
    RECT window_rect{};
    GetWindowRect(hwnd_, &window_rect);
    const int width = target_window_width_ > 0
        ? target_window_width_
        : window_rect.right - window_rect.left;
    const int height = target_window_height_ > 0
        ? target_window_height_
        : window_rect.bottom - window_rect.top;
    if (width <= 0 || height <= 0) return;
    if (!EnsureFrameBitmap(width, height)) return;

    if (static_layer_dirty_ ||
        static_cast<int>(static_layer_bits_.size()) != width * height * 4) {
        BuildStaticLayer(width, height);
    } else {
        std::memcpy(frame_bits_, static_layer_bits_.data(), static_layer_bits_.size());
    }

    HDC screen_dc = GetDC(nullptr);
    {
        Gdiplus::Graphics graphics(frame_dc_);
        graphics.SetPageUnit(Gdiplus::UnitPixel);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const int shadow_padding = Dp(kShadowPadding);
        const int horizontal_padding = Dp(kHorizontalPadding);
        const int indicator_size = Dp(kIndicatorSize);
        const int visual_width = std::clamp(animated_window_width_, 1, width);
        const int visual_height = std::clamp(animated_window_height_, 1, height);
        const int visual_x = VisualOffsetX(width, visual_width);
        const int visual_y = VisualOffsetY(height, visual_height);
        const int content_height = visual_height - shadow_padding * 2;
        const int indicator_x = visual_x + shadow_padding + horizontal_padding;
        const int indicator_y = visual_y + shadow_padding + (content_height - indicator_size) / 2;
        PaintIndicator(graphics, indicator_x, indicator_y, indicator_size);
    }

    const int destination_x = target_window_width_ > 0 ? target_window_x_ : window_rect.left;
    const int destination_y = target_window_height_ > 0 ? target_window_y_ : window_rect.top;
    POINT destination = {destination_x, destination_y};
    POINT source = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, static_cast<BYTE>(current_alpha_), AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen_dc, &destination, &size, frame_dc_, &source, 0, &blend, ULW_ALPHA);

    ReleaseDC(nullptr, screen_dc);
}

bool OverlayWindow::EnsureFrameBitmap(int width, int height) {
    if (frame_dc_ && frame_bitmap_ && frame_width_ == width && frame_height_ == height) {
        return true;
    }

    ReleaseFrameBitmap();

    HDC screen_dc = GetDC(nullptr);
    frame_dc_ = CreateCompatibleDC(screen_dc);

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    frame_bitmap_ = CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS,
                                     &frame_bits_, nullptr, 0);
    ReleaseDC(nullptr, screen_dc);

    if (!frame_dc_ || !frame_bitmap_ || !frame_bits_) {
        ReleaseFrameBitmap();
        return false;
    }

    frame_old_bitmap_ = SelectObject(frame_dc_, frame_bitmap_);
    frame_width_ = width;
    frame_height_ = height;
    InvalidateStaticLayer();
    return true;
}

void OverlayWindow::ReleaseFrameBitmap() {
    if (frame_dc_ && frame_old_bitmap_) {
        SelectObject(frame_dc_, frame_old_bitmap_);
    }
    if (frame_bitmap_) {
        DeleteObject(frame_bitmap_);
    }
    if (frame_dc_) {
        DeleteDC(frame_dc_);
    }
    frame_dc_ = nullptr;
    frame_bitmap_ = nullptr;
    frame_old_bitmap_ = nullptr;
    frame_bits_ = nullptr;
    frame_width_ = 0;
    frame_height_ = 0;
    static_layer_bits_.clear();
    static_layer_dirty_ = true;
}

void OverlayWindow::InvalidateStaticLayer() {
    static_layer_dirty_ = true;
}

void OverlayWindow::BuildStaticLayer(int width, int height) {
    const std::size_t byte_count = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4;
    static_layer_bits_.assign(byte_count, 0);
    std::memset(frame_bits_, 0, byte_count);

    {
        Gdiplus::Graphics graphics(frame_dc_);
        graphics.SetPageUnit(Gdiplus::UnitPixel);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        PaintContent(graphics, width, height);
    }
    PaintText(frame_bits_, width, height);
    std::memcpy(static_layer_bits_.data(), frame_bits_, byte_count);
    static_layer_dirty_ = false;
}

void OverlayWindow::PaintContent(Gdiplus::Graphics& graphics, int width, int height) {
    const int shadow_padding = Dp(kShadowPadding);
    const int shadow_blur = Dp(kShadowBlur);
    const int shadow_y_offset = Dp(kShadowYOffset);
    const int corner_radius = Dp(kCornerRadius);
    const int visual_width = std::clamp(animated_window_width_, 1, width);
    const int visual_height = std::clamp(animated_window_height_, 1, height);
    const int visual_x = VisualOffsetX(width, visual_width);
    const int visual_y = VisualOffsetY(height, visual_height);

    Gdiplus::RectF background_rect(static_cast<float>(visual_x + shadow_padding) + 0.5f,
                                   static_cast<float>(visual_y + shadow_padding) + 0.5f,
                                   static_cast<float>(visual_width - shadow_padding * 2) - 1.0f,
                                   static_cast<float>(visual_height - shadow_padding * 2) - 1.0f);

    for (int layer = shadow_blur; layer >= 1; --layer) {
        const float spread = static_cast<float>(layer);
        const BYTE alpha = static_cast<BYTE>(2 + (shadow_blur - layer));
        Gdiplus::RectF shadow_rect(background_rect.X - spread,
                                   background_rect.Y - spread + static_cast<float>(shadow_y_offset),
                                   background_rect.Width + spread * 2.0f,
                                   background_rect.Height + spread * 2.0f);
        Gdiplus::GraphicsPath shadow_path;
        AddRoundedRect(shadow_path, shadow_rect, static_cast<float>(corner_radius) + spread);
        Gdiplus::SolidBrush shadow_brush(Gdiplus::Color(alpha, 0, 0, 0));
        graphics.FillPath(&shadow_brush, &shadow_path);
    }

    Gdiplus::GraphicsPath background_path;
    AddRoundedRect(background_path, background_rect, static_cast<float>(corner_radius));

    BYTE red = 252;
    BYTE green = 252;
    BYTE blue = 252;
    switch (theme_color_) {
    case OverlayThemeColor::kPink:
        red = 255; green = 214; blue = 230;
        break;
    case OverlayThemeColor::kGreen:
        red = 214; green = 242; blue = 214;
        break;
    case OverlayThemeColor::kYellow:
        red = 255; green = 240; blue = 184;
        break;
    case OverlayThemeColor::kBlue:
        red = 209; green = 232; blue = 255;
        break;
    case OverlayThemeColor::kPurple:
        red = 230; green = 214; blue = 255;
        break;
    case OverlayThemeColor::kWhite:
    default:
        break;
    }
    Gdiplus::SolidBrush background_brush(Gdiplus::Color(kBackgroundAlpha, red, green, blue));
    graphics.FillPath(&background_brush, &background_path);

    Gdiplus::Pen border_pen(Gdiplus::Color(36, 225, 225, 225),
                            std::max(1.0f, DpF(1)));
    graphics.DrawPath(&border_pen, &background_path);
}

void OverlayWindow::PaintText(void* bits, int width, int height) {
    const int shadow_padding = Dp(kShadowPadding);
    const int horizontal_padding = Dp(kHorizontalPadding);
    const int indicator_size = Dp(kIndicatorSize);
    const int content_spacing = Dp(kContentSpacing);
    const int visual_width = std::clamp(animated_window_width_, 1, width);
    const int visual_height = std::clamp(animated_window_height_, 1, height);
    const int visual_x = VisualOffsetX(width, visual_width);
    const int visual_y = VisualOffsetY(height, visual_height);
    const int indicator_x = visual_x + shadow_padding + horizontal_padding;
    const float text_x = static_cast<float>(indicator_x + indicator_size + content_spacing);
    const float text_width = static_cast<float>(visual_width - shadow_padding * 2 -
                                                horizontal_padding * 2 -
                                                indicator_size - content_spacing);

    const float text_font_size = DpF(kTextFontSize);
    const float text_line_height = text_font_size * kTextLineHeightMultiplier;
    const float text_baseline = text_font_size * kTextBaselineMultiplier;
    auto text_format = CreateTextFormat(text_font_size);
    auto text_layout = CreateTextLayout(text_, text_format.Get(), text_width,
                                        should_wrap_text_, text_line_height,
                                        text_baseline);
    TextLayoutSize text_metrics;
    if (text_layout) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(text_layout->GetMetrics(&metrics))) {
            text_metrics.width =
                std::ceil(std::max(metrics.width, metrics.widthIncludingTrailingWhitespace));
            text_metrics.height = std::ceil(metrics.height);
            text_metrics.line_count = std::max<UINT32>(1, metrics.lineCount);
        }
    }

    TextLayoutSize hint_metrics;
    ComPtr<IDWriteTextLayout> hint_layout;
    if (!hint_.empty()) {
        auto hint_format = CreateTextFormat(DpF(kHintFontSize), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        hint_layout = CreateTextLayout(hint_, hint_format.Get(), text_width, false);
        if (hint_layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(hint_layout->GetMetrics(&metrics))) {
                hint_metrics.height = std::ceil(metrics.height);
            }
        }
    }

    const float gap = hint_.empty() ? 0.0f : DpF(8);
    const float block_height = text_metrics.height + gap + hint_metrics.height;
    const float block_y = static_cast<float>(visual_y + shadow_padding) +
                          std::max(0.0f, (static_cast<float>(visual_height - shadow_padding * 2) - block_height) / 2.0f);

    auto render_target = CreateBitmapRenderTarget(bits, width, height);
    if (!render_target.target || !render_target.bitmap) return;
    const D2D1_RECT_F text_clip = D2D1::RectF(
        static_cast<float>(visual_x + shadow_padding),
        static_cast<float>(visual_y + shadow_padding),
        static_cast<float>(visual_x + visual_width - shadow_padding),
        static_cast<float>(visual_y + visual_height - shadow_padding));
    render_target.target->BeginDraw();
    render_target.target->PushAxisAlignedClip(text_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    DrawTextLayout(render_target.target.Get(), text_layout.Get(), text_x, block_y,
                   kTextAlpha, kInkRgb);
    DrawTextLayout(render_target.target.Get(), hint_layout.Get(), text_x,
                   block_y + text_metrics.height + gap, kHintAlpha, kInkRgb);
    render_target.target->PopAxisAlignedClip();
    if (SUCCEEDED(render_target.target->EndDraw())) {
        render_target.bitmap->CopyPixels(nullptr, render_target.stride,
                                         render_target.buffer_size,
                                         static_cast<BYTE*>(bits));
    }
}

void OverlayWindow::PaintIndicator(Gdiplus::Graphics& graphics, int x, int y, int size) {
    const int cx = x + size / 2;
    const int cy = y + size / 2;

    if (mode_ == Mode::kListening) {
        const int bar_width = Dp(4);
        const int spacing = Dp(5);
        const int num_bars = 3;
        const int total_w = num_bars * bar_width + (num_bars - 1) * spacing;
        int start_x = cx - total_w / 2;
        const double elapsed = static_cast<double>(GetTickCount64() % 100000) / 1000.0;

        Gdiplus::SolidBrush bar_brush(Gdiplus::Color(kIndicatorAlpha, kInkRgb, kInkRgb, kInkRgb));
        for (int i = 0; i < num_bars; ++i) {
            const double phase = elapsed * 5.5 + i * 0.85;
            const int bar_h = Dp(9) + static_cast<int>(Dp(10) * (0.5 + 0.5 * std::sin(phase)));
            int bx = start_x + i * (bar_width + spacing);
            int by = cy - bar_h / 2;
            graphics.FillRectangle(&bar_brush, bx, by, bar_width, bar_h);
        }
        if (translation_enabled_) {
            const int t_size = Dp(14);
            const int bx = start_x + total_w + Dp(5);
            const int by = cy - t_size / 2;
            Gdiplus::Pen t_pen(Gdiplus::Color(200, kInkRgb, kInkRgb, kInkRgb), DpF(3));
            t_pen.SetStartCap(Gdiplus::LineCapRound);
            t_pen.SetEndCap(Gdiplus::LineCapRound);
            graphics.DrawLine(&t_pen, bx, by, bx + t_size, by);
            graphics.DrawLine(&t_pen, bx + t_size / 2, by, bx + t_size / 2, by + t_size);
        }
    } else if (mode_ == Mode::kCountdown) {
        Gdiplus::Pen track_pen(Gdiplus::Color(kIndicatorTrackAlpha, kInkRgb,
                                              kInkRgb, kInkRgb), DpF(3));
        Gdiplus::Pen ring_pen(Gdiplus::Color(kIndicatorAlpha, kInkRgb, kInkRgb, kInkRgb), DpF(3));
        ring_pen.SetStartCap(Gdiplus::LineCapRound);
        ring_pen.SetEndCap(Gdiplus::LineCapRound);
        const int inset = Dp(5);
        Gdiplus::RectF ring_rect(static_cast<Gdiplus::REAL>(x + inset),
                                 static_cast<Gdiplus::REAL>(y + inset),
                                 static_cast<Gdiplus::REAL>(size - inset * 2),
                                 static_cast<Gdiplus::REAL>(size - inset * 2));
        graphics.DrawEllipse(&track_pen, ring_rect);

        const ULONGLONG elapsed_ms = countdown_started_at_ms_ == 0
            ? 0
            : GetTickCount64() - countdown_started_at_ms_;
        const float remaining = std::clamp(
            1.0f - static_cast<float>(elapsed_ms) / static_cast<float>(countdown_duration_ms_),
            0.0f, 1.0f);
        if (remaining > 0.0f) {
            graphics.DrawArc(&ring_pen, ring_rect, -90.0f, -360.0f * remaining);
        }
    } else if (mode_ == Mode::kPaused) {
        Gdiplus::Pen ring_pen(Gdiplus::Color(kIndicatorAlpha, kInkRgb, kInkRgb, kInkRgb), DpF(3));
        const int inset = Dp(5);
        graphics.DrawEllipse(&ring_pen, x + inset, y + inset, size - inset * 2, size - inset * 2);
    } else if (mode_ == Mode::kError) {
        Gdiplus::Pen ring_pen(Gdiplus::Color(255, 200, 60, 60), DpF(3));
        const int inset = Dp(5);
        graphics.DrawEllipse(&ring_pen, x + inset, y + inset, size - inset * 2, size - inset * 2);

        const int x_inset = size / 3;
        graphics.DrawLine(&ring_pen, x + x_inset, y + x_inset,
                          x + size - x_inset, y + size - x_inset);
        graphics.DrawLine(&ring_pen, x + size - x_inset, y + x_inset,
                          x + x_inset, y + size - x_inset);
    }
}

void OverlayWindow::StartFadeIn() {
    current_alpha_ = kMaxAlpha;
    target_alpha_ = kMaxAlpha;
    KillTimer(hwnd_, kFadeTimerId);
    UpdateLayeredBitmap();
}

void OverlayWindow::StartFadeOut() {
    KillTimer(hwnd_, kFadeTimerId);
    current_alpha_ = 0;
    target_alpha_ = 0;
    UpdateLayeredBitmap();
    ShowWindow(hwnd_, SW_HIDE);
    mode_ = Mode::kHidden;
    largest_visible_width_ = 0;
    largest_visible_height_ = 0;
    animated_window_width_ = 0;
    animated_window_height_ = 0;
    target_window_width_ = 0;
    target_window_height_ = 0;
    target_window_x_ = 0;
    target_window_y_ = 0;
    ReleaseFrameBitmap();
    auto callback = std::move(pending_callback_);
    pending_callback_ = {};
    if (callback) callback();
}

void OverlayWindow::OnDpiChanged(UINT new_dpi, const RECT* suggested_rect) {
    dpi_ = new_dpi != 0 ? new_dpi : 96;
    ReleaseFrameBitmap();
    if (mode_ != Mode::kHidden) {
        largest_visible_width_ = 0;
        largest_visible_height_ = 0;
        animated_window_width_ = 0;
        animated_window_height_ = 0;
        Reposition();
    } else if (suggested_rect) {
        SetWindowPos(hwnd_, nullptr, suggested_rect->left, suggested_rect->top,
                     suggested_rect->right - suggested_rect->left,
                     suggested_rect->bottom - suggested_rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<OverlayWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT:
        self->OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        self->OnTimer(static_cast<UINT_PTR>(wp));
        return 0;
    case WM_DPICHANGED: {
        UINT new_dpi = HIWORD(wp);
        auto* rect = reinterpret_cast<const RECT*>(lp);
        self->OnDpiChanged(new_dpi, rect);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace voicestick
