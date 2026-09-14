#include "onboarding_dialog.h"

#include "voice_stick_cloud_api_linux.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace voicestick {

OnboardingDialog::OnboardingDialog(GtkWindow* parent, BleCentralBluez* ble, AppConfig config)
    : ble_(ble), config_(std::move(config)) {
    window_ = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(window_, "Welcome to VoiceStick");
    gtk_window_set_default_size(window_, 520, 480);
    if (parent) gtk_window_set_transient_for(window_, parent);
    gtk_window_set_modal(window_, TRUE);

    navigation_ = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
    adw_window_set_content(ADW_WINDOW(window_), GTK_WIDGET(navigation_));
    ShowPairPage();

    g_signal_connect(window_, "close-request", reinterpret_cast<GCallback>(+[](GtkWindow*, gpointer data) -> gboolean {
        auto* self = static_cast<OnboardingDialog*>(data);
        if (!self->finished_ && self->on_cancelled) self->on_cancelled();
        return FALSE;
    }), this);
}

OnboardingDialog::~OnboardingDialog() {
    if (ble_) ble_->StopPairingScan();
}

void OnboardingDialog::Present() {
    gtk_window_present(window_);
}

void OnboardingDialog::ShowPairPage() {
    auto* page = adw_navigation_page_new(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "Pair Device");
    auto* toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
    auto* status = gtk_label_new("Turn on your StickS3 and select VS-XXXX.");
    pair_status_ = status;
    auto* list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "boxed-list");
    auto* next = gtk_button_new_with_label("Next");
    gtk_widget_set_sensitive(next, FALSE);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_box_append(GTK_BOX(box), status);
    gtk_box_append(GTK_BOX(box), list);
    gtk_box_append(GTK_BOX(box), next);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
    adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(page), toolbar);
    adw_navigation_view_push(navigation_, ADW_NAVIGATION_PAGE(page));

    if (ble_) {
        ble_->on_scan_updated = [list, next, this](const std::vector<ScannedBleDevice>& devices) {
            while (auto* child = gtk_widget_get_first_child(list)) gtk_list_box_remove(GTK_LIST_BOX(list), child);
            for (const auto& device : devices) {
                auto* row = adw_action_row_new();
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), device.name.c_str());
                gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(row));
            }
            gtk_widget_set_sensitive(next, !devices.empty());
            g_object_set_data_full(G_OBJECT(list), "devices",
                                   new std::vector<ScannedBleDevice>(devices),
                                   [](gpointer p) { delete static_cast<std::vector<ScannedBleDevice>*>(p); });
        };
        ble_->StartPairingScan();
    }

    g_signal_connect(next, "clicked", reinterpret_cast<GCallback>(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<OnboardingDialog*>(data);
        if (auto* devices = static_cast<std::vector<ScannedBleDevice>*>(
                g_object_get_data(G_OBJECT(self->pair_status_), "unused"))) {
            (void)devices;
        }
        auto* list = gtk_widget_get_parent(self->pair_status_);
        (void)list;
        if (self->ble_) {
            auto scanned = self->ble_->ScannedDevices();
            if (!scanned.empty()) self->selected_device_ = scanned.front();
        }
        if (self->ble_) self->ble_->StopPairingScan();
        self->ShowAsrPage();
    }), this);
}

