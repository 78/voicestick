#include "subtitle_window.h"

#include "ble_protocol.h"
#include "dpi_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace voicestick {

namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kSubtitleFontFamily = L"Microsoft YaHei UI";
constexpr float kTextLayoutMaxHeight = 10000.0f;

struct TextLayoutSize {
    float width = 0.0f;
    float height = 0.0f;
    UINT32 line_count = 1;
};

struct FittedText {
    float font_size = 0.0f;
    TextLayoutSize metrics;
};

struct BitmapRenderTarget {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    UINT stride = 0;
    UINT buffer_size = 0;
};

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
                                           DWRITE_FONT_WEIGHT weight,
                                           DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment) {
    ComPtr<IDWriteTextFormat> format;
    auto* factory = SharedDwriteFactory();
    if (!factory) return format;

    if (FAILED(factory->CreateTextFormat(kSubtitleFontFamily, nullptr, weight,
                                         DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL,
                                         font_size, L"zh-cn", &format))) {
        return {};
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(paragraph_alignment);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
    format->SetTrimming(nullptr, nullptr);
    return format;
}

ComPtr<IDWriteTextLayout> CreateTextLayout(const std::wstring& text,
                                           IDWriteTextFormat* format,
                                           float width,
                                           float height) {
    ComPtr<IDWriteTextLayout> layout;
    auto* factory = SharedDwriteFactory();
    if (!factory || !format) return layout;
    const auto& measured = text.empty() ? std::wstring(L" ") : text;
    if (FAILED(factory->CreateTextLayout(measured.c_str(),
                                         static_cast<UINT32>(measured.size()),
                                         format,
                                         std::max(1.0f, width),
                                         std::max(1.0f, height),
                                         &layout))) {
        return {};
    }
    return layout;
}

TextLayoutSize MeasureText(const std::wstring& text,
                           float font_size,
                           DWRITE_FONT_WEIGHT weight,
                           float width) {
    TextLayoutSize result;
    auto format = CreateTextFormat(font_size, weight, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    auto layout = CreateTextLayout(text, format.Get(), width, kTextLayoutMaxHeight);
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

// Picks the largest font size in [minimum, preferred] that allows the text to
// fit within `width` using at most two visual lines.
FittedText FitSubtitleText(const std::wstring& text,
                           float preferred_font_size,
                           float minimum_font_size,
                           float width) {
    FittedText fitted;
    for (float font_size = preferred_font_size; font_size >= minimum_font_size; font_size -= 1.0f) {
        auto metrics = MeasureText(text, font_size, DWRITE_FONT_WEIGHT_NORMAL, width);
        fitted = FittedText{font_size, metrics};
        if (metrics.line_count <= 2 && metrics.width <= width + 1.0f) {
            return fitted;
        }
    }
    return fitted;
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

void DrawTextLayout(ID2D1RenderTarget* target,
                    IDWriteTextLayout* layout,
                    float x,
                    float y,
                    BYTE alpha,
                    BYTE red,
                    BYTE green,
                    BYTE blue) {
    if (!target || !layout) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(
                static_cast<float>(red) / 255.0f,
                static_cast<float>(green) / 255.0f,
                static_cast<float>(blue) / 255.0f,
                static_cast<float>(alpha) / 255.0f),
            &brush))) {
        return;
    }
    target->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

} // namespace

SubtitleWindow::SubtitleWindow(HINSTANCE instance, HWND parent)
    : instance_(instance), parent_(parent) {
    EnsureGdiplus();

    WNDCLASSW wc{};
    wc.lpfnWndProc = SubtitleWindow::WndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"VoiceStickSubtitleWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"VoiceStick Subtitles",
        WS_POPUP,
        0, 0, 1, 1,
        parent_,
        nullptr,
        instance_,
        this);
    if (hwnd_) {
        RefreshDpi();
    }
}

SubtitleWindow::~SubtitleWindow() {
    ReleaseFrameBitmap();
    if (hwnd_) DestroyWindow(hwnd_);
}

void SubtitleWindow::ShowSubtitle(const std::string& text,
                                  const std::string& device_id,
                                  OverlayThemeColor color) {
    if (!hwnd_) return;
    auto normalized = BleProtocol::NormalizeDeviceId(device_id);
    if (normalized.empty()) return;
    auto trimmed = NormalizeSubtitleText(text);
    if (trimmed.empty()) return;

    ++generation_;
    lanes_[normalized] = Lane{Utf16(trimmed), color, generation_};
    SetTimer(hwnd_, kHideTimerBase + static_cast<UINT_PTR>(generation_), kHoldMs, nullptr);
    Render();
}

void SubtitleWindow::HideAll() {
    if (!hwnd_) return;
    lanes_.clear();
    ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK SubtitleWindow::WndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* self = reinterpret_cast<SubtitleWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = reinterpret_cast<SubtitleWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    return self ? self->HandleMessage(message, w_param, l_param)
                : DefWindowProcW(hwnd, message, w_param, l_param);
}

