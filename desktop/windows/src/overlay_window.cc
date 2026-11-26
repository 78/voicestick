#include "overlay_window.h"
#include "dpi_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <CommCtrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <objidl.h>
#include <gdiplus.h>
#include <imm.h>
#include <richedit.h>
#include <utility>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Imm32.lib")
#pragma comment(lib, "Dwrite.lib")
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Windowscodecs.lib")

namespace voicestick {

namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kOverlayFontFamily = L"Microsoft YaHei UI";
constexpr float kTextLayoutMaxHeight = 10000.0f;
constexpr wchar_t kSendTitle[] = L"Send [Main]";
constexpr wchar_t kRetryTitle[] = L"Paste Here";
constexpr wchar_t kCancelShakeTitle[] = L"Cancel [Shake]";
constexpr wchar_t kCancelSideTitle[] = L"Cancel [Side]";
constexpr int kConfirmationMargin = 24;
constexpr int kConfirmationButtonGap = 10;
constexpr COLORREF kConfirmationTextColor = RGB(32, 32, 32);

void SetDraftTextColor(HWND edit, WPARAM scope) {
    CHARFORMATW format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = kConfirmationTextColor;
    SendMessageW(edit, EM_SETCHARFORMAT, scope, reinterpret_cast<LPARAM>(&format));
}

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

int MeasureTextWidth(HDC dc, std::wstring_view text, HFONT font) {
    if (text.empty()) return 0;
    const auto previous = SelectObject(dc, font);
    SIZE extent{};
    GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &extent);
    SelectObject(dc, previous);
    return extent.cx;
}

LRESULT CALLBACK PastePlainText(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param,
                                UINT_PTR, DWORD_PTR) {
    if (message == WM_CHAR && w_param == 0x16) return 0;
    const bool paste_key = message == WM_KEYDOWN &&
        ((w_param == 'V' && GetKeyState(VK_CONTROL) < 0) ||
         (w_param == VK_INSERT && GetKeyState(VK_SHIFT) < 0));
    if (message == WM_PASTE || paste_key) {
        SendMessageW(hwnd, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
        return 0;
    }
    return DefSubclassProc(hwnd, message, w_param, l_param);
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
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TRANSPARENT | WS_EX_CONTROLPARENT,
        wc.lpszClassName, L"", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1, parent_, nullptr, instance_, this);
    // Newly exposed pixels in the redirected surface start black until WM_PAINT.
    SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), kBackgroundAlpha, LWA_ALPHA | LWA_COLORKEY);
    EnsureGdiplus();
}

OverlayWindow::~OverlayWindow() {
    if (shadow_window_) DestroyWindow(shadow_window_);
    if (hwnd_) {
        KillTimer(hwnd_, kAutoHideTimerId);
        KillTimer(hwnd_, kAnimationTimerId);
        DestroyWindow(hwnd_);
    }
    for (HFONT font : {draft_font_, action_font_, status_font_}) {
        if (font) DeleteObject(font);
    }
    ReleaseFrameBitmap();
}

void OverlayWindow::ShowListening(std::string output_mode) {
    Show(Mode::kListening, "Listening...", output_mode);
}

void OverlayWindow::ShowPartial(const std::string& text, std::string output_mode) {
    Show(Mode::kListening, text.empty() ? "Listening..." : text, output_mode);
}

void OverlayWindow::ShowFinalCountdown(const std::string& text, int duration_ms,
                                       std::function<void()> on_complete) {
    countdown_duration_ms_ = duration_ms;
    countdown_started_at_ms_ = GetTickCount64();
    Show(Mode::kCountdown, text);
    pending_callback_ = std::move(on_complete);
    SetTimer(hwnd_, kAutoHideTimerId, countdown_duration_ms_, nullptr);
}

