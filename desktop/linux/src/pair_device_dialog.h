#pragma once

#include "ble_central_bluez.h"

#include <adwaita.h>
#include <functional>
#include <vector>

namespace voicestick {

class PairDeviceDialog {
public:
    PairDeviceDialog(GtkWindow* parent, BleCentralBluez* ble);
    ~PairDeviceDialog();

    void Present();
    std::function<void(ScannedBleDevice)> on_pair;

private:
    void Refresh(const std::vector<ScannedBleDevice>& devices);
    GtkWindow* window_ = nullptr;
    GtkListBox* list_ = nullptr;
    BleCentralBluez* ble_ = nullptr;
    std::vector<ScannedBleDevice> devices_;
};

} // namespace voicestick
