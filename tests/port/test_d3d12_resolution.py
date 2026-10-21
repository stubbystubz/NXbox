# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host coverage for fractional render-target scaling and its D3D12 integration."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <cmath>
#include <sstream>
#include "eden_uwp/title_settings.h"
#include "video_core/renderer_d3d12/d3d12_resolution.h"
#include "video_core/renderer_d3d12/d3d12_present/present_math.h"
int main() {
    using D3D12::ResolutionScale;
    for (const auto scale : {ResolutionScale{1, 1}, ResolutionScale{3, 2},
                             ResolutionScale{1, 0}, ResolutionScale{2, 0}}) {
        const float factor = scale.Factor();
        assert(scale.Size(1280) == static_cast<unsigned>(1280 * factor));
        assert(scale.Size(720) == static_cast<unsigned>(720 * factor));
        assert(scale.Size(1) >= 1);
        assert(scale.Size(0) == 0);
        assert(scale.Size(1279) == static_cast<unsigned>(1279 * factor));
        assert(scale.Coordinate(3.5f) == 3.5f * factor);
        assert(scale.Coordinate(-3.5f) == -3.5f * factor);
        // Half-open scissor edges round outward, preserve empty rectangles,
        // and support negative origins before the render-area intersection.
        const auto rect = scale.Rect(3, -3, 101, 99);
        assert(rect.left == std::floor(3 * factor));
        assert(rect.top == std::floor(-3 * factor));
        assert(rect.right == std::ceil(101 * factor));
        assert(rect.bottom == std::ceil(99 * factor));
        const auto empty = scale.Rect(3, 3, 3, 3);
        assert(empty.left == empty.right && empty.top == empty.bottom);
        const auto crop = D3D12::Present::NormalizeCrop({160, 90, 1120, 630}, {1280, 720});
        assert(crop.left == .125f && crop.right == .875f);
        assert(scale.PresentCoordinate(160, 1280, scale.Size(1280)) ==
               160.f * scale.Size(1280) / 1280.f);
        assert(scale.PresentCoordinate(1280, 1280, scale.Size(1280)) /
                   scale.Size(1280) == 1.f);
    }
    using namespace EdenXbox::TitleSettings;
    assert((kResolutionValues == std::array<int, 6>{1, 2, 3, 5, 6, 7}));
    assert(kDefaultResolution == 3);
    std::istringstream settings;
    assert(ReadResolution(settings) == 3);
}
"""


class ResolutionTests(unittest.TestCase):
    def test_host_scaling_math_and_menu_values(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "resolution.cpp"
            binary = Path(directory) / "resolution"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
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
            subprocess.run([str(binary)], check=True)

    def test_actual_image_scale_transitions(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        methods = source[
            source.index("bool Image::ScaleUp(") : source.index("void Image::UploadMemory(")
        ]
        driver = (
            r"""
