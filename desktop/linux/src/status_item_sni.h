#pragma once

#include "app_config.h"
#include "voice_stick_coordinator.h"

#include <functional>
#include <string>
#include <vector>

namespace voicestick {

class StatusItemSni {
public:
    struct DeviceRow {
        std::string id;
        std::string name;
        bool connected = false;
        OverlayThemeColor theme = OverlayThemeColor::kWhite;
        OverlayPosition position = OverlayPosition::kCenter;
        OutputProfile output;
        DeviceFirmwareInfo firmware;
    };

    StatusItemSni();
    ~StatusItemSni();

    bool Start();
    void Shutdown();

    void SetStatus(const std::string& status);
    void SetHasRecoverableInput(bool value);
    void SetInputOptions(InteractionMode mode, bool auto_enter, OutputTarget output, int countdown_ms);
    void SetDevices(const std::vector<DeviceRow>& devices);

    std::function<void()> on_restore;
    std::function<void()> on_pair;
    std::function<void()> on_preferences;
    std::function<void()> on_website;
    std::function<void()> on_quit;
    std::function<void(std::string)> on_forget;
    std::function<void(std::string)> on_update_firmware;
    std::function<void(OutputTarget)> on_output_target;
    std::function<void(bool)> on_auto_enter;
    std::function<void(InteractionMode)> on_interaction;
    std::function<void(int)> on_countdown;
    std::function<void(std::string, OverlayThemeColor)> on_theme;
    std::function<void(std::string, OverlayPosition)> on_position;
    std::function<void(std::string, TextTransform, std::string)> on_translation;

    struct Impl;

private:
    Impl* impl_;
};

} // namespace voicestick
