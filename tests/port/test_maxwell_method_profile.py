# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check exclusive method clocks, disabled code generation, and log ranking."""

import importlib.util
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_gpu_frontend_hot_paths as frontend

ROOT = frontend.ROOT
SCRIPT = ROOT / "tools/nxbox/method_rank.py"
SPEC = importlib.util.spec_from_file_location("method_rank", SCRIPT)
RANK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RANK)


class MethodRankTests(unittest.TestCase):
    def test_median_windows_and_synthetic_cli(self):
        log = """unrelated line
[Debug] NXBOX METHOD_PROFILE cb_data=2.0,100 other=1,2 other_0x123=1,2 frames=100
NXBOX METHOD_PROFILE cb_data=8.0,400 other=0,0 frames=200
NXBOX METHOD_PROFILE cb_data=3.0,200 other=0,0 frames=100
NXBOX METHOD_PROFILE cb_data=nan,4
NXBOX METHOD_PROFILE cb_data=1,2 cb_data=3,4
NXBOX METHOD_PROFILE cb_data=1,2 frames=0
NXBOX METHOD_PROFILE cb_data=1,
"""
        result = RANK.summarize(log.splitlines())
        self.assertEqual((result["windows"], result["malformed"]), (3, 4))
        self.assertEqual(result["ranking"][0]["class"], "cb_data")
        self.assertEqual(result["ranking"][0]["ms/frame"], 3)
        self.assertEqual(result["ranking"][0]["count/frame"], 200)
        detail = next(r for r in result["ranking"] if r["class"] == "other_0x123")
        self.assertEqual(detail["ms/frame"], 0)
        self.assertEqual(detail["windows_present"], 1)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "eden.txt"
            path.write_text(log)
            output = subprocess.check_output([sys.executable, SCRIPT, path, "--json"])
            self.assertEqual(json.loads(output), result)
            output = subprocess.check_output([sys.executable, SCRIPT, path], text=True)
            self.assertIn("cb_data", output)
            path.write_text("NXBOX METHOD_PROFILE cb_data=broken\n")
            self.assertEqual(
                subprocess.run(
                    [sys.executable, SCRIPT, path], check=False, capture_output=True
                ).returncode,
                1,
            )