#include <cassert>
#include <memory>
#include "video_core/renderer_d3d12/d3d12_resolution.h"
namespace Settings {
struct Resolution { unsigned up_scale, down_shift; bool active; };
struct Values { Resolution resolution_info; } values;
}
namespace VideoCommon { enum ImageFlagBits { Rescaled = 1 }; }
namespace D3D12 {
struct Info { struct Size { unsigned width, height; } size; bool rescaleable{true}; };
using ResourcePtr = std::shared_ptr<Info>;
unsigned allocations = 0, blits = 0;
ResourcePtr MakeTexture(int, const Info& info, int) {
    ++allocations;
    return std::make_shared<Info>(info);
}
struct Runtime { int context{}; } runtime;
struct Image {
    Info info{{1280, 720}};
    Runtime* runtime = &D3D12::runtime;
    int host_format{}, flags{};
    bool has_scaled{};
    ResourcePtr original_resource = std::make_shared<Info>(info);
    ResourcePtr resource = original_resource, scaled_resource;
    bool IsRescaled() const { return (flags & VideoCommon::Rescaled) != 0; }
    void BlitScale(const ResourcePtr& dst, const ResourcePtr& src) {
        assert(dst && src && dst != src);
        ++blits;
    }
    bool ScaleUp(bool ignore = false);
    bool ScaleDown(bool ignore = false);
};
"""
            + methods
            + r"""
}
int main() {
    using namespace D3D12;
    for (const auto resolution : {Settings::Resolution{1, 1, true},
                                 Settings::Resolution{3, 2, true},
                                 Settings::Resolution{1, 0, false},
                                 Settings::Resolution{2, 0, true}}) {
        Settings::values.resolution_info = resolution;
        Image image;
        const auto initial_allocations = allocations;
        const auto initial_blits = blits;
        assert(image.ScaleUp(true) == resolution.active);
        assert(image.IsRescaled() == resolution.active);
        assert(!image.ScaleUp());
        if (!resolution.active) {
            assert(!image.ScaleDown());
            assert(allocations == initial_allocations && blits == initial_blits);
            continue;
        }
        const ResolutionScale scale{resolution.up_scale, resolution.down_shift};
        assert(image.resource->size.width == scale.Size(1280));
        assert(image.resource->size.height == scale.Size(720));
        assert(image.has_scaled && blits == initial_blits + 1);
        assert(image.ScaleDown());
        assert(image.resource == image.original_resource && !image.IsRescaled());
        assert(image.ScaleUp(true));
        assert(image.ScaleDown(true));
        assert(blits == initial_blits + 2);
        assert(image.ScaleUp());
        assert(allocations == initial_allocations + 1);
        assert(blits == initial_blits + 3);
    }
}
"""
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "transitions.cpp"
            binary = Path(directory) / "transitions"
            path.write_text(driver)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(path),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_backend_uses_scaled_storage_and_native_transfers(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        header = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.h").read_text()
        self.assertIn("bool Image::ScaleUp(bool ignore)", source)
        self.assertIn("bool Image::ScaleDown(bool ignore)", source)
        self.assertIn("BlitScale", source)
        self.assertIn("const bool restore_scaled = ScaleDown();", source)
        self.assertIn("ScaleUp(download)", source)
        self.assertIn("is_rescaled{image.IsRescaled()}", source)
        self.assertIn("return is_rescaled;", header)

    def test_rasterizer_scales_dynamic_state_and_clear(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        self.assertIn("scale.Coordinate(viewports[i].TopLeftX)", source)
        self.assertIn("scale.Rect(scissors[i].left", source)
        self.assertIn("scale.Rect(clip.left", source)
        self.assertIn("texture_cache.IsRescaling()", source)
        self.assertIn("words[4]", source)

    def test_draw_texture_normalizes_guest_coordinates(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        draw = source[
            source.index("void RasterizerD3D12::DrawTexture()") : source.index(
                "void RasterizerD3D12::Clear("
            )
        ]
        self.assertTrue("texture_cache.IsRescaling() ? static_cast<u32>(maxwell3d->regs.surface_clip.width)" in " ".join(draw.split()))
        self.assertTrue("texture_cache.IsRescaling() ? static_cast<u32>(maxwell3d->regs.surface_clip.height)" in " ".join(draw.split()))

    def test_shader_scaling_masks_follow_each_stage(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        emitter = (
            ROOT / "src/shader_recompiler/backend/hlsl/emit_hlsl_context_get_set.cpp"
        ).read_text()
        self.assertTrue("words[11 + stage] = texture_scaling_masks[stage]" in source)
        self.assertTrue("texture_scaling_masks[stage]" in source)
        self.assertTrue("nx_reserved1" in emitter)
        self.assertTrue("constants.texture_scaling_mask = compute_texture_scaling_mask" in source)

    def test_cached_shaders_are_partitioned_by_resolution(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp").read_text()
        self.assertIn("static_cast<u32>(resolution) << 16", source)
        self.assertRegex(source, r"Res1X\s*\? 6u")
        self.assertIn("!Int64AtomicsEnabled()", source)
        self.assertIn("!NativeInt64AtomicsEnabled()", source)
        self.assertTrue("{emitter_identity, 2," in source)

    def test_menu_and_present_are_not_fixed_to_native_scale(self):
        ui = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        for locale in ("en", "pt_br"):
            strings = (ROOT / f"src/eden_uwp/ui/lang/{locale}.inc").read_text()
            self.assertIn("Resolution0_5X", strings)
            self.assertIn("Resolution0_75X", strings)
        self.assertIn("TitleSettings::kResolutionValues.size()", ui)
        self.assertNotIn("std::min(3, resolution_choice_ + 1)", ui)
        present = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        self.assertIn("PresentCoordinate", present)


if __name__ == "__main__":
    unittest.main()
