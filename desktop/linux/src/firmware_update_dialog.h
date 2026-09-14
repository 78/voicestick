#pragma once

#include "voice_stick_coordinator.h"

#include <adwaita.h>
#include <functional>
#include <string>

namespace voicestick {

class FirmwareUpdateDialog {
public:
    explicit FirmwareUpdateDialog(GtkWindow* parent);
    void Present(const std::string& device_id, const std::string& current, const std::string& latest);
    void SetProgress(const FirmwareUpdateProgress& progress);
    void Finish(bool success, const std::string& message);
    std::function<void()> on_start;
    std::function<void()> on_cancel;

private:
    GtkWindow* window_ = nullptr;
    GtkWidget* status_ = nullptr;
    GtkWidget* bar_ = nullptr;
};

} // namespace voicestick
