#pragma once

#include "app_config.h"
#include "ble_central_bluez.h"

#include <adwaita.h>
#include <functional>
#include <optional>

namespace voicestick {

class OnboardingDialog {
public:
    OnboardingDialog(GtkWindow* parent, BleCentralBluez* ble, AppConfig config);
    ~OnboardingDialog();

    void Present();
    std::function<void(AppConfig)> on_finished;
    std::function<void()> on_cancelled;

private:
    void ShowPairPage();
    void ShowAsrPage();
    void ShowReadyPage();
    void ApplyTrial();
    void Finish();
    void UpdateTrialButton();

    GtkWindow* window_ = nullptr;
    AdwNavigationView* navigation_ = nullptr;
    BleCentralBluez* ble_ = nullptr;
    AppConfig config_;
    GtkWidget* pair_status_ = nullptr;
    GtkWidget* provider_row_ = nullptr;
    GtkWidget* api_key_row_ = nullptr;
    GtkWidget* trial_button_ = nullptr;
    std::optional<ScannedBleDevice> selected_device_;
    bool finished_ = false;
};

} // namespace voicestick
