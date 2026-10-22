# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bound the one-frame lighting metadata capture, including draws rejected after binding."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class LightingTraceTests(unittest.TestCase):
    def test_production_budget(self):
        driver = r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_lighting_trace.h"
int main(int argc, char** argv) {
    if (argc == 2) {
        const auto expected = std::strtoull(argv[1], nullptr, 10);
        assert(D3D12::LightingTraceFrame() == expected);
        setenv("NXBOX_D3D12_LIGHTING_TRACE_FRAME", "777", 1);
        assert(D3D12::LightingTraceFrame() == expected); // Latched, including disabled.
        return 0;
    }
    D3D12::LightingTraceBudget trace;
    assert(!trace.Binding());
    assert(!trace.BindingLimitReached());
    // Unset/zero and all non-selected frames do not consume the budget.
    for (unsigned i = 0; i != 10000; ++i) {
        assert(!trace.Begin(0, 0));
        assert(!trace.Begin(3000, 2999));
        assert(!trace.Begin(3000, 3001));
    }
    // Counting attempts rather than successful draws bounds repeated rejected bindings.
    for (unsigned i = 1; i <= trace.MaxAttempts; ++i)
        assert(trace.Begin(3000, 3000) == i);
    assert(!trace.Begin(3000, 3000));
    for (unsigned i = 0; i != trace.MaxBindings; ++i)
        assert(trace.Binding());
    assert(trace.BindingLimitReached());
    for (unsigned i = 0; i != 10000; ++i) {
        assert(!trace.Binding());
        assert(!trace.Begin(3000, 3000));
    }
    D3D12::LightingTraceBudget other_renderer;
    assert(other_renderer.Begin(3000, 3000) == 1);
    assert(other_renderer.Binding());
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-lighting-trace-") as directory:
            cpp = Path(directory) / "trace.cpp"
            binary = Path(directory) / "trace"
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
            subprocess.run([str(binary)], check=True, timeout=30)
            for value, expected in (
                (None, 0),
                ("", 0),
                ("0", 0),
                ("-1", 0),
                ("3000x", 0),
                (" 3000", 0),
                ("18446744073709551616", 0),
                ("3000", 3000),
            ):
                with self.subTest(value=value):
                    environment = dict(os.environ)
                    environment.pop("NXBOX_D3D12_LIGHTING_TRACE_FRAME", None)
                    if value is not None:
                        environment["NXBOX_D3D12_LIGHTING_TRACE_FRAME"] = value
                    subprocess.run(
                        [str(binary), str(expected)],
                        env=environment,
                        check=True,
                        timeout=30,
                    )

    def test_trace_uses_resolved_srv_and_no_vertex_readbacks(self):
        source = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        self.assertIn("LightingTraceFrame() != 0", source)
        selection = source.split("const bool trace_vertex_bytes =", 1)[1].split(";", 1)[0]
        self.assertIn("!lighting_trace_enabled", selection)
        self.assertIn("trace_binding ? &resolved_srv : nullptr", source)
        self.assertIn("static_cast<u32>(resolved_srv.ViewDimension)", source)
        self.assertIn("resolved_srv.TextureCubeArray.First2DArrayFace", source)
        self.assertIn("resolved_srv.TextureCubeArray.NumCubes * 6", source)
        for kind in ("BEGIN", "CBV", "SRV", "DRAW", "TARGET"):
            self.assertIn(f"D3D12_LIGHTING_{kind}", source)
        sampler = source.split("if (trace_binding) {", 1)[1].split(
            "// Diagnostic: describe each distinct vertex-stage cube binding once.", 1
        )[0]
        self.assertIn("MakeSamplerDesc(sampler.Entry(), desc.is_depth)", sampler)
        for forbidden in ("Commands()", "Transition(", "CreateBuffer(", "Finish(", "Wait("):
            self.assertNotIn(forbidden, sampler)


if __name__ == "__main__":
    unittest.main()
