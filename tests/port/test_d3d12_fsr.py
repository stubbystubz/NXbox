# SPDX-License-Identifier: GPL-3.0-or-later
"""Host regressions for FSR selection, settings, pass coordinates and shader compilation."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PRESENT = ROOT / "src/video_core/renderer_d3d12/d3d12_present"


class FsrTests(unittest.TestCase):
    def test_selection_and_persistence(self):
        driver = r"""
#include <cassert>
#include <fstream>
#include <iterator>
#include "eden_uwp/renderer_settings.h"
#include "eden_uwp/ui/settings_model.h"
#include "video_core/renderer_d3d12/d3d12_present/present_math.h"
int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace D3D12::Present;
    using namespace EdenXbox::RendererSettings;
    const std::filesystem::path local = argv[1];
    assert(EdenXbox::Ui::SettingsFirstVisibleRow(0) == 0);
    assert(EdenXbox::Ui::SettingsFirstVisibleRow(6) == 0);
    assert(EdenXbox::Ui::SettingsFirstVisibleRow(7) == 1);
    assert(ReadFsr(local));
    { std::ofstream file(local / "eden_settings.txt"); file << "# keep\nresolution=1\n"; }
    for (bool fsr : {false, true, false}) {
        assert(SaveFsr(local, fsr));
        assert(ReadFsr(local) == fsr);
        std::ifstream file(local / "eden_settings.txt");
        const std::string contents{std::istreambuf_iterator<char>(file), {}};
        assert(contents == std::string("# keep\nresolution=1\nupscaler=") +
                           (fsr ? "fsr1\n" : "bilinear\n"));
    }
    assert(!SaveFsr(local / "missing", true));
    for (const Extent source : {Extent{640, 360}, Extent{960, 540}}) {
        assert(NeedsUpscale(source, {1280, 720}, {}, 0));
        assert(NeedsUpscale(source, {720, 1280}, {}, 4));
    }
    assert(!NeedsUpscale({1280, 720}, {1280, 720}, {}, 0));
    assert(!NeedsUpscale({2560, 1440}, {1280, 720}, {}, 0));
    assert(!NeedsUpscale({1280, 720}, {720, 1280}, {}, 4));
    assert(NeedsUpscale({1280, 720}, {1280, 720}, {.25f, .25f, .75f, .75f}, 0));
    const auto mapping = EasuMapping({960, 540}, {1280, 720}, {});
    assert(mapping[0] == .75f && mapping[1] == .75f);
    assert(mapping[2] == -.125f && mapping[3] == -.125f);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "fsr.cpp"
            binary = Path(directory) / "fsr"
            cpp.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary), directory], check=True)

    def test_active_compositor_and_frame_lifetimes(self):
        renderer = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        self.assertIn('BisectEnabled("NXBOX_D3D12_FSR")', renderer)
        self.assertIn("upscaler->DrawLayer", renderer)
        self.assertLess(
            renderer.index("rasterizer.FramebufferTexture(layer)"),
            renderer.index("upscaler->BeginFrame"),
        )
        self.assertIn("index < BufferCount", renderer)  # BGRA screenshots retain their own PSO.
        present = (PRESENT / "present.cpp").read_text()
        begin = present.split("void Presenter::BeginFrame", 1)[1].split(
            "void Presenter::DrawLayer", 1
        )[0]
        self.assertNotIn("context.Finish", begin)
        self.assertIn("WaitSubmitted(previous_tick", begin)
        self.assertIn("NeedsUpscale(extent, output, crop, layer.transform)", present)
        self.assertIn("scratch[frame_index][index]", present)

    def test_ui_and_boot_wiring(self):
        ui = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        self.assertIn("RendererSettings::ReadUpscaler(local_state_)", ui)
        self.assertIn("RendererSettings::SaveUpscaler(local_state_, next)", ui)
        boot = (ROOT / "src/eden_uwp/mesa_window.cpp").read_text()
        self.assertLess(
            boot.index('_putenv_s("NXBOX_D3D12_FSR"'),
            boot.index("while (std::getline(env_in, line))"),
        )
        for language in ("en", "pt_br"):
            strings = (ROOT / f"src/eden_uwp/ui/lang/{language}.inc").read_text()
            for key in ("UpscalerTitle", "UpscalerBilinear", "UpscalerFsr", "UpscalerHint"):
                self.assertIn("Text::" + key, strings)

    def test_boot_setting_and_environment_override(self):
        source = (ROOT / "src/eden_uwp/mesa_window.cpp").read_text()
        loader = source.split("void LoadRuntimeEnvironment() {", 1)[1].split(
            "HMODULE loader_library", 1
        )[0]
        loader = re.sub(
            r"const auto env_file =.*?;",
            'const auto env_file = test_local / "nxbox_env.txt";',
            loader,
            count=1,
            flags=re.DOTALL,
        )
        driver = r"""
#include <cassert>
#include <cstdlib>
#include <cstring>
#include "eden_uwp/renderer_settings.h"
std::filesystem::path test_local;
void Diagnostic(const std::string&) {}
int _putenv_s(const char* key, const char* value) {
    return *value ? setenv(key, value, 1) : unsetenv(key);
}
// LOADER
int main(int argc, char** argv) {
    assert(argc == 2);
    test_local = argv[1];
    const auto check = [](const char* expected) {
        LoadRuntimeEnvironment();
        assert(std::getenv("NXBOX_D3D12_FSR"));
        assert(std::strcmp(std::getenv("NXBOX_D3D12_FSR"), expected) == 0);
    };
    check("1");
    assert(EdenXbox::RendererSettings::SaveFsr(test_local, false));
    check("0");
    const auto overrides = test_local / "nxbox_env.txt";
    { std::ofstream file(overrides); file << "NXBOX_D3D12_FSR=1\r\n"; }
    check("1");
    std::filesystem::remove(overrides);
    check("0");
    assert(EdenXbox::RendererSettings::SaveFsr(test_local, true));
    check("1");
    { std::ofstream file(overrides); file << "NXBOX_D3D12_FSR=0\n"; }
    check("0");
}
"""
        driver = driver.replace(
            "// LOADER", "using namespace EdenXbox;\nvoid LoadRuntimeEnvironment() {" + loader
        )
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "boot.cpp"
            binary = Path(directory) / "boot"
            cpp.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary), directory], check=True)

    def test_actual_draw_layer_passes(self):
        source = (PRESENT / "present.cpp").read_text()
        method = source[
            source.index("void Presenter::DrawLayer(") : source.index("HRESULT Presenter::Present(")
        ]
        driver = (
            (ROOT / "tests/port/d3d12_fsr_test.cpp").read_text().replace("// DRAW_LAYER", method)
        )
        begin = source[
            source.index("void Presenter::BeginFrame(") : source.index("void Presenter::DrawLayer(")
        ]
        driver = driver.replace("// BEGIN_FRAME", begin)
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "passes.cpp"
            binary = Path(directory) / "passes"
            cpp.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True)

    def test_hlsl_dxc(self):
        dxc = os.environ.get("DXC") or shutil.which("dxc")
        if not dxc:
            self.skipTest("DXC unavailable; Xbox shader compilation needs CI")
        ffx = ROOT / "externals/FidelityFX-FSR/ffx-fsr"
        source = (ffx / "ffx_a.h").read_text()
        for width in (2, 3, 4):
            source = source.replace(
                f"(AU{width}_AF{width}(x)!=AU{width}_(0))?AU{width}_(0):AU{width}_AF{width}(y)",
                f"select(AU{width}_AF{width}(x)!=AU{width}_(0),AU{width}_(0),AU{width}_AF{width}(y))",
            )
        source = (
            "#define A_GPU 1\n#define A_HLSL 1\n"
            + source
            + "\n#define FSR_EASU_F 1\n#define FSR_RCAS_F 1\n#define FSR_RCAS_PASSTHROUGH_ALPHA 1\n"
            + (ffx / "ffx_fsr1.h").read_text()
            + "\n"
            + (PRESENT / "present.hlsl").read_text()
        )
        with tempfile.TemporaryDirectory() as directory:
            shader = Path(directory) / "present.hlsl"
            shader.write_text(source)
            for entry in (
                "VS",
                "Bilinear",
                "Bicubic",
                "Gaussian",
                "ScaleForce",
                "Area",
                "Fxaa",
                "Easu",
                "Rcas",
            ):
                result = subprocess.run(
                    [
                        dxc,
                        "-HV",
                        "2021",
                        "-Ges",
                        "-O3",
                        "-T",
                        "vs_6_0" if entry == "VS" else "ps_6_0",
                        "-E",
                        entry,
                        "-Fo",
                        str(Path(directory) / "shader.dxil"),
                        str(shader),
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 0, f"{entry}: {result.stderr}")
