#pragma once

#include "app_config.h"

#include <adwaita.h>
#include <functional>
#include <string>

namespace voicestick {

class PreferencesWindow {
public:
    PreferencesWindow(GtkWindow* parent, AppConfig config);
    void Present();
    std::function<void(AppConfig)> on_config_changed;

private:
    void Build();
    void Load();
    void Save();
    void UpdateAsrWidgets();

    AppConfig config_;
    GtkWindow* window_ = nullptr;
    GtkWidget* provider_row_ = nullptr;
    GtkWidget* api_key_row_ = nullptr;
    GtkWidget* resource_row_ = nullptr;
    GtkWidget* hotwords_row_ = nullptr;
    GtkWidget* trial_button_ = nullptr;
    GtkWidget* llm_url_row_ = nullptr;
    GtkWidget* llm_key_row_ = nullptr;
    GtkWidget* llm_model_row_ = nullptr;
    GtkWidget* debug_row_ = nullptr;
    GtkWidget* debug_dir_row_ = nullptr;
};

} // namespace voicestick