void OverlayWindow::ShowPausedFinal(const std::string& text, bool supports_shake,
                                   bool has_secondary_button, bool has_primary_button) {
    KillTimer(hwnd_, kAutoHideTimerId);
    KillTimer(hwnd_, kAnimationTimerId);
    pending_callback_ = {};
    if (mode_ == Mode::kCountdown) {
        paused_countdown_progress_ = std::clamp(1.0f -
            static_cast<float>(GetTickCount64() - countdown_started_at_ms_) /
            static_cast<float>(countdown_duration_ms_), 0.0f, 1.0f);
    } else if (mode_ != Mode::kPaused) {
        paused_countdown_progress_ = 1.0f;
    }

    if (!CreateConfirmationControls()) {
        Show(Mode::kPaused, text);
        return;
    }

    const auto text_wide = Utf16FromUtf8(text);
    const bool keep_body = mode_ == Mode::kCountdown && text_ == text_wide && IsWindowVisible(hwnd_);
    std::wstring draft;
    for (wchar_t ch : text_wide) {
        if (ch == L'\n' && (draft.empty() || draft.back() != L'\r')) draft.push_back(L'\r');
        draft.push_back(ch);
    }
    if (!confirmation_active_ || EditedConfirmationText() != text) {
        SendMessageW(draft_edit_, EM_SETSEL, 0, -1);
        SendMessageW(draft_edit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(draft.c_str()));
        if (draft_font_) SendMessageW(draft_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(draft_font_), FALSE);
        // WM_SETFONT can restore RichEdit's automatic black text, which is the
        // layered window's transparent color.
        SetDraftTextColor(draft_edit_, SCF_ALL);
        SendMessageW(draft_edit_, EM_SETSEL, 0, -1);
        PARAFORMAT2 paragraph{};
        paragraph.cbSize = sizeof(paragraph);
        paragraph.dwMask = PFM_ALIGNMENT | PFM_LINESPACING;
        paragraph.wAlignment = PFA_CENTER;
        paragraph.bLineSpacingRule = 5;
        paragraph.dyLineSpacing = kDraftLineSpacing;
        SendMessageW(draft_edit_, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));
        SendMessageW(draft_edit_, EM_SETSEL, 0, 0);
        SendMessageW(draft_edit_, EM_EMPTYUNDOBUFFER, 0, 0);
    }
    SetWindowTextW(confirmation_status_, L"");
    SetWindowTextW(send_button_, has_primary_button ? kSendTitle : L"Send");
    SetWindowTextW(cancel_button_, supports_shake ? kCancelShakeTitle
                       : (has_secondary_button ? kCancelSideTitle : L"Cancel"));
    retrying_paste_ = false;
    mode_ = Mode::kPaused;
    text_ = text_wide;
    output_mode_.clear();
    SetConfirmationMode(true);

    for (HWND control : {draft_edit_, cancel_button_, send_button_}) {
        ShowWindow(control, SW_SHOWNA);
    }
    if (keep_body) LayoutConfirmationControls();
    else Reposition();
    UpdateConfirmationDraft();
}

std::optional<std::string> OverlayWindow::EditedConfirmationText() const {
    if (!confirmation_active_ || !IsWindowVisible(hwnd_)) return std::nullopt;
    constexpr UINT unicode_code_page = 1200;
    const GETTEXTLENGTHEX request{GTL_USECRLF | GTL_NUMCHARS | GTL_PRECISE, unicode_code_page};
    const int length = static_cast<int>(SendMessageW(
        draft_edit_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&request), 0));
    if (length <= 0) return std::string{};
    std::wstring draft(static_cast<std::size_t>(length) + 1, L'\0');
    const GETTEXTEX read{static_cast<DWORD>(draft.size() * sizeof(wchar_t)),
                        GT_USECRLF, unicode_code_page, nullptr, nullptr};
    const int copied = static_cast<int>(SendMessageW(draft_edit_, EM_GETTEXTEX,
        reinterpret_cast<WPARAM>(&read), reinterpret_cast<LPARAM>(draft.data())));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, draft.data(), copied, nullptr, 0, nullptr, nullptr);
    std::string text(bytes, '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, draft.data(), copied, text.data(), bytes, nullptr, nullptr);
    }
    return text;
}

