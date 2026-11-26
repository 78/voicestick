#pragma once

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <utility>

namespace voicestick {

// Start and Stop run on the UI thread. WinRT calls can dispatch nested UI work.
class BleAdvertisementScan {
    using Watcher = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
    using ScanningMode = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;

public:
    BleAdvertisementScan() = default;
    BleAdvertisementScan(const BleAdvertisementScan&) = delete;
    BleAdvertisementScan& operator=(const BleAdvertisementScan&) = delete;

    template<typename Handler>
    bool Start(Handler&& handler) {
        const auto generation = ++generation_;
        StopWatcher();
        if (generation != generation_) return false;

        try {
            Watcher watcher;
            watcher.ScanningMode(ScanningMode::Active);
            const auto token = watcher.Received(std::forward<Handler>(handler));
            if (generation != generation_) {
                watcher.Received(token);
                return false;
            }
            watcher_ = watcher;
            received_token_ = token;
            watcher.Start();
        } catch (...) {
            if (generation != generation_) return false;
            StopWatcher();
            if (generation != generation_) return false;
            throw;
        }
        return generation == generation_;
    }

    void Stop() noexcept {
        ++generation_;
        StopWatcher();
    }

private:
    void StopWatcher() noexcept {
        // Detach the watcher and its token before either WinRT call can reenter.
        auto watcher = std::exchange(watcher_, nullptr);
        auto token = std::exchange(received_token_, {});
        if (!watcher) return;
        try {
            watcher.Received(token);
        } catch (...) {
        }
        try {
            watcher.Stop();
        } catch (...) {
        }
    }

    Watcher watcher_{nullptr};
    winrt::event_token received_token_{};
    std::uint64_t generation_ = 0;
};

} // namespace voicestick
