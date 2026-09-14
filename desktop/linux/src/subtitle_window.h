#pragma once

#include "app_config.h"

#include <string>

namespace voicestick {

class SubtitleWindow {
public:
    SubtitleWindow();
    ~SubtitleWindow();

    void ShowLine(const std::string& text, const std::string& device_id, OverlayThemeColor color);
    void HideAll();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace voicestick
