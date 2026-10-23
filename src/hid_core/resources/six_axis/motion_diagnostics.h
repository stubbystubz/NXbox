// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace Service::HID {

// Accumulates only samples written to the guest LIFO, after HID filtering/fusion.
class MotionDiagnostics {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr float kTau = 6.28318530717958647692f;

    bool Update(const std::array<float, 3>& gyro_turns, const std::array<float, 9>& attitude,
                bool delivered, Clock::time_point now) {
        if (!initialized) {
            window_start = now;
            initialized = true;
        }
        attitude_delta_rad = 0.0f;
        if (delivered && previous_delivered) {
            // ||R_current - R_previous||_F^2 = 8 sin^2(angle/2).
            // Using differences keeps identical attitudes exactly zero, even with float drift.
            float squared_difference = 0.0f;
            for (std::size_t element = 0; element < attitude.size(); ++element) {
                const float difference = attitude[element] - previous_attitude[element];
                squared_difference += difference * difference;
            }
            attitude_delta_rad =
                2.0f * std::asin(std::min(std::sqrt(squared_difference * 0.125f), 1.0f));
        }
        previous_attitude = attitude;
        previous_delivered = delivered;
        for (std::size_t axis = 0; axis < gyro_rad_s.size(); ++axis) {
            gyro_rad_s[axis] = delivered ? gyro_turns[axis] * kTau : 0.0f;
            max_abs_gyro_rad_s[axis] =
                std::max(max_abs_gyro_rad_s[axis], std::abs(gyro_rad_s[axis]));
        }
        max_attitude_delta_rad = std::max(max_attitude_delta_rad, attitude_delta_rad);
        return now - window_start >= std::chrono::seconds(1);
    }

    void NextWindow(Clock::time_point now) {
        window_start = now;
        max_abs_gyro_rad_s = {};
        max_attitude_delta_rad = 0.0f;
    }

    std::array<float, 3> gyro_rad_s{}, max_abs_gyro_rad_s{};
    float attitude_delta_rad{}, max_attitude_delta_rad{};

private:
    Clock::time_point window_start{};
    std::array<float, 9> previous_attitude{};
    bool initialized{}, previous_delivered{};
};

} // namespace Service::HID
