#include "preferences_window.h"

#include "voice_stick_cloud_api_linux.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace voicestick {

namespace {

GtkStringList* StringList(const std::vector<std::string>& values) {
    std::vector<const char*> items;
    for (const auto& value : values) items.push_back(value.c_str());
    items.push_back(nullptr);
    return gtk_string_list_new(items.data());
}

std::string EditableText(GtkWidget* widget) {
    const auto* text = gtk_editable_get_text(GTK_EDITABLE(widget));
    return text ? text : "";
}

} // namespace

PreferencesWindow::PreferencesWindow(GtkWindow* parent, AppConfig config) : config_(std::move(config)) {
    window_ = GTK_WINDOW(adw_preferences_window_new());
    gtk_window_set_title(window_, "VoiceStick Settings");
    gtk_window_set_default_size(window_, 560, 640);
    if (parent) gtk_window_set_transient_for(window_, parent);
    Build();
}

void PreferencesWindow::Present() {
    config_ = AppConfig::Load();
    Load();
    gtk_window_present(window_);
}

void PreferencesWindow::Build() {
    auto* asr = adw_preferences_page_new();
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(asr), "ASR");
    auto* asr_group = adw_preferences_group_new();
    provider_row_ = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(provider_row_), "Provider");
    adw_combo_row_set_model(ADW_COMBO_ROW(provider_row_),
                            G_LIST_MODEL(gtk_string_list_new((const char*[]){"VoiceStick Cloud", "Volcengine", nullptr})));
    api_key_row_ = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(api_key_row_), "API Key");
    resource_row_ = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(resource_row_), "Resource ID");
    adw_combo_row_set_model(ADW_COMBO_ROW(resource_row_), G_LIST_MODEL(StringList(AppConfig::SupportedResourceIds())));
    hotwords_row_ = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(hotwords_row_), "Hotwords");
    trial_button_ = gtk_button_new_with_label("Apply Trial");
    gtk_widget_add_css_class(trial_button_, "suggested-action");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(asr_group), provider_row_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(asr_group), api_key_row_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(asr_group), resource_row_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(asr_group), hotwords_row_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(asr_group), trial_button_);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(asr), ADW_PREFERENCES_GROUP(asr_group));
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(window_), ADW_PREFERENCES_PAGE(asr));

    auto* llm = adw_preferences_page_new();
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(llm), "LLM");
    auto* llm_group = adw_preferences_group_new();
    llm_url_row_ = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(llm_url_row_), "Base URL");
    llm_key_row_ = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(llm_key_row_), "API Key");
    llm_model_row_ = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(llm_model_row_), "Model");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(llm_group), llm_url_row_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(llm_group), llm_key_row_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(llm_group), llm_model_row_);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(llm), ADW_PREFERENCES_GROUP(llm_group));
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(window_), ADW_PREFERENCES_PAGE(llm));

    auto* debug = adw_preferences_page_new();
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(debug), "Debug");
    auto* debug_group = adw_preferences_group_new();
    debug_row_ = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(debug_row_), "Save debug audio files");
    debug_dir_row_ = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(debug_dir_row_), "Audio folder");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(debug_group), debug_row_);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(debug_group), debug_dir_row_);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(debug), ADW_PREFERENCES_GROUP(debug_group));
    adw_preferences_window_add(ADW_PREFERENCES_WINDOW(window_), ADW_PREFERENCES_PAGE(debug));

    g_signal_connect(provider_row_, "notify::selected",
                     reinterpret_cast<GCallback>(+[](GObject*, GParamSpec*, gpointer data) {
                         static_cast<PreferencesWindow*>(data)->UpdateAsrWidgets();
                     }), this);
    g_signal_connect(api_key_row_, "notify::text",
                     reinterpret_cast<GCallback>(+[](GObject*, GParamSpec*, gpointer data) {
                         static_cast<PreferencesWindow*>(data)->UpdateAsrWidgets();
                     }), this);
    g_signal_connect(trial_button_, "clicked", reinterpret_cast<GCallback>(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<PreferencesWindow*>(data);
        if (adw_combo_row_get_selected(ADW_COMBO_ROW(self->provider_row_)) != 0) return;
        auto result = ApplyVoiceStickCloudTrialApiKey(self->config_.voicestick_cloud_url,
                                                      self->config_.paired_device_ids.empty()
                                                          ? ""
                                                          : self->config_.paired_device_ids.front());
        if (!result.ok()) return;
        gtk_editable_set_text(GTK_EDITABLE(self->api_key_row_), result.api_key.c_str());
        if (!result.url.empty()) self->config_.voicestick_cloud_url = result.url;
        self->UpdateAsrWidgets();
    }), this);
    g_signal_connect(window_, "close-request", reinterpret_cast<GCallback>(+[](GtkWindow*, gpointer data) -> gboolean {
        static_cast<PreferencesWindow*>(data)->Save();
        return FALSE;
    }), this);
}

