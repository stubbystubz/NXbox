# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Artwork policy and host C++ color/gradient regression tests."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cmath>
#include "eden_uwp/ui/art_colors.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/art_cache.h"
using namespace EdenXbox::Ui;
int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace Art;
    assert(kGradientStops[0] == 0 && kGradientStops[1] == .48f && kGradientStops[2] == .72f);
    // Worst-case pure-white art under the actual scrim and the 64%-opaque metadata ink.
    const float background = 1.0f - kLeftScrimOpacity;
    const float meta = .64f * (245.0f / 255) + .36f * background;
    const float contrast = (Luminance({meta, meta, meta}) + .05f) /
                           (Luminance({background, background, background}) + .05f);
    assert(contrast >= 4.5f);
    assert(kLeftScrimEnd >= (96.0f + 1100.0f) / 1920.0f);
    assert(kBottomScrimStart >= .5f && kBottomScrimStart == .82f);
    const Palette ramp{{{1,0,0}, {0,1,0}, {0,0,1}}};
    assert(Gradient(ramp, 0).r == 1);
    assert(Gradient(ramp, .48f).g == 1);
    assert(Gradient(ramp, .72f).b == 1);
    assert(Gradient(ramp, 1).b == 1);
    assert(std::abs(Gradient(ramp, .24f).r - .5f) < .0001f);
    assert(std::abs(Gradient(ramp, .60f).b - .5f) < .0001f);
    std::vector<std::uint8_t> pixels(32*32*4);
    // Three broad source colors; k-means must retain all three, ordered by luminance.
    for (unsigned i=0; i<32*32; ++i) {
        pixels[i*4 + (i%3 == 0 ? 2 : i%3 == 1 ? 1 : 0)] = 255;
        pixels[i*4+3] = 255;
    }
    const auto colors = Dominant(pixels, 32, 32);
    assert(colors[0].g > .95f && colors[1].r > .95f && colors[2].b > .95f);
    assert(Luminance(colors[0]) > Luminance(colors[1]) && Luminance(colors[1]) > Luminance(colors[2]));
    for (unsigned i=0; i<32*32; ++i) {
        pixels[i*4] = 30; pixels[i*4+1] = 100; pixels[i*4+2] = 230;
    }
    const auto mono = Dominant(pixels, 32, 32);
    assert(mono[0].r > mono[0].g && mono[0].g > mono[0].b);
    assert(Luminance(mono[0]) - Luminance(mono[2]) >= .12f);
    std::vector<std::uint8_t> black(32*32*4, 0);
    for (unsigned i = 3; i < black.size(); i += 4) black[i] = 255;
    const auto dark = Dominant(black, 32, 32);
    assert(Luminance(dark[0]) - Luminance(dark[2]) >= .12f);
    const auto hero = Hero(pixels, 32, 32);
    assert(hero.size() == 192*76*4);
    for (unsigned i=3; i<hero.size(); i+=4) assert(hero[i] == 255);
    const auto bmp = EncodeBitmap(hero, 192, 76);
    assert(bmp.size() == hero.size() + 54 && bmp[0] == 'B' && bmp[1] == 'M');
    assert(bmp[10] == 54 && bmp[18] == 192 && bmp[22] == 180); // -76, top-down
    assert(bmp[26] == 1 && bmp[28] == 32 && bmp[54+3] == 255);
    assert(EncodeBitmap({1,2,3}, 1, 1).empty());
    assert(EncodeBitmap({}, 0, 0).empty());
    // Cleanup removes legacy images/index, preserves metadata and newly generated art.
    const fs::path local = argv[1];
    const auto folder = local / "library";
    fs::create_directories(folder);
    for (const auto name : {"0100000000000000.banner.jpg", "0100000000000000.banner.partial.partial",
                           "0100000000000000.jpg", "0100000000000000.jpg.partial",
                           "eshop-art.json", "eshop-art.json.partial", "0100000000000000.control.jpg",
                           "0100000000000000.control.bmp", "0100000000000000.json", "0100000000000000.mods.json"})
        std::ofstream(folder / name) << "cached";
    // A cleanup failure leaves no marker and can be retried on the next startup.
    const auto blocked = folder / "bad.banner.jpg";
    fs::create_directory(blocked);
    std::ofstream(blocked / "child") << "busy";
    PurgeLegacyArt(local);
    assert(!fs::exists(folder / "own-art-v1"));
    fs::remove_all(blocked);
    PurgeLegacyArt(local);
    assert(fs::exists(folder / "own-art-v1"));
    assert(!fs::exists(folder / "eshop-art.json"));
    assert(!fs::exists(folder / "0100000000000000.banner.jpg"));
    assert(!fs::exists(folder / "0100000000000000.jpg.partial"));
    assert(fs::exists(folder / "0100000000000000.control.jpg"));
    assert(fs::exists(folder / "0100000000000000.control.bmp"));
    assert(fs::exists(folder / "0100000000000000.json"));
    assert(fs::exists(folder / "0100000000000000.mods.json"));
    PurgeLegacyArt(local);
    assert(fs::exists(folder / "0100000000000000.control.jpg"));
    assert(WriteBytes(folder / "replace.bmp", {1,2,3}));
    assert(WriteBytes(folder / "replace.bmp", {4,5}));
    assert(fs::file_size(folder / "replace.bmp") == 2);
    assert(!fs::exists(folder / "replace.bmp.partial"));
    assert(Hero({}, 0, 0).empty());
    assert(Hero({1,2,3}, 2, 2).empty());
    const auto seed = Seeded("0100000000000000");
    const auto again = Seeded("0100000000000000");
    assert(seed[0].r == again[0].r && seed[0].g == again[0].g);
    assert(seed[0].r != Seeded("0100000000000001")[0].r);
    Tween fade(0, kEaseHero);
    const auto start = Clock::now();
    fade.To(1, start, kDurationScreen);
    assert(fade.Value(start) == 0 && fade.Value(start + kDurationScreen) == 1);
    assert(std::abs(fade.Value(start + std::chrono::milliseconds(200)) - kEaseHero(.5f)) < .0001f);
}
"""


class OwnArtTests(unittest.TestCase):
    def test_no_store_art_endpoints_or_index_in_sources_and_distribution(self):
        for directory in ("src", "dist", "tools", "docs", ".github"):
            for path in (ROOT / directory).rglob("*"):
                if not path.is_file() or "__pycache__" in path.parts:
                    continue
                data = path.read_bytes().lower()
                for forbidden in (b"nintendo.net", b"eshop-art"):
                    self.assertNotIn(forbidden, data, str(path.relative_to(ROOT)))

    def test_color_selection_gradient_and_hero(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-art-") as temporary:
            source = Path(temporary) / "driver.cpp"
            binary = Path(temporary) / "driver"
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
            subprocess.run([str(binary), str(Path(temporary) / "state")], check=True, timeout=10)

    def test_library_and_launch_never_create_a_network_art_source(self):
        for name in ("library_screen.cpp", "launch_screen.cpp"):
            source = (ROOT / "src/eden_uwp/ui" / name).read_text()
            self.assertNotIn("ShopArtSource", source)
            self.assertNotIn("HttpClient", source)
        transport = (ROOT / "src/eden_uwp/ui/art.cpp").read_text()
        self.assertIn("AllowAutoRedirect(false)", transport)
        self.assertIn("HttpCookieUsageBehavior::NoCookies", transport)
        self.assertNotIn("ShopArtSource", transport)


if __name__ == "__main__":
    unittest.main()
