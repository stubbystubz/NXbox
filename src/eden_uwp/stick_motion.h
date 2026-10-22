// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace EdenXbox {

struct StickMotionSample {
    std::array<float, 3> accel{0.0f, 0.0f, -1.0f};
    std::array<float, 3> gyro{};
    std::uint64_t delta_timestamp{16667};
    bool minus_pressed{};
    bool suppress_right_stick{};
};

// Portable sensor/gesture state. Angles are radians; Eden consumes g and turns/second.
class StickMotion {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr float kMaxTilt = kPi / 3.0f;
    static constexpr float kReleaseSeconds = 0.15f;
    static constexpr float kDeadzone = 0.15f;

    // Frontend chords consume View without interrupting the current tilt gesture.
    void CancelTap() {
        cancelled = true;
    }

    StickMotionSample Update(bool view, float x, float y, Clock::time_point now,
                             bool blocked = false) {
        const float elapsed = initialized ? std::max(0.0f, Seconds(now - last)) : 1.0f / 60.0f;
        // MotionInput ignores intervals above 100 ms. Bound the simulated step after a stall.
        const float dt = std::clamp(elapsed, 0.000001f, 0.1f);
        initialized = true;
        last = now;
        StickMotionSample sample;
        sample.delta_timestamp = static_cast<std::uint64_t>(dt * 1000000.0f);
        sample.suppress_right_stick = view;
        if (view && !was_view) {
            pressed_at = now;
            moved = false;
            cancelled = false;
        }
        if (view) {
            moved = moved || std::abs(x) > kDeadzone || std::abs(y) > kDeadzone;
            cancelled = cancelled || blocked;
        }
        if (!view && was_view) {
            sample.minus_pressed =
                !moved && !cancelled && !blocked && Seconds(now - pressed_at) < 0.25f;
        }

        const float old_roll = roll;
        const float old_pitch = pitch;
        const bool active = view && !blocked;
        if (active) {
            // Limit sensor speed as well as angle: a full stick reversal must not exceed
            // MotionInput's gyro range and lose rotation through clipping.
            const float step = 4.0f * kPi * dt;
            roll += std::clamp(Axis(x) * kMaxTilt - roll, -step, step);
            pitch += std::clamp(Axis(y) * kMaxTilt - pitch, -step, step);
        } else {
            if (was_active) {
                released_at = now;
                release_roll = roll;
                release_pitch = pitch;
            }
            const float t = std::clamp(Seconds(now - released_at) / kReleaseSeconds, 0.0f, 1.0f);
            const float remaining = 1.0f - t * t * (3.0f - 2.0f * t);
            roll = release_roll * remaining;
            pitch = release_pitch * remaining;
        }
        // Same axes/signs as SDL's sensor conversion: pitch about right, roll about forward.
        // R = R_forward(roll) * R_right(pitch); gravity is transformed into controller space.
        sample.accel = {std::sin(roll), -std::cos(roll) * std::sin(pitch),
                        -std::cos(roll) * std::cos(pitch)};
        const float mid_pitch = (old_pitch + pitch) * 0.5f;
        const float roll_rate = (roll - old_roll) / (dt * 2.0f * kPi);
        sample.gyro = {(pitch - old_pitch) / (dt * 2.0f * kPi), roll_rate * std::cos(mid_pitch),
                       -roll_rate * std::sin(mid_pitch)};
        was_view = view;
        was_active = active;
        return sample;
    }

private:
    static float Seconds(Clock::duration duration) {
        return std::chrono::duration<float>(duration).count();
    }
    static float Axis(float value) {
        const float magnitude =
            std::clamp((std::abs(value) - kDeadzone) / (1.0f - kDeadzone), 0.0f, 1.0f);
        return std::copysign(magnitude, value);
    }

    Clock::time_point last{}, pressed_at{}, released_at{};
    float roll{}, pitch{}, release_roll{}, release_pitch{};
    bool initialized{}, was_view{}, was_active{}, moved{}, cancelled{};
};

} // namespace EdenXbox
