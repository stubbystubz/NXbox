# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline regression for the fallback quad strip's diagonal and winding."""

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


def indices(source, name):
    match = re.search(rf"\b{name}\[6\]\s*=\s*\{{([^}}]+)\}}", source)
    return tuple(int(value) for value in match[1].split(","))


class DiagonalSeamTests(unittest.TestCase):
    def test_fallback_matches_indexed_quad_strip(self):
        fallback = (D3D12 / "d3d12_buffer_cache.cpp").read_text()
        compute = (ROOT / "src/video_core/host_shaders/hlsl/quad_indexed.hlsl").read_text()
        # Strip corners are ordered (bottom-left, top-left, bottom-right, top-right).
        # The old fallback used the other diagonal AND reversed both faces.
        self.assertEqual(indices(fallback, "strip_list"), indices(compute, "strip"))
        self.assertEqual(indices(fallback, "quad_list"), indices(compute, "quads"))

    def test_shared_edges_have_exactly_one_owner(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-p87-edges-") as directory:
            binary = str(Path(directory) / "edges")
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-Wconversion",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/d3d12_diagonal_seam_test.cpp"),
                    str(D3D12 / "d3d12_index_converter.cpp"),
                    "-o",
                    binary,
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
            for precise, conservative, expected in (
                ("", "", 0),
                ("0", "0", 0),
                ("1", "0", 1),
                ("0", "1", 2),
                ("1", "1", 3),
                ("01", "true", 0),
            ):
                environment = dict(
                    os.environ,
                    NXBOX_HLSL_PRECISE_POSITION=precise,
                    NXBOX_D3D12_NO_CONSERVATIVE_RASTER=conservative,
                    NXBOX_D3D12_DRAW_TEXTURE_TRIANGLE="0",
                )
                subprocess.run([binary, str(expected)], env=environment, check=True, timeout=60)
                environment["NXBOX_D3D12_DRAW_TEXTURE_TRIANGLE"] = "1"
                subprocess.run([binary, str(expected + 4)], env=environment, check=True, timeout=60)

    def test_draw_texture_triangle_has_no_internal_edge(self):
        header = (ROOT / "src/shader_recompiler/backend/hlsl/hlsl_draw_texture.h").read_text()
        shaders = re.findall(r'R"hlsl\((.*?)\)hlsl"', header, re.DOTALL)
        strip, triangle = shaders[:2]
        pattern = r"float2 corner = float2\((.*)\);"
        # Execute the actual vertex-ID expressions, then compare the rest of the
        # shader verbatim: destination and UV interpolation must stay identical.
        expression = re.search(pattern, triangle)[1]
        expression = re.sub(r"(\d+)u\b", r"\1", expression)
        corners = [eval(expression, {"__builtins__": {}}, {"id": vertex}) for vertex in range(3)]
        self.assertEqual(corners, [(0, 0), (2, 0), (0, 2)])
        self.assertEqual(re.sub(pattern, "CORNER", strip), re.sub(pattern, "CORNER", triangle))
        for x, y in ((0.5, 0.5), (0.001, 0.999), (0.999, 0.999)):
            weights = (1 - (x + y) / 2, x / 2, y / 2)
            self.assertTrue(all(weight >= 0 for weight in weights))
            self.assertAlmostEqual(
                sum(weight * corner[0] for weight, corner in zip(weights, corners)), x
            )
            self.assertAlmostEqual(
                sum(weight * corner[1] for weight, corner in zip(weights, corners)), y
            )
        rasterizer = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        self.assertIn("Shader::Backend::HLSL::DrawTextureFillsViewport(", rasterizer)
        self.assertIn("DrawInstanced(fullscreen_triangle ? 3u : 4u", rasterizer)

    def test_diagnostics_isolate_serialized_prewarm(self):
        cache = (D3D12 / "d3d12_pipeline_cache.cpp").read_text()
        self.assertIn("(Nxbox::SeamDiagnostics::Mode() << 10)", cache)
        self.assertIn("Pso::HashBytes(&seam_mode, sizeof(seam_mode), specialization)", cache)
        self.assertIn("Nxbox::SeamDiagnostics::NoConservativeRaster", cache)

    def test_real_position_emission_modes(self):
        binary = os.environ.get("NXBOX_HLSL_IR_TEST_BINARY")
        if not binary:
            self.skipTest("Set NXBOX_HLSL_IR_TEST_BINARY to the built hlsl-ir-tests")
        for precise, conservative in (("0", "0"), ("1", "0"), ("0", "1"), ("1", "1")):
            with self.subTest(precise=precise, conservative=conservative):
                environment = dict(
                    os.environ,
                    NXBOX_HLSL_PRECISE_POSITION=precise,
                    NXBOX_D3D12_NO_CONSERVATIVE_RASTER=conservative,
                )
                subprocess.run([binary, "seam-position"], env=environment, check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
