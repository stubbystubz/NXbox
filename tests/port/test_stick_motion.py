# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the actual portable Xbox motion and gesture state on the host."""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cmath>
#include "eden_uwp/stick_motion.h"
#include "hid_core/resources/six_axis/motion_diagnostics.h"
using namespace EdenXbox;
using namespace std::chrono_literals;
bool near(float a, float b) { return std::abs(a - b) < .0001f; }
void rest(const StickMotionSample& sample) {
    assert(near(sample.accel[0], 0) && near(sample.accel[1], 0));
    assert(near(sample.accel[2], -1));
    for (float rate : sample.gyro) assert(near(rate, 0));
}
int main() {
    const auto t = StickMotion::Clock::time_point{};
    StickMotion motion;
    rest(motion.Update(true, true, 0, 0, 0, 0, t));
    auto sample = motion.Update(true, true, 0, 0, 1, 0, t + 100ms);
    assert(sample.suppress_sticks);
    assert(near(sample.gyro[2], -StickMotion::kAimTurnsPerSecond));
    sample = motion.Update(true, true, 0, 0, 0, 1, t + 200ms);
    assert(near(sample.gyro[0], StickMotion::kAimTurnsPerSecond));
    assert(near(sample.accel[1], -std::sin(StickMotion::kPi * .1f)));
    // Release immediately resets output; there is no return-motion tail.
    sample = motion.Update(true, false, 1, 1, 1, 1, t + 210ms);
    rest(sample);
    assert(!sample.suppress_sticks);
    sample = motion.Update(false, true, 1, 1, 1, 1, t + 220ms);
    rest(sample);
    assert(!sample.suppress_sticks);
    rest(motion.Update(true, true, .1f, -.1f, .1f, -.1f, t + 230ms));

    StickMotion tilt;
    tilt.Update(true, true, 0, 0, 0, 0, t);
    tilt.Update(true, true, 1, 0, 0, 0, t + 100ms);
    sample = tilt.Update(true, true, 1, 0, 0, 0, t + 200ms);
    assert(near(std::atan2(sample.accel[0], -sample.accel[2]), StickMotion::kMaxTilt));
    assert(near(sample.accel[2], -.5f));
    sample = tilt.Update(true, true, 1, 0, 0, 0, t + 210ms);
    for (float rate : sample.gyro) assert(near(rate, 0));
    sample = tilt.Update(true, true, -1, 1, 1, -1, t + 310ms);
    float length = 0;
    for (float gravity : sample.accel) length += gravity * gravity;
    assert(near(length, 1));
    for (float rate : sample.gyro) assert(std::isfinite(rate) && std::abs(rate) < 2);
    sample = tilt.Update(true, true, -1, 1, 1, -1, t + 310ms);
    for (float rate : sample.gyro) assert(std::isfinite(rate));
    assert(tilt.Update(true, true, 1, 1, 1, 1, t + 2s).delta_timestamp == 100000);
    rest(tilt.Update(false, false, 0, 0, 0, 0, t + 3s));
    rest(tilt.Update(true, true, 0, 0, 0, 0, t + 4s));

    StickMotion pitch;
    pitch.Update(true, true, 0, 0, 0, 0, t);
    pitch.Update(true, true, 0, 1, 0, 0, t + 100ms);
    sample = pitch.Update(true, true, 0, 1, 0, 0, t + 200ms);
    assert(near(std::atan2(-sample.accel[1], -sample.accel[2]), StickMotion::kMaxTilt));
    assert(sample.gyro[0] > 0);

    // The exact production button result is routed to SetButton(LStick).
    StickMotion tap;
    int rising_edges = 0;
    bool previous_button = false;
    const auto route = [&](const StickMotionSample& routed) {
        if (routed.guest_l3 && !previous_button) ++rising_edges;
        previous_button = routed.guest_l3;
    };
    for (int millis = 0; millis <= 400; ++millis) {
        const auto routed = tap.Update(true, millis < 249, .15f, -.15f, .15f, -.15f,
                                       t + std::chrono::milliseconds(millis));
        if (millis < 249) assert(!routed.guest_l3 && !routed.suppress_sticks);
        route(routed);
    }
    assert(rising_edges == 1 && !previous_button && tap.TapCount() == 1);

    StickMotion hold;
    for (int millis = 0; millis <= 400; ++millis) {
        const auto routed = hold.Update(true, millis < 300, 0, 0,
                                        millis >= 10 && millis < 100 ? 1.0f : 0.0f,
                                        0, t + std::chrono::milliseconds(millis));
        assert(!routed.guest_l3);
        if (millis >= 10 && millis < 100) {
            assert(routed.suppress_sticks && std::abs(routed.gyro[2]) > 0);
        }
        if (millis >= 100 && millis < 300) assert(routed.suppress_sticks);
    }
    assert(hold.TapCount() == 0);
    // Either stick/axis latches motion, even before the timer expires.
    for (int axis = 0; axis < 4; ++axis) {
        StickMotion moved;
        moved.Update(true, true, 0, 0, 0, 0, t);
        std::array<float, 4> sticks{};
        sticks[static_cast<std::size_t>(axis)] = -.16f;
        assert(moved.Update(true, true, sticks[0], sticks[1], sticks[2], sticks[3],
                            t + 1ms).suppress_sticks);
        assert(!moved.Update(true, false, 0, 0, 0, 0, t + 2ms).guest_l3);
    }
    StickMotion timed;
    assert(!timed.Update(true, true, 0, 0, 0, 0, t).guest_l3);
    assert(!timed.Update(true, true, 0, 0, 0, 0, t + 249ms).suppress_sticks);
    assert(timed.Update(true, true, 0, 0, 0, 0, t + 250ms).suppress_sticks);
    assert(!timed.Update(true, false, 0, 0, 0, 0, t + 251ms).guest_l3);
    StickMotion boundary;
    boundary.Update(true, true, 0, 0, 0, 0, t);
    assert(!boundary.Update(true, false, 0, 0, 0, 0, t + 250ms).guest_l3);

    StickMotion passthrough;
    for (int millis = 0; millis < 500; ++millis) {
        const bool button = millis < 400;
        const auto routed = passthrough.Update(false, button, 1, 1, 1, 1,
                                               t + std::chrono::milliseconds(millis));
        assert(routed.guest_l3 == button && !routed.suppress_sticks);
        rest(routed);
    }
    assert(passthrough.TapCount() == 0);
    assert(passthrough.Update(false, true, 0, 0, 0, 0, t + 1s, true).guest_l3);
    StickMotion blocked;
    blocked.Update(true, true, 0, 0, 0, 0, t);
    blocked.Update(true, true, 0, 0, 0, 0, t + 10ms, true);
    assert(!blocked.Update(true, false, 0, 0, 0, 0, t + 20ms).guest_l3);
    StickMotion activation;
    assert(activation.Update(false, true, 0, 0, 0, 0, t).guest_l3);
    assert(!activation.Update(true, true, 0, 0, 0, 0, t + 1ms).guest_l3);
    assert(!activation.Update(true, false, 0, 0, 0, 0, t + 2ms).guest_l3);
    StickMotion stopping;
    stopping.Update(true, true, 0, 0, 1, 0, t);
    assert(stopping.Update(false, true, 0, 0, 1, 0, t + 1ms).guest_l3);
    assert(!stopping.Update(false, false, 0, 0, 0, 0, t + 2ms).guest_l3);
    // A new motion hold cancels a previous tap pulse, including on its release.
    StickMotion rapid;
    rapid.Update(true, true, 0, 0, 0, 0, t);
    assert(rapid.Update(true, false, 0, 0, 0, 0, t + 1ms).guest_l3);
    assert(!rapid.Update(true, true, 0, 0, 1, 0, t + 2ms).guest_l3);
    assert(!rapid.Update(true, false, 0, 0, 0, 0, t + 3ms).guest_l3);

    // Diagnostics use post-filter gyro and actual guest attitude, with one-second peaks.
    using Service::HID::MotionDiagnostics;
    MotionDiagnostics diagnostic;
    const std::array<float, 9> identity{1,0,0, 0,1,0, 0,0,1};
    const std::array<float, 9> quarter_turn{0,-1,0, 1,0,0, 0,0,1};
    assert(!diagnostic.Update({0,0,0}, identity, true, t));
    assert(!diagnostic.Update({.5f,-.25f,0}, quarter_turn, true, t + 10ms));
    assert(near(diagnostic.gyro_rad_s[0], StickMotion::kPi));
    assert(near(diagnostic.attitude_delta_rad, StickMotion::kPi / 2));
    assert(!diagnostic.Update({0,0,0}, quarter_turn, true, t + 999ms));
    assert(diagnostic.Update({0,0,0}, quarter_turn, true, t + 1s));
    assert(near(diagnostic.max_abs_gyro_rad_s[0], StickMotion::kPi));
    assert(near(diagnostic.max_abs_gyro_rad_s[1], StickMotion::kPi / 2));
    assert(near(diagnostic.max_attitude_delta_rad, StickMotion::kPi / 2));
    diagnostic.NextWindow(t + 1s);
    assert(!diagnostic.Update({1,1,1}, identity, false, t + 1100ms));
    assert(near(diagnostic.max_abs_gyro_rad_s[0], 0));
    assert(near(diagnostic.attitude_delta_rad, 0));
    assert(diagnostic.Update({0,0,0}, identity, true, t + 2s));
    assert(near(diagnostic.max_attitude_delta_rad, 0));

    ViewGesture view;
    view.Update(true, .1f, -.1f, t);
    assert(view.Update(false, 0, 0, t + 249ms));
    assert(!view.Update(false, 0, 0, t + 250ms));
    view.Update(true, 0, 0, t + 300ms);
    assert(!view.Update(false, 0, 0, t + 550ms));
    view.Update(true, 1, 0, t + 600ms);
    assert(!view.Update(false, 0, 0, t + 610ms));
    view.Update(true, 0, 0, t + 700ms, true);
    assert(!view.Update(false, 0, 0, t + 710ms));
}
"""


def test_stick_motion_math_and_gating():
    with tempfile.TemporaryDirectory(prefix="nxbox-motion-") as temporary:
        source = Path(temporary) / "driver.cpp"
        binary = Path(temporary) / "driver"
        source.write_text(DRIVER)
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Wshadow",
                "-Wconversion",
                "-Werror",
                "-I",
                str(ROOT / "src"),
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run([str(binary)], check=True, timeout=10)
