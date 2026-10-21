# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise host-side FPS policy and sampling, and pin display-only D3D12 wiring."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include "common/fps_overlay.h"
#include "eden_uwp/fps_settings.h"
#include "video_core/renderer_d3d12/d3d12_present/fps_bitmap.h"
using namespace Common::FpsOverlay;
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path local(argv[1]);
    assert(!EdenXbox::ReadShowFps(local));
    { std::ofstream out(local / "eden_settings.txt");
      out << "# keep\nresolution_setup=6\nother=a=b\nshow_fps=true\nshow_fps=false\n"; }
    for (const bool value : {true, false, true, false}) {
        assert(EdenXbox::SaveShowFps(local, value));
        assert(EdenXbox::ReadShowFps(local) == value);
        std::ifstream in(local / "eden_settings.txt");
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        assert(text == std::string("# keep\nresolution_setup=6\nother=a=b\nshow_fps=") +
                           (value ? "true\n" : "false\n"));
    }
    assert(!EdenXbox::SaveShowFps(local / "missing" / "directory", true));
    assert(!EnvironmentForces(nullptr));
    for (const auto value : {"", "0", "true", "01", "1extra", " 1"})
        assert(!EnvironmentForces(value));
    assert(EnvironmentForces("1"));
    Initialize(false, std::getenv("NXBOX_SHOW_FPS"));
    const bool expected = EnvironmentForces(std::getenv("NXBOX_SHOW_FPS"));
    assert(enabled.load() == expected);
    SetPreference(true);
    assert(enabled.load());
    SetPreference(false);
    assert(enabled.load() == expected);
    // The forced state is runtime-only and never changes the persisted preference.
    assert(!EdenXbox::ReadShowFps(local));
    Initialize(false, nullptr);
    int draws = 0;
    const auto before = guest_presents.load();
    DrawIfEnabled([&] { ++draws; });
    GuestPresent();
    assert(draws == 0 && guest_presents.load() == before);
    SetPreference(true);
    GuestPresent();
    DrawIfEnabled([&] { ++draws; });
    assert(draws == 1 && guest_presents.load() == before + 1);
    SetPreference(false);
    DrawIfEnabled([&] { ++draws; });
    assert(draws == 1);
    Sample sample;
    const Sample::Clock::time_point start{};
    sample.Update(start, 100, 0);
    sample.Update(start + std::chrono::milliseconds(500), 115, 0);
    assert(sample.fps == 0);
    sample.Update(start + std::chrono::seconds(1), 130, 0);
    assert(sample.fps == 30 && sample.milliseconds > 33.3 && sample.milliseconds < 33.4);
    sample.Update(start + std::chrono::seconds(2), 190, 0);
    assert(sample.fps == 60 && sample.milliseconds > 16.6 && sample.milliseconds < 16.7);
    sample.Update(start + std::chrono::seconds(3), 190, 0);
    assert(sample.fps == 0 && sample.milliseconds == 0);
    // A long disabled interval must not dilute the next enabled window.
    sample.Update(start + std::chrono::seconds(100), 200, 1);
    assert(sample.fps == 0);
    sample.Update(start + std::chrono::seconds(102), 260, 1);
    assert(sample.fps == 30);
    using D3D12::Present::FpsGlyph;
    for (const char c : std::string("0123456789FPSms.-"))
        assert(FpsGlyph(c) != 0 && (FpsGlyph(c) >> 28) == 0);
    assert(FpsGlyph(' ') == 0);
    assert(FpsGlyph('S') != FpsGlyph('3'));
}
"""


class FpsOverlayTests(unittest.TestCase):
    def test_persistence_environment_disabled_draw_and_sampling(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-fps-") as directory:
            work = Path(directory)
            source = work / "driver.cpp"
            binary = work / "driver"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++17",
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
            for value in (None, "0", "1", "1extra"):
                with self.subTest(environment=value), tempfile.TemporaryDirectory() as state:
                    environment = dict(os.environ)
                    environment.pop("NXBOX_SHOW_FPS", None)
                    if value is not None:
                        environment["NXBOX_SHOW_FPS"] = value
                    subprocess.run([str(binary), state], env=environment, check=True, timeout=10)

    def test_only_display_draws_overlay(self):
        renderer = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        capture = renderer.split("    void Capture(", 1)[1].split("    void Present(", 1)[0]
        self.assertNotIn("fps_counter", capture)
        present = renderer.split("    void Present(", 1)[1].split(
            "RendererD3D12::RendererD3D12", 1
        )[0]
        guarded = present.split("FpsOverlay::DrawIfEnabled([&]", 1)[1].split("});", 1)[0]
        self.assertIn("std::make_unique<Present::FpsCounter>", guarded)
        self.assertIn("fps_counter->Draw(Rtv(index), width, height)", guarded)
        self.assertLess(present.index("Draw(output_layout"), present.index("fps_counter->Draw"))
        self.assertLess(
            present.index("fps_counter->Draw"), present.index("D3D12_RESOURCE_STATE_PRESENT")
        )

    def test_guest_source_and_live_setting_are_wired(self):
        composer = (ROOT / "src/core/hle/service/nvnflinger/hardware_composer.cpp").read_text()
        self.assertIn("game_buffer_acquired |= !layer->is_overlay && layer->visible", composer)
        self.assertIn(
            "if (game_buffer_acquired)\n            Common::FpsOverlay::GuestPresent();", composer
        )
        ui = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        self.assertIn("SaveShowFps(local_state_, !show_fps_)", ui)
        self.assertIn("Common::FpsOverlay::SetPreference(show_fps_)", ui)
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        self.assertIn(
            "FpsOverlay::Initialize(ReadShowFps(local_state), show_fps)",
            session,
        )
        for language, label in (("en.inc", "Show FPS"), ("pt_br.inc", "Mostrar FPS")):
            self.assertIn(
                f'{{Text::ShowFps, L"{label}"}}',
                (ROOT / "src/eden_uwp/ui/lang" / language).read_text(),
            )


if __name__ == "__main__":
    unittest.main()
