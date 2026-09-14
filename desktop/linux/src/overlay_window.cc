#include "overlay_window.h"

#include "log.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <cairo-xlib.h>
#include <cairo.h>
#include <glib-unix.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace voicestick {

namespace {

struct ThemeRgba {
    double r, g, b, a;
};

ThemeRgba ThemeColor(OverlayThemeColor color) {
    switch (color) {
    case OverlayThemeColor::kPink: return {1.0, 0.84, 0.90, 0.86};
    case OverlayThemeColor::kGreen: return {0.84, 0.95, 0.84, 0.86};
    case OverlayThemeColor::kYellow: return {1.0, 0.94, 0.72, 0.86};
    case OverlayThemeColor::kBlue: return {0.82, 0.91, 1.0, 0.86};
    case OverlayThemeColor::kPurple: return {0.90, 0.84, 1.0, 0.86};
    case OverlayThemeColor::kWhite:
    default:
        return {1.0, 1.0, 1.0, 0.86};
    }
}

double DesktopScale() {
    auto* display = gdk_display_get_default();
    if (!display) return 1.0;
    auto* monitors = gdk_display_get_monitors(display);
    if (!monitors) return 1.0;
    double scale = 1.0;
    const guint n = g_list_model_get_n_items(monitors);
    for (guint i = 0; i < n; ++i) {
        auto* monitor = GDK_MONITOR(g_list_model_get_item(monitors, i));
        if (!monitor) continue;
        double next = gdk_monitor_get_scale(monitor);
        if (next < 1.0) next = gdk_monitor_get_scale_factor(monitor);
        scale = std::max(scale, next);
        g_object_unref(monitor);
    }
    return scale < 1.0 ? 1.0 : scale;
}

void RoundRect(cairo_t* cr, double x, double y, double w, double h, double radius) {
    const double r = std::min(radius, std::min(w, h) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

} // namespace

struct OverlayWindow::Impl {
    Display* display = nullptr;
    Window window = 0;
    Visual* visual = nullptr;
    Colormap colormap = 0;
    int screen = 0;
    int depth = 32;
    static constexpr int kPad = 48;
    static constexpr int kMinLogicalW = 320;
    static constexpr int kMaxLogicalW = 720;
    static constexpr int kMinLogicalH = 80;
    static constexpr int kMaxLogicalH = 280;

    int win_w = 720;
    int win_h = 280;
    int card_w = 320;
    int card_h = 80;
    int text_box_w = 272;
    bool should_wrap = false;
    int floor_card_w = 0;
    int floor_card_h = 0;
    int placed_x = 0;
    int placed_y = 0;
    bool have_placement = false;
    double scale = 1.0;
    guint x11_watch = 0;
    OverlayThemeColor color = OverlayThemeColor::kWhite;
    OverlayPosition position = OverlayPosition::kCenter;
    std::string text;
    bool show_progress = false;
    double progress = 1.0;
    bool mapped = false;
    guint hide_timeout = 0;
    guint countdown_source = 0;
    guint partial_source = 0;
    std::string pending_partial;
    cairo_surface_t* back_buffer = nullptr;
    int back_w = 0;
    int back_h = 0;
    std::function<void()> on_complete;

    bool OpenDisplay() {
        if (display) return true;
        display = XOpenDisplay(nullptr);
        if (!display) {
            LogApp("Overlay: no X11 display; cannot show a focus-safe overlay");
            return false;
        }
        screen = DefaultScreen(display);
        scale = DesktopScale();
        LogApp("Overlay scale=" + std::to_string(scale));
        XVisualInfo info;
        if (!XMatchVisualInfo(display, screen, 32, TrueColor, &info)) {
            LogApp("Overlay: no 32-bit visual for transparency");
            XCloseDisplay(display);
            display = nullptr;
            return false;
        }
        visual = info.visual;
        depth = info.depth;
        colormap = XCreateColormap(display, RootWindow(display, screen), visual, AllocNone);
        x11_watch = g_unix_fd_add(ConnectionNumber(display), G_IO_IN,
            +[](gint, GIOCondition, gpointer data) -> gboolean {
                static_cast<Impl*>(data)->DrainEvents();
                return G_SOURCE_CONTINUE;
            }, this);
        return true;
    }

    void ApplyFixedWindowSize() {
        win_w = std::max(1, static_cast<int>(std::lround(kMaxLogicalW * scale)));
        win_h = std::max(1, static_cast<int>(std::lround(kMaxLogicalH * scale)));
    }

    void EnsureWindow() {
        if (window || !OpenDisplay()) return;
        ApplyFixedWindowSize();
        XSetWindowAttributes attrs{};
        attrs.colormap = colormap;
        attrs.border_pixel = 0;
        attrs.background_pixel = 0;
        attrs.override_redirect = True;
        attrs.event_mask = ExposureMask | StructureNotifyMask;
        window = XCreateWindow(display, RootWindow(display, screen), 0, 0, win_w, win_h, 0, depth,
                               InputOutput, visual,
                               CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask,
                               &attrs);
        Atom window_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
        Atom notification = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
        XChangeProperty(display, window, window_type, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&notification), 1);
        XShapeCombineRectangles(display, window, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
        XStoreName(display, window, "VoiceStick Overlay");
    }

    void ResetGeometry() {
        floor_card_w = 0;
        floor_card_h = 0;
    }

    void CardOrigin(double& card_x, double& card_y) const {
        const double max_w = kMaxLogicalW;
        const double max_h = kMaxLogicalH;
        switch (position) {
        case OverlayPosition::kTopLeft:
            card_x = 0;
            card_y = 0;
            break;
        case OverlayPosition::kTopRight:
            card_x = max_w - card_w;
            card_y = 0;
            break;
        case OverlayPosition::kBottomLeft:
            card_x = 0;
            card_y = max_h - card_h;
            break;
        case OverlayPosition::kBottomRight:
            card_x = max_w - card_w;
            card_y = max_h - card_h;
            break;
        case OverlayPosition::kCenter:
        default:
            card_x = (max_w - card_w) / 2.0;
            card_y = (max_h - card_h) / 2.0;
            break;
        }
    }

    void PlaceWindow() {
        if (!display || !window) return;
        ApplyFixedWindowSize();
        const int screen_w = DisplayWidth(display, screen);
        const int screen_h = DisplayHeight(display, screen);
        const int margin = static_cast<int>(24 * scale);
        const int top = static_cast<int>(48 * scale);
        int x = (screen_w - win_w) / 2;
        int y = (screen_h - win_h) / 2;
        switch (position) {
        case OverlayPosition::kTopLeft:
            x = margin;
            y = top;
            break;
        case OverlayPosition::kTopRight:
            x = screen_w - win_w - margin;
            y = top;
            break;
        case OverlayPosition::kBottomLeft:
            x = margin;
            y = screen_h - win_h - top;
            break;
        case OverlayPosition::kBottomRight:
            x = screen_w - win_w - margin;
            y = screen_h - win_h - top;
            break;
        case OverlayPosition::kCenter:
        default:
            break;
        }
        x = std::max(0, x);
        y = std::max(0, y);
        if (have_placement && placed_x == x && placed_y == y) return;
        XMoveResizeWindow(display, window, x, y, win_w, win_h);
        placed_x = x;
        placed_y = y;
        have_placement = true;
    }

    void MeasurePango(const std::string& value, int wrap_width, int& text_w, int& text_h) {
        cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
        cairo_t* cr = cairo_create(scratch);
        PangoLayout* layout = pango_cairo_create_layout(cr);
        if (wrap_width > 0) {
            pango_layout_set_width(layout, wrap_width * PANGO_SCALE);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        } else {
            pango_layout_set_width(layout, -1);
        }
        auto* font = pango_font_description_from_string("Sans 18");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_text(layout, value.c_str(), -1);
        pango_layout_get_pixel_size(layout, &text_w, &text_h);
        g_object_unref(layout);
        cairo_destroy(cr);
        cairo_surface_destroy(scratch);
    }

    void Measure(const std::string& value) {
        if (!display) return;
        // Match Windows/macOS: measure a single line, snap to 1/3, 2/3, or
        // full width, and wrap only after the text exceeds the maximum.
        const int max_text = kMaxLogicalW - kPad;
        const int one_third = std::max(1, max_text / 3);
        const int two_thirds = std::max(one_third, (max_text * 2) / 3);
        int single_w = 0, single_h = 0;
        MeasurePango(value, 0, single_w, single_h);
        text_box_w = max_text;
        if (single_w <= one_third) {
            text_box_w = one_third;
        } else if (single_w <= two_thirds) {
            text_box_w = two_thirds;
        }
        should_wrap = single_w > max_text;
        int text_h = single_h;
        if (should_wrap) {
            int wrapped_w = 0;
            MeasurePango(value, text_box_w, wrapped_w, text_h);
        }
        card_w = std::clamp(text_box_w + kPad, kMinLogicalW, kMaxLogicalW);
        card_h = std::clamp(text_h + (show_progress ? 56 : 40), kMinLogicalH, kMaxLogicalH);
        if (floor_card_w > 0) {
            card_w = std::max(card_w, floor_card_w);
            card_h = std::max(card_h, floor_card_h);
        }
        floor_card_w = card_w;
        floor_card_h = card_h;
        ApplyFixedWindowSize();
    }

    void Draw() {
        if (!display || !window || !mapped) return;
        if (!back_buffer || back_w != win_w || back_h != win_h) {
            if (back_buffer) cairo_surface_destroy(back_buffer);
            back_buffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, win_w, win_h);
            back_w = win_w;
            back_h = win_h;
        }
        cairo_surface_t* image = back_buffer;
        cairo_t* cr = cairo_create(image);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_scale(cr, scale, scale);
        double card_x = 0;
        double card_y = 0;
        CardOrigin(card_x, card_y);
        const auto theme = ThemeColor(color);
        RoundRect(cr, card_x + 0.5, card_y + 0.5, card_w - 1.0, card_h - 1.0, 18);
        cairo_set_source_rgba(cr, theme.r, theme.g, theme.b, theme.a);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.08);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);

        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_width(layout, text_box_w * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        auto* font = pango_font_description_from_string("Sans 18");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_text(layout, text.c_str(), -1);
        int text_w = 0, text_h = 0;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);
        const double bar_h = show_progress ? 8.0 : 0.0;
        const double content_h = text_h + (show_progress ? 16.0 + bar_h : 0);
        const double text_y = card_y + (card_h - content_h) / 2.0;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.68);
        cairo_move_to(cr, card_x + 24, text_y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);

        if (show_progress) {
            const double bar_y = text_y + text_h + 14;
            const double bar_w = card_w - 48;
            RoundRect(cr, card_x + 24, bar_y, bar_w, bar_h, 3);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.10);
            cairo_fill(cr);
            RoundRect(cr, card_x + 24, bar_y, bar_w * std::clamp(progress, 0.0, 1.0), bar_h, 3);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.34);
            cairo_fill(cr);
        }
        cairo_destroy(cr);

        cairo_surface_t* xlib = cairo_xlib_surface_create(display, window, visual, win_w, win_h);
        cairo_t* dest = cairo_create(xlib);
        cairo_set_operator(dest, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(dest, image, 0, 0);
        cairo_paint(dest);
        cairo_destroy(dest);
        cairo_surface_destroy(xlib);
        XFlush(display);
    }

    void DrainEvents() {
        if (!display) return;
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == Expose) Draw();
        }
    }

    void ShowText(const std::string& value, bool progress_visible) {
        if (!OpenDisplay()) return;
        EnsureWindow();
        text = value.empty() ? "Listening…" : value;
        show_progress = progress_visible;
        Measure(text);
        PlaceWindow();
        if (!mapped) {
            XMapRaised(display, window);
            mapped = true;
        }
        Draw();
    }

    void HideWindow() {
        CancelTimers();
        ResetGeometry();
        if (display && window && mapped) {
            XUnmapWindow(display, window);
            XFlush(display);
            mapped = false;
        }
    }

    void CancelTimers() {
        if (hide_timeout) {
            g_source_remove(hide_timeout);
            hide_timeout = 0;
        }
        if (countdown_source) {
            g_source_remove(countdown_source);
            countdown_source = 0;
        }
        if (partial_source) {
            g_source_remove(partial_source);
            partial_source = 0;
        }
        pending_partial.clear();
    }

    void Shutdown() {
        CancelTimers();
        if (x11_watch) {
            g_source_remove(x11_watch);
            x11_watch = 0;
        }
        if (back_buffer) {
            cairo_surface_destroy(back_buffer);
            back_buffer = nullptr;
        }
        if (display && window) {
            XDestroyWindow(display, window);
            window = 0;
        }
        if (display && colormap) {
            XFreeColormap(display, colormap);
            colormap = 0;
        }
        if (display) {
            XCloseDisplay(display);
            display = nullptr;
        }
    }
};