void OnboardingDialog::ShowAsrPage() {
    auto* page = adw_navigation_page_new(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "ASR Key");
    auto* toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
    auto* group = adw_preferences_group_new();
    provider_row_ = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(provider_row_), "Provider");
    auto* model = gtk_string_list_new((const char*[]){"VoiceStick Cloud", "Volcengine", nullptr});
    adw_combo_row_set_model(ADW_COMBO_ROW(provider_row_), G_LIST_MODEL(model));
    api_key_row_ = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(api_key_row_), "API Key");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), provider_row_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), api_key_row_);
    trial_button_ = gtk_button_new_with_label("Apply Trial");
    auto* next = gtk_button_new_with_label("Next");
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_box_append(GTK_BOX(box), group);
    gtk_box_append(GTK_BOX(box), trial_button_);
    gtk_box_append(GTK_BOX(box), next);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
    adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(page), toolbar);
    adw_navigation_view_push(navigation_, ADW_NAVIGATION_PAGE(page));

    g_signal_connect(trial_button_, "clicked", reinterpret_cast<GCallback>(+[](GtkButton*, gpointer data) {
        static_cast<OnboardingDialog*>(data)->ApplyTrial();
    }), this);
    g_signal_connect(provider_row_, "notify::selected",
                     reinterpret_cast<GCallback>(+[](GObject*, GParamSpec*, gpointer data) {
                         static_cast<OnboardingDialog*>(data)->UpdateTrialButton();
                     }), this);
    g_signal_connect(api_key_row_, "notify::text",
                     reinterpret_cast<GCallback>(+[](GObject*, GParamSpec*, gpointer data) {
                         static_cast<OnboardingDialog*>(data)->UpdateTrialButton();
                     }), this);
    UpdateTrialButton();
    g_signal_connect(next, "clicked", reinterpret_cast<GCallback>(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<OnboardingDialog*>(data);
        const auto key = gtk_editable_get_text(GTK_EDITABLE(self->api_key_row_));
        if (adw_combo_row_get_selected(ADW_COMBO_ROW(self->provider_row_)) == 0) {
            self->config_.asr_provider = AsrProvider::kVoiceStickCloud;
            self->config_.voicestick_api_key = key ? key : "";
        } else {
            self->config_.asr_provider = AsrProvider::kVolcengine;
            self->config_.volcengine_api_key = key ? key : "";
        }
        self->ShowReadyPage();
    }), this);
}

void OnboardingDialog::ShowReadyPage() {
    auto* page = adw_navigation_page_new(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "Ready");
    auto* toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
    auto* label = gtk_label_new(
        "Recognized text is copied to the clipboard.\n"
        "VoiceStick will try to insert it into the focused field.\n"
        "If that fails, press Ctrl+V.");
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    auto* finish = gtk_button_new_with_label("Finish");
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), finish);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
    adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(page), toolbar);
    adw_navigation_view_push(navigation_, ADW_NAVIGATION_PAGE(page));
    g_signal_connect(finish, "clicked", reinterpret_cast<GCallback>(+[](GtkButton* button, gpointer data) {
        gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
        static_cast<OnboardingDialog*>(data)->Finish();
    }), this);
}

void OnboardingDialog::ApplyTrial() {
    const auto device_id = selected_device_ ? selected_device_->device_id : "";
    auto result = ApplyVoiceStickCloudTrialApiKey(config_.voicestick_cloud_url, device_id);
    if (!result.ok()) {
        gtk_label_set_text(GTK_LABEL(pair_status_), result.error.c_str());
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(api_key_row_), result.api_key.c_str());
    if (!result.url.empty()) config_.voicestick_cloud_url = result.url;
    UpdateTrialButton();
}

void OnboardingDialog::UpdateTrialButton() {
    if (!trial_button_ || !provider_row_ || !api_key_row_) return;
    const bool is_cloud = adw_combo_row_get_selected(ADW_COMBO_ROW(provider_row_)) == 0;
    const auto* key = gtk_editable_get_text(GTK_EDITABLE(api_key_row_));
    std::string trimmed = key ? key : "";
    trimmed.erase(trimmed.begin(), std::find_if_not(trimmed.begin(), trimmed.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }));
    trimmed.erase(std::find_if_not(trimmed.rbegin(), trimmed.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base(), trimmed.end());
    gtk_widget_set_visible(trial_button_, is_cloud && trimmed.empty());
}

void OnboardingDialog::Finish() {
    finished_ = true;
    if (selected_device_) {
        PairedDeviceEntry entry;
        entry.device_id = selected_device_->device_id;
        entry.bluetooth_address = selected_device_->bluetooth_address;
        entry.address_kind = selected_device_->address_kind;
        entry.name = selected_device_->name;
        config_.SavePairedDevice(entry);
    } else {
        config_.Save();
    }
    auto finished = std::move(on_finished);
    gtk_window_close(window_);
    if (finished) finished(config_);
}

} // namespace voicestick