class MethodProfilePortTests(unittest.TestCase):
    compile_and_run = frontend.GpuFrontendHotPathTests.compile_and_run

    def test_exclusive_phase_clock_nested_runs_exceptions_and_windows(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        production = frontend.method(source, "FrameProfile::ReadTicks")
        self.compile_and_run(
            r"""
#include <cassert>
#include <stdexcept>
#include "common/maxwell_method_profile.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
using namespace Common::MaxwellMethodProfile;
using namespace D3D12;
using u64 = std::uint64_t;
u64 now{};
u64 ProfileWallTime() { return now; }
struct FrameProfile : Collector {
    FrameTiming timing;
    u64 method_clock_base{};
    Common::GpuProfile::Tick ReadTicks() override;
    void Switch(FramePhase phase, u64 time) { now = time; timing.Switch(phase, now); }
};
// PRODUCTION
int main() {
    FrameProfile p;
    collector = &p;
    p.Switch(FramePhase::Maxwell3DMethods, 0);
    Dispatch([] { return ConstantBuffer; }, 16, [&](auto profile) {
        static_assert(decltype(profile)::value || !decltype(profile)::value);
        now = 10;
        Shadow<decltype(profile)::value>(16, [&] { now = 30; });
        // Expensive renderer work must contribute zero method time.
        p.Switch(FramePhase::DrawRecording, 40);
        p.Switch(FramePhase::Maxwell3DMethods, 1040);
        now = 1050;
    });
    assert(p.samples[ConstantBuffer].ticks == 30);
    assert(p.samples[ConstantBuffer].count == 16);
    assert(p.samples[ShadowDirty].ticks == 20);
    assert(p.samples[ShadowDirty].count == 16);
    Dispatch([] { return Macro; }, 1, [&](auto) {
        p.Switch(FramePhase::MacroExecution, 1060);
        p.Switch(FramePhase::Maxwell3DMethods, 2000);
        Dispatch([] { return DrawEnd; }, 1, [&](auto) { now = 2020; });
        p.Switch(FramePhase::MacroExecution, 2030);
        p.Switch(FramePhase::Maxwell3DMethods, 3000);
        now = 3010;
    });
    assert(p.samples[Macro].ticks == 30);
    assert(p.samples[DrawEnd].ticks == 20);
    assert(!p.active);
    try {
        Dispatch([] { return QuerySync; }, 1, [&](auto) {
            now = 3020;
            throw std::runtime_error("test");
        });
    } catch (const std::runtime_error&) {}
    assert(!p.active && p.samples[QuerySync].ticks == 10);
    // FrameProfile can close/reset a window from inside a method's submit.
    Dispatch([] { return DrawBegin; }, 2, [&](auto) {
        now = 3030;
        p.Switch(p.timing.active, now);
        p.method_clock_base += p.timing.cpu[static_cast<size_t>(FramePhase::Maxwell3DMethods)];
        p.samples.fill({});
        p.timing.Reset(now);
        now = 3040;
    });
    assert(p.samples[DrawBegin].ticks == 20);
    assert(p.samples[DrawBegin].count == 2);
    collector = nullptr;
    unsigned keys = 0, calls = 0;
    Dispatch([&] { ++keys; return Clear; }, 99, [&](auto profile) {
        if constexpr (decltype(profile)::value) assert(false);
        Shadow<decltype(profile)::value>(99, [&] { ++calls; });
    });
    assert(keys == 0 && calls == 1);
}
""".replace("// PRODUCTION", production)
        )

    def test_production_line_normalization_and_top16(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        start = source.index(
            "        if (method_enabled) {", source.index("void FrameProfile::EndFrame")
        )
        # The first block resets the owner; select the reporting block by its marker.
        marker = source.index('std::string method_line = "NXBOX METHOD_PROFILE";', start)
        start = source.rfind("        if (method_enabled) {", start, marker)
        end = source.index("        Diagnostic(line);", marker)
        report = source[start:end]
        try:
            flags = shlex.split(
                subprocess.check_output(
                    ["pkg-config", "--cflags", "fmt"], text=True, stderr=subprocess.DEVNULL
                )
            )
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("host fmt headers unavailable")
        driver = r"""
#include <algorithm>
#include <iostream>
#include <string_view>
#include <fmt/format.h>
#include "common/maxwell_method_profile.h"
void Diagnostic(std::string_view line) { std::cout << line << '\n'; }
int main() {
    using namespace Common::MaxwellMethodProfile;
    std::array<Sample, ClassCount + RegisterCount> samples{};
    const bool method_enabled = true;
    const unsigned frames = 100;
    samples[ConstantBuffer] = {2000000, 1200};
    for (unsigned i = 0; i < 17; ++i)
        samples[ClassCount + 0x100 + i] = {10000 * (i + 1), i + 1};
    // REPORT
}
""".replace("// REPORT", report)
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "report.cpp"
            binary = Path(directory) / "report"
            cpp.write_text(driver)
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-DFMT_HEADER_ONLY",
                    *flags,
                    f"-I{ROOT / 'src'}",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            output = subprocess.check_output([str(binary)], text=True)
        result = RANK.summarize(output.splitlines())
        self.assertEqual(result["windows"], 1)
        self.assertEqual(result["malformed"], 0)
        rows = {row["class"]: row for row in result["ranking"]}
        self.assertEqual(rows["cb_data"]["ms/frame"], 2)
        self.assertEqual(rows["cb_data"]["count/frame"], 12)
        self.assertAlmostEqual(rows["other"]["ms/frame"], 1.53)
        self.assertEqual(sum(key.startswith("other_0x") for key in rows), 16)
        self.assertNotIn("other_0x100", rows)
        self.assertIn("other_0x110", rows)

    def test_exact_latched_env_gate(self):
        self.compile_and_run(
            r"""
#include <cassert>
#include <cstring>
#include "common/maxwell_method_profile.h"
int main() {
    const char* value = std::getenv("NXBOX_D3D12_METHOD_PROFILE");
    const bool expected = value && std::strcmp(value, "1") == 0;
    assert(Common::MaxwellMethodProfile::Enabled() == expected);
    setenv("NXBOX_D3D12_METHOD_PROFILE", expected ? "0" : "1", 1);
    assert(Common::MaxwellMethodProfile::Enabled() == expected);
}
""",
            [dict(os.environ, NXBOX_D3D12_METHOD_PROFILE=v) for v in ("", "0", "1", "10", "true")],
        )

    def test_disabled_instantiation_compiles_out_all_hooks(self):
        # The false instantiation selected by the one TLS-pointer branch must
        # retain only the payload loop, even at -O0 (no optimizer dependency).
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "disabled.cpp"
            path.write_text(
                r"""
#include "common/maxwell_method_profile.h"
extern "C" void disabled_run(unsigned* output, unsigned count) {
    Common::MaxwellMethodProfile::Shadow<false>(count, [&] {
        for (unsigned i = 0; i < count; ++i) output[i] += 1;
    });
}
"""
            )
            assembly = Path(directory) / "disabled.s"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O0",
                    "-S",
                    f"-I{ROOT / 'src'}",
                    str(path),
                    "-o",
                    str(assembly),
                ],
                check=True,
            )
            text = assembly.read_text()
            for forbidden in (
                "ReadTicks",
                "getenv",
                "QueryPerformance",
                "steady_clock",
                "Collector5Scope",
                "collector",
                "samples",
            ):
                self.assertNotIn(forbidden, text)