std::optional<std::string> OverlayWindow::ConfirmationDraftForSending() {
    if (confirmation_active_ && GetFocus() == draft_edit_) {
        if (const HIMC ime = ImmGetContext(draft_edit_)) {
            ImmNotifyIME(ime, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
            ImmReleaseContext(draft_edit_, ime);
        }
    }
    return EditedConfirmationText();
}

void OverlayWindow::ConcealConfirmation() {
    if (!confirmation_active_) return;
    if (shadow_window_) ShowWindow(shadow_window_, SW_HIDE);
    ShowWindow(hwnd_, SW_HIDE);
}

void OverlayWindow::ShowPasteFailure() {
    if (!confirmation_active_) return;
    retrying_paste_ = true;
    SetWindowTextW(confirmation_status_, L"Click an input field, then Paste Here.");
    SetWindowTextW(send_button_, kRetryTitle);
    LayoutConfirmationControls();
    ShowWindow(confirmation_status_, SW_SHOWNA);
}

bool OverlayWindow::HandleDialogMessage(MSG* message) {
    if (!confirmation_active_ || !IsWindowVisible(hwnd_)) return false;
    if (message->message == WM_KEYDOWN && message->wParam == VK_ESCAPE &&
        (message->hwnd == hwnd_ || IsChild(hwnd_, message->hwnd))) {
        if (on_cancel_confirmation) on_cancel_confirmation();
        return true;
    }
    return IsDialogMessageW(hwnd_, message);
}

bool OverlayWindow::CreateConfirmationControls() {
    if (draft_edit_) return true;
    static const auto rich_edit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!rich_edit) return false;
    draft_edit_ = CreateWindowExW(0, MSFTEDIT_CLASS, nullptr,
                                  WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_CENTER | ES_NOOLEDRAGDROP |
                                      ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                  0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    confirmation_status_ = CreateWindowExW(0, L"STATIC", nullptr,
                                            WS_CHILD | SS_RIGHT | SS_ENDELLIPSIS,
                                            0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    cancel_button_ = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                      WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), instance_, nullptr);
    send_button_ = CreateWindowExW(0, L"BUTTON", L"Send",
                                    WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 0, 0, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), instance_, nullptr);
    if (draft_edit_ && confirmation_status_ && cancel_button_ && send_button_ &&
        SetWindowSubclass(draft_edit_, PastePlainText, 1, 0)) {
        SetDraftTextColor(draft_edit_, SCF_DEFAULT);
        SendMessageW(confirmation_status_, WM_SETFONT, reinterpret_cast<WPARAM>(status_font_), FALSE);
        for (HWND button : {cancel_button_, send_button_}) {
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(action_font_), FALSE);
        }
        SendMessageW(draft_edit_, EM_EXLIMITTEXT, 0, 0x7ffffffe);
        SendMessageW(draft_edit_, EM_SETEVENTMASK, 0, ENM_CHANGE);
        return true;
    }
    for (auto* control : {&draft_edit_, &confirmation_status_, &cancel_button_, &send_button_}) {
        if (*control) DestroyWindow(*control);
        *control = nullptr;
    }
    return false;
}

void OverlayWindow::SetConfirmationMode(bool active) {
    if (confirmation_active_ == active) return;
    confirmation_active_ = active;
    if (!active) {
        retrying_paste_ = false;
        for (HWND control : {draft_edit_, confirmation_status_, cancel_button_, send_button_}) {
            ShowWindow(control, SW_HIDE);
        }
    }

    // Keep the redirected surface alive while enabling the editor and mouse input.
    auto style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (active) style &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    else style |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style);
}

