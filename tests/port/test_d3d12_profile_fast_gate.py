# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production inline profile gates and enabled accounting on the host."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_subresource_feedback import function

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class ProfileFastGateTests(unittest.TestCase):
    def test_disabled_has_no_owner_checks_and_enabled_matches(self):
        header = (NATIVE / "d3d12_context.h").read_text()
        source = (NATIVE / "d3d12_context.cpp").read_text()
        scope = (
            "class Scope {"
            + header.split("class Scope {", 1)[1].split("    bool Enabled() const", 1)[0]
        )
        methods = "\n".join(
            function(source, name)
            for name in (
                "void FrameProfile::Scope::Begin(",
                "void FrameProfile::Scope::End(",
                "void FrameProfile::CountEnabled(",
            )
        )
        driver = r"""
#include <cassert>
#include <iostream>
#include "video_core/renderer_d3d12/d3d12_context.h"
using namespace D3D12;
using u64 = std::uint64_t;
u64 clock_reads{}, clock_value{};
u64 ProfileWallTime() { ++clock_reads; return clock_value; }
struct FrameProfile {
    bool enabled{}, owner{true};
    unsigned owner_checks{};
    FrameTiming timing;
    u64 fence_wall{};
    std::array<u64, static_cast<size_t>(FrameCount::Count)> counts{};
    __SCOPE__
    __ENABLED__
    __COUNT__
    void CountEnabled(FrameCount, u64);
    bool OnOwner() { ++owner_checks; return enabled && owner; }
    void Switch(FramePhase phase) { timing.Switch(phase, ProfileWallTime()); }
};
__METHODS__
int main() {
    ::FrameProfile profile;
    for (unsigned i = 0; i < 10000; ++i) {
        ::FrameProfile::Scope scope{profile, FramePhase::DrawSetup};
        profile.Count(FrameCount::UniformUploadBytes, 256);
    }
    assert(clock_reads == 0 && profile.owner_checks == 0);
    for (auto count : profile.counts) assert(count == 0);
    profile.enabled = true;
    {
        ::FrameProfile::Scope outer{profile, FramePhase::DrawSetup};
        clock_value = 10;
        {
            ::FrameProfile::Scope inner{profile, FramePhase::FenceWait};
            clock_value = 30;
            profile.Count(FrameCount::UniformUploadBytes, 256);
            profile.Count(FrameCount::UniformUploadBytes, 0);
        }
        clock_value = 40;
    }
    assert(profile.timing.active == FramePhase::BetweenDraws);
    assert(profile.timing.cpu[static_cast<size_t>(FramePhase::DrawSetup)] == 20);
    assert(profile.timing.cpu[static_cast<size_t>(FramePhase::FenceWait)] == 20);
    assert(profile.fence_wall == 20);
    assert(profile.counts[static_cast<size_t>(FrameCount::UniformUploadBytes)] == 256);
    assert(profile.counts[static_cast<size_t>(FrameCount::UniformUploadCount)] == 1);
    profile.owner = false;
    auto reads = clock_reads;
    {
        ::FrameProfile::Scope wrong_thread{profile, FramePhase::DrawSetup};
        profile.Count(FrameCount::UniformUploadBytes, 999);
    }
    assert(clock_reads == reads);
    assert(profile.counts[static_cast<size_t>(FrameCount::UniformUploadBytes)] == 256);
    std::cout << "10000 disabled scopes/counters: zero owner checks/clocks; enabled nested timing, waits, counters and thread ownership unchanged\n";
}
"""
        driver = driver.replace("__SCOPE__", scope)
        driver = driver.replace("__ENABLED__", function(header, "bool Enabled() const"))
        driver = driver.replace("__COUNT__", function(header, "void Count(FrameCount"))
        driver = driver.replace("__METHODS__", methods)
        with tempfile.TemporaryDirectory(prefix="nxbox-profile-gate-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(driver)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