OverlayWindow::OverlayWindow(GtkApplication*) : impl_(std::make_unique<Impl>()) {}

OverlayWindow::~OverlayWindow() {
    impl_->Shutdown();
}

void OverlayWindow::SetThemeColor(OverlayThemeColor color) {
    impl_->color = color;
    if (impl_->mapped) impl_->Draw();
}

void OverlayWindow::SetPosition(OverlayPosition position) {
    impl_->position = position;
    if (impl_->mapped) {
        impl_->PlaceWindow();
        impl_->Draw();
    }
}

void OverlayWindow::ShowListening() {
    impl_->CancelTimers();
    impl_->ResetGeometry();
    impl_->ShowText("Listening…", false);
}

void OverlayWindow::ShowPartial(const std::string& text) {
    impl_->pending_partial = text.empty() ? "Listening…" : text;
    if (impl_->partial_source) return;
    impl_->partial_source = g_timeout_add(100, [](gpointer data) -> gboolean {
        auto* self = static_cast<OverlayWindow::Impl*>(data);
        self->partial_source = 0;
        self->ShowText(self->pending_partial, false);
        return G_SOURCE_REMOVE;
    }, impl_.get());
}

void OverlayWindow::ShowFinalCountdown(const std::string& text,
                                       int duration_ms,
                                       std::function<void()> on_complete) {
    impl_->CancelTimers();
    impl_->on_complete = std::move(on_complete);
    impl_->progress = 1.0;
    impl_->ShowText(text.empty() ? "No speech" : text, true);
    if (duration_ms <= 0) {
        auto complete = std::move(impl_->on_complete);
        impl_->HideWindow();
        if (complete) complete();
        return;
    }
    const int ticks = std::max(1, duration_ms / 50);
    struct CountdownState {
        Impl* self;
        int left;
        int total;
    };
    auto* state = new CountdownState{impl_.get(), ticks, ticks};
    impl_->countdown_source = g_timeout_add(50, [](gpointer data) -> gboolean {
        auto* countdown = static_cast<CountdownState*>(data);
        --countdown->left;
        countdown->self->progress = std::max(0.0, countdown->left / static_cast<double>(countdown->total));
        countdown->self->Draw();
        if (countdown->left <= 0) {
            countdown->self->countdown_source = 0;
            auto complete = std::move(countdown->self->on_complete);
            countdown->self->HideWindow();
            delete countdown;
            if (complete) complete();
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }, state);
}

void OverlayWindow::ShowPausedFinal(const std::string& text) {
    impl_->CancelTimers();
    impl_->ShowText((text.empty() ? "No speech" : text) + "\nFront: Send    Side: Cancel", false);
}

void OverlayWindow::ShowError(const std::string& text, std::function<void()> on_complete) {
    impl_->CancelTimers();
    impl_->on_complete = std::move(on_complete);
    impl_->ShowText(text.empty() ? "Unknown ASR error" : text, false);
    impl_->hide_timeout = g_timeout_add(2000, [](gpointer data) -> gboolean {
        auto* self = static_cast<Impl*>(data);
        self->hide_timeout = 0;
        auto complete = std::move(self->on_complete);
        self->HideWindow();
        if (complete) complete();
        return G_SOURCE_REMOVE;
    }, impl_.get());
}

void OverlayWindow::ShowMessage(const std::string& text) {
    impl_->CancelTimers();
    impl_->ShowText(text, false);
}

void OverlayWindow::Hide(std::function<void()> on_hidden) {
    impl_->HideWindow();
    if (on_hidden) on_hidden();
}

} // namespace voicestick