LRESULT SubtitleWindow::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_TIMER: {
        const auto generation = static_cast<int>(w_param - kHideTimerBase);
        KillTimer(hwnd_, w_param);
        for (auto it = lanes_.begin(); it != lanes_.end();) {
            if (it->second.generation == generation) {
                it = lanes_.erase(it);
            } else {
                ++it;
            }
        }
        Render();
        return 0;
    }
    case WM_DPICHANGED:
        dpi_ = HIWORD(w_param);
        RefreshDpi();
        Render();
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, w_param, l_param);
    }
}

void SubtitleWindow::Render() {
    if (lanes_.empty()) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }

    const WindowLayout layout = ComputeLayout();
    if (layout.width <= 0 || layout.height <= 0 || layout.lanes.empty()) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }

    SetWindowPos(hwnd_, HWND_TOPMOST,
                 layout.x, layout.y, layout.width, layout.height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateLayeredBitmap(layout);
}

SubtitleWindow::WindowLayout SubtitleWindow::ComputeLayout() const {
    WindowLayout layout;
    if (lanes_.empty()) return layout;

    const RECT work_area = GetWorkAreaForWindow(hwnd_);
    const int screen_width = std::max(1, static_cast<int>(work_area.right - work_area.left));
    const int screen_height = std::max(1, static_cast<int>(work_area.bottom - work_area.top));

    // Total horizontal chrome that is not occupied by text:
    //   window padding (left + right) + lane text insets (left + right).
    const int chrome_width = Dp(kWindowSidePadding) * 2 +
                             Dp(kLaneTextLeftInset + kLaneTextRightInset);
    const int max_window_width = std::max(Dp(kMinLaneWidth),
                                          static_cast<int>(screen_width * 0.86));
    const int natural_text_budget = std::max(1, max_window_width - chrome_width);

    // Pass 1: determine the final window width by measuring each lane's natural
    // single-line text width and taking the maximum (clamped to screen budget).
    int window_width = Dp(kMinLaneWidth);
    for (const auto& [_, lane] : lanes_) {
        const auto natural = MeasureText(lane.text,
                                         DpF(kTextFontSize),
                                         DWRITE_FONT_WEIGHT_NORMAL,
                                         static_cast<float>(natural_text_budget));
        const int needed = static_cast<int>(std::ceil(natural.width)) + chrome_width;
        window_width = std::max(window_width, std::min(max_window_width, needed));
    }

    // Pass 2: re-fit every lane using the FINAL text width that the renderer
    // will see. This is what guarantees window height and lane height agree.
    const int text_width = std::max(1, window_width - chrome_width);
    const int min_lane_height = Dp(kMinLaneHeight);
    const int vertical_inset_total = Dp(kLaneTextVerticalInset) * 2;

    int y = Dp(kWindowEdgePadding);
    layout.lanes.reserve(lanes_.size());
    bool first = true;
    for (const auto& [device_id, lane] : lanes_) {
        if (!first) y += Dp(kLaneGap);
        first = false;

        const auto fitted = FitSubtitleText(lane.text,
                                            DpF(kTextFontSize),
                                            DpF(kMinTextFontSize),
                                            static_cast<float>(text_width));
        const int text_height = static_cast<int>(std::ceil(fitted.metrics.height));
        const int lane_height = std::max(min_lane_height, text_height + vertical_inset_total);

        LaneLayout entry;
        entry.device_id = device_id;
        entry.lane = &lane;
        entry.font_size = fitted.font_size;
        entry.x = Dp(kWindowSidePadding);
        entry.y = y;
        entry.width = window_width - Dp(kWindowSidePadding) * 2;
        entry.height = lane_height;
        layout.lanes.push_back(entry);

        y += lane_height;
    }
    int total_height = y + Dp(kWindowEdgePadding);

    const int max_window_height = std::max(min_lane_height,
                                           static_cast<int>(screen_height * 0.36));
    total_height = std::min(total_height, max_window_height);

    layout.width = window_width;
    layout.height = total_height;
    layout.x = work_area.left + (screen_width - layout.width) / 2;
    layout.y = work_area.bottom - layout.height - Dp(kBottomScreenMargin);
    return layout;
}

