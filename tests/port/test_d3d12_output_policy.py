# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host checks for Series S output allocation limits."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class OutputPolicyTests(unittest.TestCase):
    def test_size_policy(self):
        source = r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_output_policy.h"
#include "video_core/renderer_d3d12/d3d12_int64_policy.h"
int main() {
    using namespace D3D12;
    assert(Int64AtomicSwitchEnabled(nullptr));
    assert(Int64AtomicSwitchEnabled(""));
    assert(Int64AtomicSwitchEnabled("1"));
    assert(!Int64AtomicSwitchEnabled("0"));
    for (unsigned mask = 0; mask < 32; ++mask) {
        assert(UseNativeInt64Atomics(mask & 1, mask & 2, mask & 4, mask & 8, mask & 16)
               == (mask == 31));
    }
    assert(IsSeriesS("Xbox Series S"));
    assert(!IsSeriesS("Xbox Series X"));
    assert(!IsSeriesS(""));
    const auto check = [](OutputExtent in, bool series_s, const char* env, OutputExtent want) {
        const auto got = ChooseOutputExtent(in, series_s, env);
        assert(got.width == want.width && got.height == want.height);
    };
    check({3840,2160}, true, "", {1920,1080});
    check({3840,2160}, true, "0", {3840,2160});
    check({3840,2160}, false, "", {3840,2160});
    check({1920,1080}, true, "", {1920,1080});
    check({1280,720}, true, "", {1280,720});
    check({2560,1080}, true, "1", {1920,810});
    check({1080,1920}, true, "", {607,1080});
    check({0,0}, true, "", {1,1});
    check({1,2160}, true, "", {1,1080});
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-output-policy-") as directory:
            path = Path(directory)
            (path / "test.cpp").write_text(source)
            subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-I",
                    str(ROOT / "src"),
                    str(path / "test.cpp"),
                    "-o",
                    str(path / "test"),
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run([str(path / "test")], check=True)
