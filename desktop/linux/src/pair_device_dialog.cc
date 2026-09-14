#include "pair_device_dialog.h"

namespace voicestick {

PairDeviceDialog::PairDeviceDialog(GtkWindow* parent, BleCentralBluez* ble) : ble_(ble) {
    window_ = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(window_, "Pair Device");
    gtk_window_set_default_size(window_, 420, 420);
    if (parent) gtk_window_set_transient_for(window_, parent);
    gtk_window_set_modal(window_, TRUE);

    auto* toolbar = adw_toolbar_view_new();
    auto* header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    list_ = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(list_), "boxed-list");
    auto* clamp = adw_clamp_new();
    adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(list_));
    gtk_widget_set_margin_top(clamp, 16);
    gtk_widget_set_margin_bottom(clamp, 16);
    gtk_widget_set_margin_start(clamp, 16);
    gtk_widget_set_margin_end(clamp, 16);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), clamp);
    adw_window_set_content(ADW_WINDOW(window_), toolbar);

    g_signal_connect(list_, "row-activated", reinterpret_cast<GCallback>(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
        auto* self = static_cast<PairDeviceDialog*>(data);
        const auto index = gtk_list_box_row_get_index(row);
        if (index < 0 || static_cast<std::size_t>(index) >= self->devices_.size()) return;
        if (self->on_pair) self->on_pair(self->devices_[static_cast<std::size_t>(index)]);
        gtk_window_close(self->window_);
    }), this);

    g_signal_connect(window_, "close-request", reinterpret_cast<GCallback>(+[](GtkWindow*, gpointer data) -> gboolean {
        auto* self = static_cast<PairDeviceDialog*>(data);
        if (self->ble_) self->ble_->StopPairingScan();
        return FALSE;
    }), this);
}

PairDeviceDialog::~PairDeviceDialog() {
    if (ble_) ble_->StopPairingScan();
}

void PairDeviceDialog::Present() {
    if (ble_) {
        ble_->on_scan_updated = [this](const std::vector<ScannedBleDevice>& devices) {
            Refresh(devices);
        };
        ble_->StartPairingScan();
        Refresh(ble_->ScannedDevices());
    }
    gtk_window_present(window_);
}

void PairDeviceDialog::Refresh(const std::vector<ScannedBleDevice>& devices) {
    devices_ = devices;
    while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(list_))) {
        gtk_list_box_remove(list_, child);
    }
    if (devices_.empty()) {
        auto* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Scanning for VS-XXXX…");
        gtk_list_box_append(list_, GTK_WIDGET(row));
        return;
    }
    for (const auto& device : devices_) {
        auto* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), device.name.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    ("ID " + device.device_id + "   RSSI " + std::to_string(device.rssi)).c_str());
        gtk_list_box_append(list_, GTK_WIDGET(row));
    }
}

} // namespace voicestick