void PreferencesWindow::Load() {
    adw_combo_row_set_selected(ADW_COMBO_ROW(provider_row_),
                               config_.asr_provider == AsrProvider::kVolcengine ? 1 : 0);
    gtk_editable_set_text(GTK_EDITABLE(api_key_row_), config_.ActiveApiKey().c_str());
    const auto& ids = AppConfig::SupportedResourceIds();
    auto it = std::find(ids.begin(), ids.end(), config_.resource_id);
    adw_combo_row_set_selected(ADW_COMBO_ROW(resource_row_),
                               it == ids.end() ? 0 : static_cast<guint>(it - ids.begin()));
    std::string hotwords;
    for (std::size_t i = 0; i < config_.asr_hotwords.size(); ++i) {
        if (i) hotwords += ",";
        hotwords += config_.asr_hotwords[i];
    }
    gtk_editable_set_text(GTK_EDITABLE(hotwords_row_), hotwords.c_str());
    gtk_editable_set_text(GTK_EDITABLE(llm_url_row_), config_.llm_base_url.c_str());
    gtk_editable_set_text(GTK_EDITABLE(llm_key_row_), config_.llm_api_key.c_str());
    gtk_editable_set_text(GTK_EDITABLE(llm_model_row_), config_.llm_model.c_str());
    adw_switch_row_set_active(ADW_SWITCH_ROW(debug_row_), config_.debug_audio_cache);
    gtk_editable_set_text(GTK_EDITABLE(debug_dir_row_), config_.debug_audio_directory.string().c_str());
    UpdateAsrWidgets();
}

void PreferencesWindow::UpdateAsrWidgets() {
    const bool is_cloud = adw_combo_row_get_selected(ADW_COMBO_ROW(provider_row_)) == 0;
    auto key = EditableText(api_key_row_);
    key.erase(key.begin(), std::find_if_not(key.begin(), key.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }));
    key.erase(std::find_if_not(key.rbegin(), key.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base(), key.end());
    gtk_widget_set_visible(resource_row_, !is_cloud);
    gtk_widget_set_visible(trial_button_, is_cloud && key.empty());
}

void PreferencesWindow::Save() {
    auto latest = AppConfig::Load();
    latest.asr_provider = adw_combo_row_get_selected(ADW_COMBO_ROW(provider_row_)) == 1
                              ? AsrProvider::kVolcengine
                              : AsrProvider::kVoiceStickCloud;
    const auto key = EditableText(api_key_row_);
    if (latest.asr_provider == AsrProvider::kVoiceStickCloud) {
        latest.voicestick_api_key = key;
    } else {
        latest.volcengine_api_key = key;
    }
    if (!config_.voicestick_cloud_url.empty()) {
        latest.voicestick_cloud_url = config_.voicestick_cloud_url;
    }
    const auto resource_index = adw_combo_row_get_selected(ADW_COMBO_ROW(resource_row_));
    const auto& ids = AppConfig::SupportedResourceIds();
    if (resource_index < ids.size()) latest.resource_id = ids[resource_index];
    latest.asr_hotwords = ParseHotwordList(EditableText(hotwords_row_).c_str());
    latest.llm_base_url = EditableText(llm_url_row_);
    latest.llm_api_key = EditableText(llm_key_row_);
    latest.llm_model = EditableText(llm_model_row_);
    latest.debug_audio_cache = adw_switch_row_get_active(ADW_SWITCH_ROW(debug_row_));
    latest.debug_audio_directory = EditableText(debug_dir_row_);
    latest.Save();
    config_ = std::move(latest);
    if (on_config_changed) on_config_changed(config_);
}

} // namespace voicestick
