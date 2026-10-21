# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production display lookup with conflicting live image aliases."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run
from test_texture_cache_fastpath import function

ROOT = Path(__file__).resolve().parents[2]


class PresentImageSelectionTests(unittest.TestCase):
    def test_display_geometry_format_recency_and_view_creation(self):
        source = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        lookup = function(source, "TextureCache<P>::TryFindFramebufferImageView(")
        lookup += "\n" + function(source, "TextureCache<P>::PrepareImageForPresentation(")
        harness = (ROOT / "tests/port/fixtures/present_image_selection.cpp").read_text()
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "fmt"], text=True))
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                harness.replace("// PRODUCTION_METHOD", lookup),
                Path(directory),
                extra_flags=[*flags, "-fsanitize=address,undefined", "-Wall", "-Wextra"],
            )

    def test_presentation_cast_uses_native_family_and_resource_capabilities(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        start = source.index("bool TextureCacheRuntime::CanPresentImage(")
        end = source.index("\n}", start) + 2
        predicate = source[start:end]
        harness = (ROOT / "tests/port/fixtures/present_format_cast.cpp").read_text()
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                harness.replace("// PRODUCTION_METHOD", predicate),
                Path(directory),
                extra_flags=["-fsanitize=address,undefined", "-Wall", "-Wextra"],
            )

    def test_presenter_fallback_downloads_gpu_owned_alias_before_guest_upload_unless_opted_out(
        self,
    ):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        signature = "RasterizerD3D12::DisplayTexture RasterizerD3D12::FramebufferTexture("
        body = source.split(signature, 1)[1].split("\n}\n", 1)[0]
        presenter = signature + body + "\n}\n"
        harness = (ROOT / "tests/port/fixtures/present_framebuffer.cpp").read_text()
        for value, fast, gpu_lookup in (
            (v, f, g)
            for v in (None, "0", "1")
            for f, g in ((None, None), ("0", "0"), ("1", "1"), ("10", "10"), ("true", "true"))
        ):
            with (
                self.subTest(fallback_download=value, fast=fast, gpu_lookup=gpu_lookup),
                tempfile.TemporaryDirectory() as directory,
            ):
                environment = os.environ.copy()
                environment.pop("NXBOX_PRESENT_FALLBACK_DOWNLOAD", None)
                environment.pop("NXBOX_D3D12_PRESENT_FAST", None)
                environment.pop("NXBOX_D3D12_PRESENT_GPU_LOOKUP", None)
                if gpu_lookup is not None:
                    environment["NXBOX_D3D12_PRESENT_GPU_LOOKUP"] = gpu_lookup
                if fast is not None:
                    environment["NXBOX_D3D12_PRESENT_FAST"] = fast
                if value is not None:
                    environment["NXBOX_PRESENT_FALLBACK_DOWNLOAD"] = value
                compile_run(
                    harness.replace("// PRODUCTION_METHOD", presenter),
                    Path(directory),
                    environment=environment,
                    extra_flags=[
                        "-fsanitize=address,undefined",
                        "-Wall",
                        "-Wextra",
                        f"-DEXPECT_FALLBACK_DOWNLOAD={int(value != '0')}",
                        f"-DEXPECT_GPU_LOOKUP={int(gpu_lookup != '0')}",
                        f"-DEXPECT_FAST={int(fast == '1')}",
                    ],
                )


if __name__ == "__main__":
    unittest.main()
