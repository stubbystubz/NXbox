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
