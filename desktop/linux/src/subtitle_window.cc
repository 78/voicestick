#include "subtitle_window.h"

#include <gtk/gtk.h>

#include <map>
#include <string>

namespace voicestick {

namespace {

const char* ThemeCss(OverlayThemeColor color) {
    switch (color) {
    case OverlayThemeColor::kPink: return "#FFD6E6";
    case OverlayThemeColor::kGreen: return "#D6F2D6";
    case OverlayThemeColor::kYellow: return "#FFF0B8";
    case OverlayThemeColor::kBlue: return "#D1E8FF";
    case OverlayThemeColor::kPurple: return "#E6D6FF";
    case OverlayThemeColor::kWhite:
    default:
        return "#FCFCFC";
    }
}

} // namespace

struct SubtitleWindow::Impl {
    GtkWindow* window = nullptr;
    GtkBox* list = nullptr;
    std::map<std::string, GtkWidget*> rows;
    std::map<std::string, guint> hide_timeouts;

    void Ensure() {
        if (window) return;
        window = GTK_WINDOW(gtk_window_new());
        gtk_window_set_decorated(window, FALSE);
        gtk_window_set_resizable(window, FALSE);
        gtk_window_set_title(window, "VoiceStick Subtitles");
        gtk_widget_set_can_target(GTK_WIDGET(window), FALSE);
        list = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
        gtk_widget_set_margin_top(GTK_WIDGET(list), 12);
        gtk_widget_set_margin_bottom(GTK_WIDGET(list), 24);
        gtk_widget_set_margin_start(GTK_WIDGET(list), 24);
        gtk_widget_set_margin_end(GTK_WIDGET(list), 24);
        gtk_window_set_child(window, GTK_WIDGET(list));
        gtk_window_set_default_size(window, 640, 80);
    }
};

SubtitleWindow::SubtitleWindow() : impl_(new Impl) {}

SubtitleWindow::~SubtitleWindow() {
    HideAll();
    if (impl_->window) gtk_window_destroy(impl_->window);
    delete impl_;
}

void SubtitleWindow::ShowLine(const std::string& text, const std::string& device_id,
                              OverlayThemeColor color) {
    impl_->Ensure();
    GtkWidget* row = nullptr;
    if (auto it = impl_->rows.find(device_id); it != impl_->rows.end()) {
        row = it->second;
        if (auto* label = GTK_LABEL(gtk_widget_get_first_child(row))) {
            gtk_label_set_text(label, text.c_str());
        }
    } else {
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        auto* swatch = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_size_request(swatch, 8, 24);
        auto* provider = gtk_css_provider_new();
        const std::string css = "* { background: " + std::string(ThemeCss(color)) + "; border-radius: 4px; }";
        gtk_css_provider_load_from_string(provider, css.c_str());
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
        auto* label = gtk_label_new(text.c_str());
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_box_append(GTK_BOX(row), swatch);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(impl_->list, row);
        impl_->rows[device_id] = row;
    }
    if (auto timeout = impl_->hide_timeouts[device_id]; timeout) {
        g_source_remove(timeout);
    }
    auto* key = new std::string(device_id);
    impl_->hide_timeouts[device_id] = g_timeout_add(7000, [](gpointer data) -> gboolean {
        auto* pair = static_cast<std::pair<Impl*, std::string*>*>(data);
        if (auto it = pair->first->rows.find(*pair->second); it != pair->first->rows.end()) {
            gtk_widget_unparent(it->second);
            pair->first->rows.erase(it);
        }
        pair->first->hide_timeouts.erase(*pair->second);
        if (pair->first->rows.empty() && pair->first->window) {
            gtk_widget_set_visible(GTK_WIDGET(pair->first->window), FALSE);
        }
        delete pair->second;
        delete pair;
        return G_SOURCE_REMOVE;
    }, new std::pair<Impl*, std::string*>{impl_, key});
    gtk_widget_set_visible(GTK_WIDGET(impl_->window), TRUE);
    gtk_window_present(impl_->window);
}

void SubtitleWindow::HideAll() {
    for (auto& [_, timeout] : impl_->hide_timeouts) {
        if (timeout) g_source_remove(timeout);
    }
    impl_->hide_timeouts.clear();
    for (auto& [_, row] : impl_->rows) {
        gtk_widget_unparent(row);
    }
    impl_->rows.clear();
    if (impl_->window) gtk_widget_set_visible(GTK_WIDGET(impl_->window), FALSE);
}

} // namespace voicestick
