#pragma once

#include <cstdint>

namespace voicestick {

// Host-side shake detector running on Protocol v2 accel IMU stream (50 Hz).
// Matches the macOS ShakeDetector thresholds: three alternating acceleration
// delta peaks of at least 1100 mg, separated by >= 80 ms, within 900 ms.
class ShakeDetector {
public:
    static constexpr std::int32_t kMinDeltaMg = 1100;
    static constexpr std::int64_t kMinTurnUs = 80'000;
    static constexpr std::int64_t kMaxShakeUs = 900'000;

    void Reset() {
        has_previous_ = false;
        prev_x_ = prev_y_ = prev_z_ = 0;
        peak_x_ = peak_y_ = peak_z_ = 0;
        started_us_ = 0;
        last_peak_us_ = 0;
        peaks_ = 0;
    }

    bool Sample(std::int16_t ax, std::int16_t ay, std::int16_t az,
                std::uint32_t accel_lsb_per_g, std::int64_t timestamp_us) {
        if (accel_lsb_per_g == 0) return false;
        const double scale = 1000.0 / static_cast<double>(accel_lsb_per_g);
        const auto cx = static_cast<std::int32_t>(static_cast<double>(ax) * scale);
        const auto cy = static_cast<std::int32_t>(static_cast<double>(ay) * scale);
        const auto cz = static_cast<std::int32_t>(static_cast<double>(az) * scale);

        const std::int32_t px = prev_x_;
        const std::int32_t py = prev_y_;
        const std::int32_t pz = prev_z_;
        prev_x_ = cx;
        prev_y_ = cy;
        prev_z_ = cz;
        if (!has_previous_) {
            has_previous_ = true;
            return false;
        }
        if (timestamp_us - started_us_ > kMaxShakeUs) {
            peaks_ = 0;
        }
        const std::int64_t dx = static_cast<std::int64_t>(cx) - px;
        const std::int64_t dy = static_cast<std::int64_t>(cy) - py;
        const std::int64_t dz = static_cast<std::int64_t>(cz) - pz;
        const std::int64_t threshold = static_cast<std::int64_t>(kMinDeltaMg) * kMinDeltaMg;
        const std::int64_t mag_sq = dx * dx + dy * dy + dz * dz;
        if (mag_sq < threshold) return false;

        if (peaks_ == 0) {
            started_us_ = timestamp_us;
        } else {
            const std::int64_t dot = dx * peak_x_ + dy * peak_y_ + dz * peak_z_;
            if (timestamp_us - last_peak_us_ < kMinTurnUs || dot > -threshold / 2) {
                return false;
            }
        }
        last_peak_us_ = timestamp_us;
        peak_x_ = static_cast<std::int32_t>(dx);
        peak_y_ = static_cast<std::int32_t>(dy);
        peak_z_ = static_cast<std::int32_t>(dz);
        ++peaks_;
        if (peaks_ < 3) return false;
        peaks_ = 0;
        return true;
    }

private:
    bool has_previous_ = false;
    std::int32_t prev_x_ = 0;
    std::int32_t prev_y_ = 0;
    std::int32_t prev_z_ = 0;
    std::int32_t peak_x_ = 0;
    std::int32_t peak_y_ = 0;
    std::int32_t peak_z_ = 0;
    std::int64_t started_us_ = 0;
    std::int64_t last_peak_us_ = 0;
    int peaks_ = 0;
};

} // namespace voicestick
