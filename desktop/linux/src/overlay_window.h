#pragma once

#include "app_config.h"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace voicestick {

class OverlayWindow {
public:
    explicit OverlayWindow(GtkApplication* application = nullptr);
    ~OverlayWindow();

    void SetThemeColor(OverlayThemeColor color);
    void SetPosition(OverlayPosition position);
    void ShowListening();
    void ShowPartial(const std::string& text);
    void ShowFinalCountdown(const std::string& text, int duration_ms, std::function<void()> on_complete);
    void ShowPausedFinal(const std::string& text);
    void ShowError(const std::string& text, std::function<void()> on_complete);
    void ShowMessage(const std::string& text);
    void Hide(std::function<void()> on_hidden = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voicestick
