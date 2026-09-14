#include "firmware_update_dialog.h"

namespace voicestick {

FirmwareUpdateDialog::FirmwareUpdateDialog(GtkWindow* parent) {
    window_ = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(window_, "Firmware Update");
    gtk_window_set_default_size(window_, 420, 220);
    if (parent) gtk_window_set_transient_for(window_, parent);
    gtk_window_set_modal(window_, TRUE);

    auto* toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
    status_ = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(status_), TRUE);
    bar_ = GTK_WIDGET(gtk_progress_bar_new());
    auto* start = gtk_button_new_with_label("Update");
    auto* cancel = gtk_button_new_with_label("Cancel");
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_box_append(GTK_BOX(box), status_);
    gtk_box_append(GTK_BOX(box), bar_);
    gtk_box_append(GTK_BOX(box), start);
    gtk_box_append(GTK_BOX(box), cancel);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
    adw_window_set_content(ADW_WINDOW(window_), toolbar);

    g_signal_connect(start, "clicked", reinterpret_cast<GCallback>(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<FirmwareUpdateDialog*>(data);
        if (self->on_start) self->on_start();
    }), this);
    g_signal_connect(cancel, "clicked", reinterpret_cast<GCallback>(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<FirmwareUpdateDialog*>(data);
        if (self->on_cancel) self->on_cancel();
        gtk_window_close(self->window_);
    }), this);
}

void FirmwareUpdateDialog::Present(const std::string& device_id,
                                   const std::string& current,
                                   const std::string& latest) {
    gtk_label_set_text(GTK_LABEL(status_),
                       ("Update VS-" + device_id + " from " + current + " to " + latest).c_str());
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar_), 0);
    gtk_window_present(window_);
}

void FirmwareUpdateDialog::SetProgress(const FirmwareUpdateProgress& progress) {
    if (progress.total_bytes > 0) {
        gtk_progress_bar_set_fraction(
            GTK_PROGRESS_BAR(bar_),
            static_cast<double>(progress.written_bytes) / progress.total_bytes);
    }
}

void FirmwareUpdateDialog::Finish(bool success, const std::string& message) {
    gtk_label_set_text(GTK_LABEL(status_), success ? "Update complete." : message.c_str());
}

} // namespace voicestick