void SubtitleWindow::UpdateLayeredBitmap(const WindowLayout& layout) {
    const int width = layout.width;
    const int height = layout.height;
    if (width <= 0 || height <= 0) return;
    if (!EnsureFrameBitmap(width, height)) return;

    const std::size_t byte_count = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4;
    std::memset(frame_bits_, 0, byte_count);

    // Phase 1 (GDI+): rounded background + accent color bar for every lane.
    {
        Gdiplus::Graphics graphics(frame_dc_);
        graphics.SetPageUnit(Gdiplus::UnitPixel);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        for (const auto& entry : layout.lanes) {
            Gdiplus::RectF lane_rect(
                static_cast<float>(entry.x) + 0.5f,
                static_cast<float>(entry.y) + 0.5f,
                static_cast<float>(entry.width) - 1.0f,
                static_cast<float>(entry.height) - 1.0f);

            Gdiplus::GraphicsPath background_path;
            AddRoundedRect(background_path, lane_rect, DpF(kLaneCornerRadius));
            Gdiplus::SolidBrush background_brush(Gdiplus::Color(158, 0, 0, 0));
            graphics.FillPath(&background_brush, &background_path);

            const auto color = ColorValue(entry.lane->color);
            const BYTE red = static_cast<BYTE>((color >> 16) & 0xff);
            const BYTE green = static_cast<BYTE>((color >> 8) & 0xff);
            const BYTE blue = static_cast<BYTE>(color & 0xff);

            Gdiplus::RectF bar_rect(
                lane_rect.X + DpF(kColorBarOffset),
                lane_rect.Y + DpF(kColorBarVerticalInset),
                DpF(kColorBarWidth),
                std::max(1.0f, lane_rect.Height - DpF(kColorBarVerticalInset) * 2));
            Gdiplus::GraphicsPath bar_path;
            AddRoundedRect(bar_path, bar_rect, DpF(kColorBarRadius));
            Gdiplus::SolidBrush accent_brush(Gdiplus::Color(255, red, green, blue));
            graphics.FillPath(&accent_brush, &bar_path);
        }
        graphics.Flush();
    }

    // Phase 2 (DirectWrite): device label + subtitle text, both vertically
    // centered in the lane regardless of how many lines the text wraps to.
    auto render_target = CreateBitmapRenderTarget(frame_bits_, width, height);
    if (render_target.target && render_target.bitmap) {
        auto device_format = CreateTextFormat(DpF(kDeviceFontSize),
                                              DWRITE_FONT_WEIGHT_BOLD,
                                              DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        render_target.target->BeginDraw();
        for (const auto& entry : layout.lanes) {
            const float lane_x = static_cast<float>(entry.x);
            const float lane_y = static_cast<float>(entry.y);
            const float lane_width = static_cast<float>(entry.width);
            const float lane_height = static_cast<float>(entry.height);

            const auto color = ColorValue(entry.lane->color);
            const BYTE red = static_cast<BYTE>((color >> 16) & 0xff);
            const BYTE green = static_cast<BYTE>((color >> 8) & 0xff);
            const BYTE blue = static_cast<BYTE>(color & 0xff);

            const auto device_text = Utf16(entry.device_id);
            auto device_layout = CreateTextLayout(device_text, device_format.Get(),
                                                  DpF(kDeviceLabelWidth), lane_height);
            DrawTextLayout(render_target.target.Get(), device_layout.Get(),
                           lane_x + DpF(kDeviceLabelOffset), lane_y,
                           242, red, green, blue);

            const float text_x = lane_x + DpF(kLaneTextLeftInset);
            const float text_y = lane_y + DpF(kLaneTextVerticalInset);
            const float text_width = lane_width - DpF(kLaneTextLeftInset + kLaneTextRightInset);
            const float text_height = lane_height - DpF(kLaneTextVerticalInset) * 2;

            auto text_format = CreateTextFormat(entry.font_size,
                                                DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto text_layout = CreateTextLayout(entry.lane->text, text_format.Get(),
                                                text_width, text_height);
            DrawTextLayout(render_target.target.Get(), text_layout.Get(),
                           text_x, text_y,
                           255, 255, 255, 255);
        }
        if (SUCCEEDED(render_target.target->EndDraw())) {
            render_target.bitmap->CopyPixels(nullptr, render_target.stride,
                                             render_target.buffer_size,
                                             static_cast<BYTE*>(frame_bits_));
        }
    }

    HDC screen_dc = GetDC(nullptr);
    POINT destination = {layout.x, layout.y};
    POINT source = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen_dc, &destination, &size, frame_dc_, &source,
                        0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen_dc);
}

bool SubtitleWindow::EnsureFrameBitmap(int width, int height) {
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
    return true;
}

void SubtitleWindow::ReleaseFrameBitmap() {
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
}

int SubtitleWindow::Dp(int px) const {
    return ScalePx(px, dpi_);
}

float SubtitleWindow::DpF(int px) const {
    return static_cast<float>(Dp(px));
}

void SubtitleWindow::RefreshDpi() {
    dpi_ = hwnd_ ? GetDpiForHwnd(hwnd_) : 96;
}

unsigned int SubtitleWindow::ColorValue(OverlayThemeColor color) const {
    switch (color) {
    case OverlayThemeColor::kPink: return 0xff6b9e;
    case OverlayThemeColor::kGreen: return 0x4fd68c;
    case OverlayThemeColor::kYellow: return 0xffc742;
    case OverlayThemeColor::kBlue: return 0x61adff;
    case OverlayThemeColor::kPurple: return 0xb88cff;
    case OverlayThemeColor::kWhite:
    default:
        return 0xffffff;
    }
}

std::wstring SubtitleWindow::Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), len);
    return wide;
}

std::string SubtitleWindow::NormalizeSubtitleText(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool pending_space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch) != 0) {
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space) {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(static_cast<char>(ch));
    }
    return normalized;
}

} // namespace voicestick