void OverlayWindow::SetConfirmationFonts() {
    if (confirmation_font_dpi_ == dpi_) return;
    confirmation_font_dpi_ = dpi_;
    const auto set_font = [&](HFONT& font, int size, int weight, BYTE quality, HWND first, HWND second = nullptr) {
        const auto new_font = CreateFontW(-Dp(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          0, 0, quality, 0, kOverlayFontFamily);
        if (!new_font) return;
        if (first) SendMessageW(first, WM_SETFONT, reinterpret_cast<WPARAM>(new_font), FALSE);
        if (second) SendMessageW(second, WM_SETFONT, reinterpret_cast<WPARAM>(new_font), FALSE);
        if (font) DeleteObject(font);
        font = new_font;
    };
    set_font(draft_font_, kTextFontSize, FW_NORMAL, ANTIALIASED_QUALITY, draft_edit_);
    if (draft_edit_) SetDraftTextColor(draft_edit_, SCF_ALL);
    set_font(action_font_, 20, FW_MEDIUM, CLEARTYPE_QUALITY, cancel_button_, send_button_);
    set_font(status_font_, 14, FW_MEDIUM, CLEARTYPE_QUALITY, confirmation_status_);
}

int OverlayWindow::ConfirmationButtonWidth(std::wstring_view title) {
    const auto shortcut = title.find(L" [");
    const HDC dc = GetDC(hwnd_);
    const int title_width = MeasureTextWidth(dc, title.substr(0, shortcut), action_font_);
    const int shortcut_width = shortcut == std::wstring_view::npos ? 0
        : MeasureTextWidth(dc, title.substr(shortcut), status_font_);
    ReleaseDC(hwnd_, dc);
    return std::max(Dp(96), title_width + shortcut_width + Dp(20));
}

int OverlayWindow::MinimumConfirmationWidth() {
    SetConfirmationFonts();
    const int cancel = std::max(ConfirmationButtonWidth(kCancelShakeTitle), ConfirmationButtonWidth(kCancelSideTitle));
    const int send = std::max(ConfirmationButtonWidth(kSendTitle), ConfirmationButtonWidth(kRetryTitle));
    return 2 * Dp(kConfirmationMargin) + Dp(kConfirmationButtonGap) + cancel + send;
}

void OverlayWindow::LayoutConfirmationControls() {
    SetConfirmationFonts();
    const int shadow = Dp(kShadowPadding);
    const int body_width = target_window_width_ - shadow * 2;
    const int body_height = target_window_height_ - shadow * 2;
    const int footer = Dp(retrying_paste_ ? kPasteFailureFooterHeight : kConfirmationFooterHeight);
    const int height = body_height + footer;
    const int text_left = Dp(kHorizontalPadding) + Dp(kIndicatorSize) + Dp(kContentSpacing);
    const int top_inset = std::max(Dp(kVerticalPadding), (body_height - body_text_height_) / 2);
    const int control_top = std::min(top_inset, body_height - Dp(kVerticalPadding) - Dp(kTextFontSize));
    const int button_height = Dp(38);
    const int button_top = height - button_height - Dp(8);
    const int margin = Dp(kConfirmationMargin);
    const int gap = Dp(kConfirmationButtonGap);
    SendMessageW(draft_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SendMessageW(draft_edit_, EM_SETBKGNDCOLOR, FALSE, BackgroundColor());

    const auto button_width = [&](HWND button) {
        wchar_t label[32]{};
        GetWindowTextW(button, label, static_cast<int>(std::size(label)));
        return ConfirmationButtonWidth(label);
    };

    const int send_width = button_width(send_button_);
    const int cancel_width = button_width(cancel_button_);
    const int width = std::max(body_width, margin * 2 + send_width + gap + cancel_width);
    const int right = width - margin;
    const int text_width = std::max(1, width - text_left - Dp(kHorizontalPadding));
    const int x = target_window_x_ + shadow - VisualOffsetX(width, body_width);
    const int y = target_window_y_ + shadow;
    SetSurfaceBounds(x, y, width, height);
    // The editing control can reserve space for its scrollbar while wrapping.
    const int editor_extra = Dp(12);
    MoveWindow(draft_edit_, text_left - editor_extra, control_top, text_width + editor_extra * 2,
               body_height - control_top - Dp(kVerticalPadding), TRUE);
    MoveWindow(confirmation_status_, Dp(24), body_height, width - Dp(48), Dp(19), TRUE);
    MoveWindow(cancel_button_, right - send_width - gap - cancel_width, button_top,
               cancel_width, button_height, TRUE);
    MoveWindow(send_button_, right - send_width, button_top, send_width, button_height, TRUE);
    ShowWindow(confirmation_status_, retrying_paste_ ? SW_SHOWNA : SW_HIDE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    UpdateShadow(x, y, width, height);
}

void OverlayWindow::UpdateShadow(int x, int y, int width, int height) {
    // The rounded surface clips its own painting. Keep the shadow behind it.
    if (!shadow_window_) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance_;
        wc.lpszClassName = L"VoiceStickOverlayShadow";
        RegisterClassW(&wc);
        shadow_window_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, parent_, nullptr, instance_, nullptr);
        if (!shadow_window_) return;
    }

    const int padding = Dp(kShadowPadding);
    SIZE size{width + padding * 2, height + padding * 2};
    POINT destination{x - padding, y - padding};
    RECT previous{};
    GetWindowRect(shadow_window_, &previous);
    if (shadow_dpi_ == dpi_ && previous.right - previous.left == size.cx &&
        previous.bottom - previous.top == size.cy) {
        SetWindowPos(shadow_window_, hwnd_, destination.x, destination.y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        return;
    }
    if (!EnsureFrameBitmap(size.cx, size.cy)) {
        ShowWindow(shadow_window_, SW_HIDE);
        return;
    }
    std::memset(frame_bits_, 0, static_cast<std::size_t>(size.cx) * size.cy * 4);
    {
        Gdiplus::Graphics graphics(frame_dc_);
        graphics.SetPageUnit(Gdiplus::UnitPixel);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const float inset = static_cast<float>(padding) + 0.5f;
        PaintShadow(graphics, Gdiplus::RectF(inset, inset, width - 1.0f, height - 1.0f));
    }
    InvalidateStaticLayer();
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, kMaxAlpha, AC_SRC_ALPHA};
    if (UpdateLayeredWindow(shadow_window_, nullptr, &destination, &size,
                            frame_dc_, &source, 0, &blend, ULW_ALPHA)) {
        shadow_dpi_ = dpi_;
        SetWindowPos(shadow_window_, hwnd_, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    } else {
        ShowWindow(shadow_window_, SW_HIDE);
    }
}

void OverlayWindow::UpdateConfirmationDraft() {
    const auto content = EditedConfirmationText();
    EnableWindow(send_button_, content && content->find_first_not_of(" \r\n\t") != std::string::npos);
}

void OverlayWindow::PaintConfirmationButton(const DRAWITEMSTRUCT& item) {
    SetDCBrushColor(item.hDC, BackgroundColor());
    FillRect(item.hDC, &item.rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    {
        Gdiplus::Graphics graphics(item.hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const auto& rect = item.rcItem;
        const Gdiplus::RectF frame(static_cast<float>(rect.left), static_cast<float>(rect.top),
                                   static_cast<float>(rect.right - rect.left - 1),
                                   static_cast<float>(rect.bottom - rect.top - 1));
        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, frame, DpF(9));
        Gdiplus::SolidBrush fill(pressed ? Gdiplus::Color(232, 232, 232)
                                         : Gdiplus::Color(252, 252, 252));
        Gdiplus::Pen border(Gdiplus::Color(215, 215, 215), DpF(1));
        graphics.FillPath(&fill, &path);
        graphics.DrawPath(&border, &path);
    }
    wchar_t label[32]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    const auto previous_font = SelectObject(item.hDC, action_font_);
    SetBkMode(item.hDC, TRANSPARENT);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    SetTextColor(item.hDC, disabled ? RGB(140, 140, 140) : RGB(55, 55, 55));
    auto text_rect = item.rcItem;
    constexpr UINT format = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    const std::wstring_view title(label);
    if (const auto shortcut = title.find(L" ["); shortcut != std::wstring_view::npos) {
        const int leading = static_cast<int>(shortcut);
        const int trailing = static_cast<int>(title.size() - shortcut);
        const int title_width = MeasureTextWidth(item.hDC, title.substr(0, shortcut), action_font_);
        const int shortcut_width = MeasureTextWidth(item.hDC, title.substr(shortcut), status_font_);
        text_rect.left += (text_rect.right - text_rect.left - title_width - shortcut_width) / 2;
        DrawTextW(item.hDC, label, leading, &text_rect, format | DT_LEFT);
        text_rect.left += title_width;
        SelectObject(item.hDC, status_font_);
        SetTextColor(item.hDC, disabled ? RGB(140, 140, 140) : RGB(108, 108, 108));
        DrawTextW(item.hDC, label + shortcut, trailing, &text_rect, format | DT_LEFT);
    } else {
        DrawTextW(item.hDC, label, -1, &text_rect, format | DT_CENTER);
    }
    SelectObject(item.hDC, previous_font);
    if (item.itemState & ODS_FOCUS) {
        text_rect = item.rcItem;
        InflateRect(&text_rect, -Dp(4), -Dp(4));
        DrawFocusRect(item.hDC, &text_rect);
    }
}

void OverlayWindow::ShowError(const std::string& text, std::function<void()> on_complete) {
    Show(Mode::kError, text.empty() ? "ASR Error" : text);
    pending_callback_ = std::move(on_complete);
    SetTimer(hwnd_, kAutoHideTimerId, 2000, nullptr);
}

void OverlayWindow::SetThemeColor(OverlayThemeColor color) {
    if (theme_color_ == color) return;
    theme_color_ = color;
    InvalidateStaticLayer();
    if (confirmation_active_) {
        SendMessageW(draft_edit_, EM_SETBKGNDCOLOR, FALSE, BackgroundColor());
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    else if (mode_ != Mode::kHidden) InvalidateRect(hwnd_, nullptr, FALSE);
}

void OverlayWindow::SetPosition(OverlayPosition position) {
    if (position_ == position) return;
    position_ = position;
    if (mode_ != Mode::kHidden) Reposition();
}

void OverlayWindow::Hide(std::function<void()> on_hidden) {
    KillTimer(hwnd_, kAutoHideTimerId);
    KillTimer(hwnd_, kAnimationTimerId);
    pending_callback_ = std::move(on_hidden);
    ShowWindow(hwnd_, SW_HIDE);
    if (shadow_window_) ShowWindow(shadow_window_, SW_HIDE);
    SetConfirmationMode(false);
    mode_ = Mode::kHidden;
    largest_visible_height_ = 0;
    target_window_width_ = 0;
    target_window_height_ = 0;
    target_window_x_ = 0;
    target_window_y_ = 0;
    ReleaseFrameBitmap();
    auto callback = std::exchange(pending_callback_, {});
    if (callback) callback();
}

void OverlayWindow::OnTimer(UINT_PTR timer_id) {
    if (timer_id == kAutoHideTimerId) {
        if (mode_ != Mode::kCountdown && mode_ != Mode::kError) return;
        if (mode_ == Mode::kCountdown &&
            GetTickCount64() - countdown_started_at_ms_ < static_cast<ULONGLONG>(countdown_duration_ms_)) {
            return;
        }
        Hide(std::move(pending_callback_));
    } else if (timer_id == kAnimationTimerId) {
        if (mode_ == Mode::kListening || mode_ == Mode::kCountdown) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            KillTimer(hwnd_, kAnimationTimerId);
        }
    }
}

void OverlayWindow::OnPaint() {
    PAINTSTRUCT ps{};
    const auto dc = BeginPaint(hwnd_, &ps);
    if (confirmation_active_) {
        SetDCBrushColor(dc, BackgroundColor());
        FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const int indicator = Dp(kIndicatorSize);
        const int body_height = target_window_height_ - 2 * Dp(kShadowPadding);
        PaintIndicator(graphics, Dp(kHorizontalPadding), (body_height - indicator) / 2, indicator);
    } else {
        PaintPreview(dc);
    }
    EndPaint(hwnd_, &ps);
}

void OverlayWindow::Show(Mode mode, const std::string& text, const std::string& output_mode) {
    KillTimer(hwnd_, kAutoHideTimerId);
    SetConfirmationMode(false);
    pending_callback_ = {};
    mode_ = mode;
    text_ = Utf16FromUtf8(text);
    output_mode_ = Utf16FromUtf8(output_mode);
    InvalidateStaticLayer();
    Reposition();

    if (mode == Mode::kListening || mode == Mode::kCountdown) {
        SetTimer(hwnd_, kAnimationTimerId, kAnimationStepMs, nullptr);
    } else {
        KillTimer(hwnd_, kAnimationTimerId);
    }

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
    const int preferred_max_width = std::clamp(screen_w * 45 / 100, Dp(kMinWidthLimit), Dp(kMaxWidthLimit));
    const int available_max_width = std::max(side_chrome_width + 1,
        std::min(preferred_max_width, screen_w - Dp(48) - shadow_padding * 2));
    const int max_text_width = std::max(1, available_max_width - side_chrome_width);

    const float text_font_size = DpF(kTextFontSize);
    const float text_line_height = text_font_size * kTextLineHeightMultiplier;
    const float text_baseline = text_font_size * kTextBaselineMultiplier;
    const auto single_line_text = MeasureText(text_, text_font_size,
                                              static_cast<float>(max_text_width) * 4.0f,
                                              false, text_line_height, text_baseline);
    const float measured_text_width = single_line_text.width;
    const bool can_confirm = mode_ == Mode::kCountdown || mode_ == Mode::kPaused;
    const int min_width = can_confirm ? std::max(Dp(kMinContentWidth), MinimumConfirmationWidth())
                                      : Dp(kMinContentWidth);
    const int content_width = std::min(available_max_width,
        std::max(min_width, static_cast<int>(measured_text_width) + side_chrome_width));
    const int text_width = content_width - side_chrome_width;
    should_wrap_text_ = measured_text_width > static_cast<float>(max_text_width) ||
                        text_.find(L'\n') != std::wstring::npos;
    const auto laid_out_text = should_wrap_text_
        ? MeasureText(text_, text_font_size, static_cast<float>(text_width), true,
                      text_line_height + DpF(kMultilineGap), text_baseline + DpF(kMultilineGap) / 2)
        : single_line_text;
    const float text_height = laid_out_text.height;
    body_text_height_ = static_cast<int>(std::ceil(text_height));
    const int footer = can_confirm ? Dp(kPasteFailureFooterHeight) : 0;
    const int max_content_height = std::max(min_content_height,
        screen_h - std::max(Dp(120), Dp(kPositionMargin * 2) + footer) - shadow_padding * 2);
    const int desired_content_height = static_cast<int>(std::ceil(
        vertical_padding * 2 + std::max(text_height, static_cast<float>(indicator_size))));

    // Keep the body height stable through partial results and confirmation.
    largest_visible_height_ = std::min(max_content_height,
        std::max({min_content_height, desired_content_height, largest_visible_height_}));

    target_window_width_ = content_width + shadow_padding * 2;
    target_window_height_ = largest_visible_height_ + shadow_padding * 2;
    const POINT origin = TargetWindowOrigin(work_area, target_window_width_, target_window_height_);
    target_window_x_ = origin.x;
    target_window_y_ = std::max(static_cast<int>(work_area.top) + Dp(kPositionMargin),
        std::min(static_cast<int>(origin.y),
                 static_cast<int>(work_area.bottom) - Dp(kPositionMargin) - target_window_height_ - footer));

    if (confirmation_active_) {
        LayoutConfirmationControls();
    } else {
        const int x = target_window_x_ + shadow_padding;
        const int y = target_window_y_ + shadow_padding;
        SetSurfaceBounds(x, y, content_width, largest_visible_height_);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        UpdateShadow(x, y, content_width, largest_visible_height_);
    }
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

void OverlayWindow::RefreshDpi() {
    dpi_ = GetDpiForHwnd(parent_ ? parent_ : hwnd_);
}

int OverlayWindow::Dp(int px) const {
    return voicestick::ScalePx(px, dpi_);
}

float OverlayWindow::DpF(int px) const {
    return ScaleF(px, dpi_);
}

void OverlayWindow::SetSurfaceBounds(int x, int y, int width, int height) {
    RECT current{};
    GetWindowRect(hwnd_, &current);
    const bool resized = current.right - current.left != width || current.bottom - current.top != height;
    if (resized || current.left != x || current.top != y) {
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    if (resized || surface_dpi_ != dpi_) {
        const int diameter = Dp(kCornerRadius * 2);
        if (auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter)) {
            if (SetWindowRgn(hwnd_, region, FALSE)) surface_dpi_ = dpi_;
            else DeleteObject(region);
        }
    }
}

void OverlayWindow::PaintPreview(HDC dc) {
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    const int width = rect.right;
    const int height = rect.bottom;
    if (width <= 0 || height <= 0) return;
    if (!EnsureFrameBitmap(width, height)) {
        SetDCBrushColor(dc, BackgroundColor());
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return;
    }

    if (static_layer_dirty_) {
        BuildStaticLayer(width, height);
    } else {
        std::memcpy(frame_bits_, static_layer_bits_.data(), static_layer_bits_.size());
    }
    {
        Gdiplus::Graphics graphics(frame_dc_);
        graphics.SetPageUnit(Gdiplus::UnitPixel);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const int indicator = Dp(kIndicatorSize);
        PaintIndicator(graphics, Dp(kHorizontalPadding), (height - indicator) / 2, indicator);
    }
    BitBlt(dc, 0, 0, width, height, frame_dc_, 0, 0, SRCCOPY);
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
    const auto color = BackgroundColor();
    const DWORD pixel = 0xff000000 | (static_cast<DWORD>(GetRValue(color)) << 16) |
                        (static_cast<DWORD>(GetGValue(color)) << 8) | GetBValue(color);
    std::fill_n(static_cast<DWORD*>(frame_bits_), byte_count / sizeof(DWORD), pixel);
    PaintText(frame_bits_, width, height);
    std::memcpy(static_layer_bits_.data(), frame_bits_, byte_count);
    static_layer_dirty_ = false;
}

void OverlayWindow::PaintShadow(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds) {
    const float shadow_y_offset = DpF(kShadowYOffset);
    const float corner_radius = DpF(kCornerRadius);
    for (int layer = kShadowBlur; layer >= 1; --layer) {
        const float spread = DpF(layer);
        const BYTE alpha = static_cast<BYTE>(1 + (kShadowBlur - layer) / 2);
        Gdiplus::RectF shadow_rect(bounds.X - spread,
                                   bounds.Y - spread + shadow_y_offset,
                                   bounds.Width + spread * 2.0f,
                                   bounds.Height + spread * 2.0f);
        Gdiplus::GraphicsPath shadow_path;
        AddRoundedRect(shadow_path, shadow_rect, corner_radius + spread);
        Gdiplus::SolidBrush shadow_brush(Gdiplus::Color(alpha, 0, 0, 0));
        graphics.FillPath(&shadow_brush, &shadow_path);
    }
}

COLORREF OverlayWindow::BackgroundColor() const {
    switch (theme_color_) {
    case OverlayThemeColor::kPink: return RGB(255, 214, 230);
    case OverlayThemeColor::kGreen: return RGB(214, 242, 214);
    case OverlayThemeColor::kYellow: return RGB(255, 240, 184);
    case OverlayThemeColor::kBlue: return RGB(209, 232, 255);
    case OverlayThemeColor::kPurple: return RGB(230, 214, 255);
    case OverlayThemeColor::kWhite:
    default: return RGB(255, 255, 255);
    }
}

void OverlayWindow::PaintText(void* bits, int width, int height) {
    const int horizontal_padding = Dp(kHorizontalPadding);
    const int indicator_size = Dp(kIndicatorSize);
    const int content_spacing = Dp(kContentSpacing);
    const float text_x = static_cast<float>(horizontal_padding + indicator_size + content_spacing);
    const float text_width = static_cast<float>(width - horizontal_padding * 2 -
                                                indicator_size - content_spacing);

    const float text_font_size = DpF(kTextFontSize);
    const float extra_gap = should_wrap_text_ ? DpF(kMultilineGap) : 0;
    const float text_line_height = text_font_size * kTextLineHeightMultiplier + extra_gap;
    const float text_baseline = text_font_size * kTextBaselineMultiplier + extra_gap / 2;
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

    const float block_y = std::max(0.0f, (static_cast<float>(height) - text_metrics.height) / 2.0f);
    auto render_target = CreateBitmapRenderTarget(bits, width, height);
    if (!render_target.target || !render_target.bitmap) return;
    const D2D1_RECT_F text_clip = D2D1::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    render_target.target->BeginDraw();
    render_target.target->PushAxisAlignedClip(text_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    DrawTextLayout(render_target.target.Get(), text_layout.Get(), text_x, block_y,
                   kTextAlpha, kInkRgb);
    if (!output_mode_.empty()) {
        auto format = CreateTextFormat(DpF(kOutputModeFontSize));
        if (format) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(SharedDwriteFactory()->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
                format->SetTrimming(&trimming, ellipsis.Get());
            }
            const float left = text_clip.left + DpF(23);
            const float right = text_clip.right - DpF(11);
            auto label = CreateTextLayout(output_mode_, format.Get(), right - left, false);
            DWRITE_TEXT_METRICS metrics{};
            if (label && SUCCEEDED(label->GetMetrics(&metrics))) {
                DrawTextLayout(render_target.target.Get(), label.Get(), left,
                               text_clip.bottom - DpF(11) - metrics.height, kOutputModeAlpha, kInkRgb);
            }
        }
    }
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
    } else if (mode_ == Mode::kCountdown || mode_ == Mode::kPaused) {
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
        const float remaining = mode_ == Mode::kPaused ? paused_countdown_progress_ : std::clamp(
            1.0f - static_cast<float>(elapsed_ms) / static_cast<float>(countdown_duration_ms_), 0.0f, 1.0f);
        if (remaining > 0.0f) {
            graphics.DrawArc(&ring_pen, ring_rect, -90.0f, -360.0f * remaining);
        }
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

void OverlayWindow::OnDpiChanged(UINT new_dpi, const RECT* suggested_rect) {
    dpi_ = new_dpi != 0 ? new_dpi : 96;
    ReleaseFrameBitmap();
    if (mode_ != Mode::kHidden) {
        largest_visible_height_ = 0;
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
    case WM_MOUSEACTIVATE:
        if (self->confirmation_active_) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            return ChildWindowFromPoint(hwnd, point) == self->draft_edit_ ? MA_ACTIVATE : MA_NOACTIVATE;
        }
        return MA_NOACTIVATE;
    case WM_ACTIVATE:
        if (self->confirmation_active_ && LOWORD(wp) != WA_INACTIVE) {
            SetFocus(self->draft_edit_);
        }
        break;
    case WM_COMMAND:
        if (!self->confirmation_active_) break;
        if (HIWORD(wp) == EN_CHANGE && reinterpret_cast<HWND>(lp) == self->draft_edit_) {
            self->UpdateConfirmationDraft();
            return 0;
        }
        if (LOWORD(wp) == IDOK && IsWindowEnabled(self->send_button_)) {
            if (self->on_send_confirmation) self->on_send_confirmation();
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            if (self->on_cancel_confirmation) self->on_cancel_confirmation();
            return 0;
        }
        break;
    case WM_CLOSE:
        if (self->confirmation_active_ && self->on_cancel_confirmation) self->on_cancel_confirmation();
        return 0;
    case WM_DRAWITEM:
        if (self->confirmation_active_ && (wp == IDOK || wp == IDCANCEL)) {
            self->PaintConfirmationButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
            return TRUE;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (self->confirmation_active_) {
            const auto dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, kConfirmationTextColor);
            SetBkColor(dc, self->BackgroundColor());
            SetDCBrushColor(dc, self->BackgroundColor());
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        break;
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
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace voicestick
