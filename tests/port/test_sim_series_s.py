# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable config/spin checks and D3D12 queue/timestamp integration contracts."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"
DRIVER = r"""
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include "common/nxbox_sim_series_s.h"
using namespace NxboxSimSeriesS;
int main(int argc, char** argv) {
    assert(argc == 4);
    const auto& config = Settings();
    assert(config.enabled == (std::atoi(argv[1]) != 0));
    assert(std::abs(config.gpu_factor - std::atof(argv[2])) < 1e-9);
    assert(std::abs(config.cpu_factor - std::atof(argv[3])) < 1e-9);
    assert(ParseFactor(nullptr, 3, 10) == 3);
    for (auto text : {"", "nan", "inf", "-2", "0", "0.99", "3oops", "11", "1e999"})
        assert(ParseFactor(text, 3, 10) == 3);
    assert(ParseFactor("1", 3, 10) == 1);
    assert(ParseFactor("10", 3, 10) == 10);
    assert(ParseFactor("2.5", 3, 10) == 2.5);
    using namespace std::chrono;
    assert(ExtraCpuTime(milliseconds{20}, 1.0) == Clock::duration::zero());
    assert(std::abs(duration<double, std::milli>(ExtraCpuTime(milliseconds{20}, 1.05)).count()
                    - 1.0) < 0.001);
    // The real spin must add the requested fraction after a real timeslice.
    const CpuTimeslice slice;
    const auto start = Clock::now();
    while (Clock::now() - start < milliseconds{10}) {}
    const auto end = Clock::now();
    slice.Finish();
    if (config.enabled && config.cpu_factor > 1.0)
        assert(Clock::now() - end >= ExtraCpuTime(end - start, config.cpu_factor));
    // A process restart is required to change the cached configuration.
    setenv("NXBOX_SIM_SERIES_S", config.enabled ? "0" : "1", 1);
    assert(&Settings() == &config);
    assert(Settings().enabled == (std::atoi(argv[1]) != 0));
}
"""


