# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute shipping HLSL math on host, DXC entry points, and schedule/settings."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from p59_metal_shaders import ROOT, PRESENT, function


def cpu(text):
    text = re.sub(r"\[numthreads\([^]]*\)\]|\[unroll\]|\[loop\]", "", text)
    text = text.replace(" : SV_DispatchThreadID", "")
    text = text.replace("Texture2D<float4> image", "const Image<float4>& image")
    text = re.sub(r"\b(float[234]|int[234]|uint[234])\(", r"Make<\1>(", text)
    text = re.sub(r"\binout (AF[1234])\b", r"\1&", text)
    text = re.sub(r"\bout (AF[1234])\b", r"\1&", text)
    text = re.sub(r"\b(AF[234]|AU[234]|ASU2)\(", r"Make<\1>(", text)
    return text.replace("outAU4", "AU4&").replace("A_STATIC ", "")


def image_driver():
    temporal = (PRESENT / "temporal_upscaler.hlsl").read_text()
    temporal = cpu(temporal[temporal.index("float3 ToYCoCg") :])
    fg = (PRESENT / "frame_generation.hlsl").read_text()
    flow = cpu(
        "\n".join(function(fg, n) for n in ("Luma", "ReadLuma", "Cost", "Search", "MotionCS"))
    )
    header = (ROOT / "externals/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h").read_text()
    fsr = cpu(
        "\n".join(
            function(header, n)
            for n in ("FsrEasuCon", "FsrEasuTapF", "FsrEasuSetF", "FsrEasuF", "FsrRcasF")
        )
    )
    # Use the same portability aliases as the Metal adapter, preserving AMD's
    # approximate reciprocals and exact EASU/RCAS bodies from the vendored source.
    from p59_metal_shaders import build

    metal = build()
    macros = metal[metal.index("#define A_GPU") : metal.index("struct Fsr {")]
    macros = (
        macros.replace("as_type<uint>", "std::bit_cast<uint>")
        .replace("as_type<float2>", "std::bit_cast<float2>")
        .replace("as_type<float>", "std::bit_cast<float>")
    )
    macros = cpu(macros)
    driver = (
        '#include "p59_shader_cpu.h"\n#include <cassert>\n#include <cstdio>\n#include <random>\n'
    )
    driver += (
        """struct Temporal {
const Image<float4>& current_frame; const Image<float4>& history_frame;
const Image<float4>& motion; const Image<float4>& previous_frame; Image<float4>& accumulated;
const Image<float4>& spatial_frame;
uint2 source_size, output_size; uint history_valid; int linear_sampler=0;
"""
        + temporal
        + "\n};\n"
    )
    driver += (
        """struct Flow { Image<float2>& luma; Image<float4>& motion_out; uint2 blocks;
"""
        + flow
        + "\n};\n"
        + macros
    )
    driver += (
        """struct Fsr { const Image<float4>& source;
float4 FsrEasuRF(float2 p) { return source.Gather(p,0); }
float4 FsrEasuGF(float2 p) { return source.Gather(p,1); }
float4 FsrEasuBF(float2 p) { return source.Gather(p,2); }
float4 FsrRcasLoadF(int2 p) { return source.Load(int3{p.x,p.y,0}); }
void FsrRcasInputF(float&,float&,float&) {}
"""
        + fsr
        + "\n};\n"
    )
    driver += (
        "struct Warp { const Image<float4>& previous_frame; const Image<float4>& current_frame; "
        "const Image<float4>& motion; Image<float4>& generated; uint2 size, output_size; "
        "float interpolation; int linear_sampler=0;\n" + cpu(function(fg, "WarpCS")) + "\n};\n"
    )
    driver += (ROOT / "tests/port/p59_temporal_image.cpp").read_text()
    return driver


class UpscalingTests(unittest.TestCase):
    def test_image_error(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "image.cpp"
            binary = Path(directory) / "image"
            cpp.write_text(image_driver())
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O2",
                    "-Wno-narrowing",
                    "-Wno-implicit-int-float-conversion",
                    "-I",
                    str(ROOT / "tests/port"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")

    def test_shader_entries(self):
        dxc = os.environ.get("DXC") or shutil.which("dxc")
        if not dxc:
            self.skipTest("DXC unavailable")
        with tempfile.TemporaryDirectory() as directory:
            for shader, entries in [
                ("frame_generation.hlsl", ["LumaCS", "MotionCS", "WarpCS", "DownsampleCS"]),
                ("temporal_upscaler.hlsl", ["TemporalCS"]),
            ]:
                for entry in entries:
                    result = subprocess.run(
                        [
                            dxc,
                            "-HV",
                            "2021",
                            "-Ges",
                            "-O3",
                            "-T",
                            "cs_6_0",
                            "-E",
                            entry,
                            "-Fo",
                            str(Path(directory) / "shader.dxil"),
                            str(PRESENT / shader),
                        ],
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_modes_timestamps_and_budget(self):
        from test_d3d12_capture import compile_run

        driver = r"""
#include <cassert>
#include <limits>
#include "eden_uwp/renderer_settings.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
int main(int, char** argv) {
    using namespace D3D12::Present;
    using namespace EdenXbox::RendererSettings;
    const std::filesystem::path local(argv[1]);
    assert(ReadFrameGenerationMode(local)==0);
    assert(ReadUpscaler(local)=="fsr1"); // Preserve the existing spatial default.
    for (unsigned preference : {0u,2u,4u}) {
        assert(FrameGenerationMode(preference,nullptr)==preference);
        assert(FrameGenerationMode(preference,"bad")==preference);
        assert(FrameGenerationMode(preference,"0")==0);
        assert(FrameGenerationMode(preference,"1")==2);
        assert(FrameGenerationMode(preference,"2")==2);
        assert(FrameGenerationMode(preference,"4")==4);
        assert(SaveFrameGenerationMode(local,preference));
        assert(ReadFrameGenerationMode(local)==preference);
    }
    for (auto mode : {"bilinear","fsr1","temporal"}) {
        assert(SaveUpscaler(local,mode));
        assert(ReadUpscaler(local)==mode);
    }
    assert(!SaveUpscaler(local,"invalid"));
    for (unsigned mode : {2u,4u}) {
        const GeneratedFrameSchedule schedule{mode};
        assert(schedule.Count()==mode-1);
        double previous=1.;
        for(unsigned i=0;i<schedule.Count();++i) {
            const double timestamp=schedule.Timestamp(1.,1.+1./30,i);
            assert(timestamp>previous && timestamp<1.+1./30);
            assert(std::abs(timestamp-(1.+(i+1.)/(30*mode)))<1e-9);
            previous=timestamp;
        }
    }
    assert(GeneratedFrameSchedule{0}.Count()==0);
    assert(DisplayFrameGenerationMode(4,60)==2);
    assert(DisplayFrameGenerationMode(4,59.94)==2);
    assert(DisplayFrameGenerationMode(4,0)==2);
    assert(DisplayFrameGenerationMode(4,std::numeric_limits<double>::quiet_NaN())==2);
    assert(DisplayFrameGenerationMode(4,120)==4);
    assert(DisplayFrameGenerationMode(4,119.88)==4);
    assert(DisplayFrameGenerationMode(2,120)==2);
    assert(!FrameGenerationBudgetMissed(8,1./30,4));
    assert(FrameGenerationBudgetMissed(9,1./30,4));
    assert(!FrameGenerationBudgetMissed(9,1./30,2));
    assert(FrameGenerationBudgetMissed(17,1./30,2));
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver, Path(directory), extra_flags=["-Wall", "-Wextra", "-Werror"])