class SimSeriesSTests(unittest.TestCase):
    def test_native_configuration_and_cpu_stretch(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-sim-series-s-") as temp:
            source = Path(temp) / "sim.cpp"
            binary = Path(temp) / "sim"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
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
            base = {k: v for k, v in os.environ.items() if not k.startswith("NXBOX_SIM_")}
            cases = [
                ({}, (0, 3, 1.05)),
                ({"NXBOX_SIM_GPU_FACTOR": "5"}, (0, 3, 1.05)),
                ({"NXBOX_SIM_SERIES_S": "true"}, (0, 3, 1.05)),
                ({"NXBOX_SIM_SERIES_S": "0"}, (0, 3, 1.05)),
                ({"NXBOX_SIM_SERIES_S": "1"}, (1, 3, 1.05)),
                (
                    {
                        "NXBOX_SIM_SERIES_S": "1",
                        "NXBOX_SIM_GPU_FACTOR": "2.5",
                        "NXBOX_SIM_CPU_FACTOR": "1.25",
                    },
                    (1, 2.5, 1.25),
                ),
                (
                    {
                        "NXBOX_SIM_SERIES_S": "1",
                        "NXBOX_SIM_GPU_FACTOR": "nan",
                        "NXBOX_SIM_CPU_FACTOR": "200",
                    },
                    (1, 3, 1.05),
                ),
                (
                    {
                        "NXBOX_SIM_SERIES_S": "1",
                        "NXBOX_SIM_GPU_FACTOR": "1",
                        "NXBOX_SIM_CPU_FACTOR": "1",
                    },
                    (1, 1, 1),
                ),
            ]
            for overrides, expected in cases:
                with self.subTest(overrides=overrides):
                    subprocess.run(
                        [str(binary), *map(str, expected)],
                        env=base | overrides,
                        check=True,
                        timeout=10,
                    )

    def test_jit_hooks_bracket_both_guest_architectures(self):
        for bits in (32, 64):
            source = (ROOT / f"src/core/arm/dynarmic/arm_dynarmic_{bits}.cpp").read_text()
            begin = source.index("const NxboxSimSeriesS::CpuTimeslice sim_timeslice;")
            run = source.index("const auto result = m_jit->Run();", begin)
            finish = source.index("sim_timeslice.Finish();", run)
            self.assertLess(begin, run)
            self.assertLess(run, finish)
            self.assertLess(finish, source.index("return TranslateHaltReason(result);", run))

    def test_gpu_current_frame_timestamps_and_fenced_calibration(self):
        source = (BACKEND / "d3d12_sim_series_s.cpp").read_text()
        context = (BACKEND / "d3d12_context.cpp").read_text()
        present = (BACKEND / "renderer_d3d12.cpp").read_text()
        self.assertIn("sim_series_s->BeginSegment(facade)", context)
        self.assertIn("sim_series_s->EndSegment(facade)", context)
        self.assertLess(
            context.index("sim_series_s->EndSegment(facade)"),
            context.index("return SealRecording(reason)"),
        )
        self.assertIn("!NxboxSimSeriesS::Settings().enabled", context)
        self.assertLess(
            present.index("context.SimulateSeriesSFrame()"),
            present.index("present_ticks[index] = context.Submit"),
        )
        self.assertIn("slot.pending && slot.tick <= completed", source)
        begin = source[source.index("void SimSeriesS::PrepareFrame") :]
        self.assertLess(
            begin.index("context.WaitSubmitted(slot.tick"), begin.index("ReadSample(slot)")
        )
        frame = source[source.index("void SimSeriesS::EndFrame") :]
        self.assertNotIn("WaitSubmitted", frame)
        self.assertLess(frame.index("EndSegment(*commands)"), frame.index("commands->Dispatch"))
        self.assertLess(
            frame.index("commands->ResolveQueryData"), frame.index("commands->Dispatch")
        )
        self.assertIn("D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE", frame)
        self.assertIn("D3D12_RESOURCE_STATE_COPY_SOURCE", frame)
        self.assertIn("context.InvalidateRecordedState()", frame)
        self.assertIn("slot.tick = context.CurrentTick()", frame)
        self.assertIn("std::chrono::seconds{5}", source)
        self.assertIn("NXBOX SIM_SERIES_S gpu_factor=", source)

    def test_embedded_shader_has_observable_bounded_alu_work(self):
        shader = (ROOT / "src/video_core/host_shaders/hlsl/sim_series_s.hlsl").read_text()
        cmake = (ROOT / "src/video_core/CMakeLists.txt").read_text()
        source = (BACKEND / "d3d12_sim_series_s.cpp").read_text()
        self.assertIn("sim_series_s\n", cmake)
        self.assertIn("renderer_d3d12/d3d12_sim_series_s.cpp", cmake)
        self.assertIn(
            'compiler.Compile(HostShaders::SIM_SERIES_S_HLSL, L"main", L"cs_6_0")', source
        )
        self.assertIn("timestamps.Load2", shader)
        self.assertIn("end.y - start.y - uint(end.x < start.x)", shader)
        self.assertIn("ticks * extra_factor", shader)
        self.assertIn("maximum_ticks", shader)
        self.assertIn("4194304.0", shader)
        self.assertIn("i < iterations", shader)
        self.assertIn("output.Store", shader)
        self.assertNotIn("CreateCommandQueue", source)

    def test_shader_embedding_generates_compilable_source_header(self):
        shaders = ROOT / "src/video_core/host_shaders"
        with tempfile.TemporaryDirectory(prefix="nxbox-sim-shader-") as temp:
            header = Path(temp) / "sim_series_s_hlsl.h"
            subprocess.run(
                [
                    "cmake",
                    "-P",
                    str(shaders / "StringShaderHeader.cmake"),
                    str(shaders / "hlsl/sim_series_s.hlsl"),
                    str(header),
                    str(shaders / "source_shader.h.in"),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            source = Path(temp) / "embed.cpp"
            source.write_text(
                '#include "sim_series_s_hlsl.h"\n'
                'static_assert(HostShaders::SIM_SERIES_S_HLSL.find("i < iterations") '
                "!= std::string_view::npos);\n"
            )
            subprocess.run(
                ["clang++", "-std=c++20", "-fsyntax-only", str(source)],
                check=True,
                capture_output=True,
                text=True,
            )


if __name__ == "__main__":
    unittest.main()
