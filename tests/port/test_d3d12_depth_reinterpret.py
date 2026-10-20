# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production alias preflight without the Windows SDK."""

import unittest
from pathlib import Path

from test_d3d12_depth_recording import function_body, run_cpp

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"
SOURCE = (NATIVE / "d3d12_texture_cache.cpp").read_text()
METHOD = function_body(SOURCE, "bool TextureCacheRuntime::ReinterpretDepthOnGpu(")
DRIVER = r"""
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_formats.h"
using u64 = std::uint64_t;
using u32 = std::uint32_t;
namespace VideoCommon {
enum class ImageType { e2D, e3D };
struct SubresourceLayers { int base_level{}, base_layer{}, num_layers{1}; };
struct Offset3D { int x{}, y{}, z{}; };
struct Extent3D { unsigned width{1280}, height{720}, depth{1}; };
struct ImageCopy {
    SubresourceLayers src_subresource, dst_subresource;
    Offset3D src_offset, dst_offset;
    Extent3D extent;
};
}
namespace D3D12 {
bool disabled = false, legacy = false;
unsigned GetEnvironmentVariableA(const char* name, char* value, unsigned) {
    assert(std::string_view{name} == "NXBOX_D3D12_GPU_DEPTH_REINTERPRET");
    value[0] = disabled ? '0' : '1';
    return disabled ? 1 : 0;
}
bool BisectEnabled(const char*) { return legacy; }
enum class FrameCount { GpuDepthReinterpretBytes };
struct Image {
    struct Info {
        VideoCommon::ImageType type{VideoCommon::ImageType::e2D};
        unsigned num_samples{1};
        struct { int levels{1}, layers{1}; } resources;
        VideoCommon::Extent3D size;
    } info;
    std::shared_ptr<int> resource = std::make_shared<int>(0);
    FormatInfo format = LookupFormat("A8B8G8R8_UNORM");
    bool scaled = false;
    auto Handle() const { return resource; }
    auto HostFormat() const { return format; }
    bool IsRescaled() const { return scaled; }
};
struct Surface { std::shared_ptr<int> resource; FormatInfo format; };
struct Helper {
    unsigned calls{};
    void CopyImage(Surface, Surface, std::span<const VideoCommon::ImageCopy>) { ++calls; }
};
struct Profile {
    u64 bytes{};
    void Count(FrameCount, u64 value) { bytes += value; }
};
struct Context {
    D3D12::Profile profile;
    auto& Profile() { return profile; }
};
struct TextureCacheRuntime {
    Context context;
    Helper helper;
    Helper& BlitHelpers() { return helper; }
    bool ReinterpretDepthOnGpu(Image&, Image&, std::span<const VideoCommon::ImageCopy>);
};
__METHOD__
}
int main() {
    using namespace D3D12;
    TextureCacheRuntime runtime;
    Image depth, color;
    depth.format = LookupFormat("D24_UNORM_S8_UINT");
    VideoCommon::ImageCopy copy;
    std::vector<VideoCommon::ImageCopy> copies{copy};
    __TEST__
}
"""


def execute(test):
    run_cpp(DRIVER.replace("__METHOD__", METHOD).replace("__TEST__", test))


class DepthReinterpretTests(unittest.TestCase):
    def test_gpu_batch_and_all_or_nothing_preflight(self):
        execute(r"""
    assert(runtime.ReinterpretDepthOnGpu(color, depth, copies));
    assert(runtime.context.profile.bytes == 1280ULL * 720 * 4);
    assert(runtime.helper.calls == 1);
    color.format = LookupFormat("A8B8G8R8_SRGB");
    copy.src_offset = {1, 2, 0}; copy.dst_offset = {3, 4, 0};
    copy.extent = {32, 16, 1};
    copies = {copy};
    assert(runtime.ReinterpretDepthOnGpu(depth, color, copies));
    assert(runtime.helper.calls == 2);
    copies.push_back(copy);
    copies.back().dst_subresource.base_level = 1;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    assert(runtime.helper.calls == 2); // No partial write before a late rejection.
    copies = {copy};
    copies[0].src_offset.x = -1;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    copies[0] = copy; copies[0].dst_offset.y = 720;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    copies[0] = copy; copies[0].extent.depth = 2;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    copies[0] = copy; copies[0].src_subresource.num_layers = 2;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    copies[0] = copy;
    color.scaled = true;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    color.scaled = false; color.info.num_samples = 4;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    color.info.num_samples = 1; color.info.type = VideoCommon::ImageType::e3D;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    color.info.type = VideoCommon::ImageType::e2D;
    for (auto name : {"X8_D24_UNORM", "D32_FLOAT_S8_UINT",
                      "D32_FLOAT", "S8_UINT"}) {
        depth.format = LookupFormat(name);
        assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    }
    depth.format = LookupFormat("D24_UNORM_S8_UINT");
    for (auto name : {"A8B8G8R8_UINT", "A8B8G8R8_SNORM", "B8G8R8A8_UNORM"}) {
        color.format = LookupFormat(name);
        assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    }
    color.format = LookupFormat("A8B8G8R8_UNORM");
    depth.format = LookupFormat("S8_UINT_D24_UNORM");
    assert(runtime.ReinterpretDepthOnGpu(depth, color, copies));
    assert(runtime.ReinterpretDepthOnGpu(color, depth, copies));
    color.info.resources = {2, 3}; depth.info.resources = {2, 3};
    copies[0].src_subresource = {1, 1, 2};
    copies[0].dst_subresource = {1, 1, 2};
    assert(runtime.ReinterpretDepthOnGpu(depth, color, copies));
    copies[0].dst_subresource.base_layer = 2;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    copies[0] = copy;
    legacy = true;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    legacy = false; color.resource = depth.resource;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    assert(!runtime.ReinterpretDepthOnGpu(depth, depth, copies));
    assert(runtime.helper.calls == 5);
""")

    def test_zero_opt_out_preserves_cpu_fallback(self):
        execute(r"""
    disabled = true;
    assert(!runtime.ReinterpretDepthOnGpu(depth, color, copies));
    assert(!runtime.helper.calls && !runtime.context.profile.bytes);
""")

    def test_gpu_dispatch_precedes_cpu_download_and_reupload(self):
        body = function_body(SOURCE, "void TextureCacheRuntime::ReinterpretImage(")
        self.assertLess(body.index("ReinterpretDepthOnGpu"), body.index("DownloadStagingBuffer"))
        self.assertIn("CpuReinterpretBytes", body)
        for forbidden in ("Finish(", "StagingBuffer(", "UploadMemory(", "DownloadMemory("):
            self.assertNotIn(forbidden, METHOD)
        blit = (NATIVE / "d3d12_blit_image.cpp").read_text()
        copy = function_body(blit, "void BlitImageHelper::CopyImage(")
        self.assertIn("BlitConversion::D24S8ToRGBA8", copy)
        self.assertIn("BlitConversion::RGBA8ToD24S8", copy)
        self.assertIn("impl->Draw(RawSurface(d), RawSurface(s)", copy)
        self.assertIn("ReadStencil(coord, sampleId) << 24", blit)
        self.assertIn("(packed << 8) | (packed >> 24)", blit)
        self.assertIn("(packed >> 8) | (packed << 24)", blit)
        header = (NATIVE / "d3d12_blit_image.h").read_text()
        enum = header[header.index("enum class BlitConversion") :].split("};", 1)[0] + "};"
        run_cpp(
            "#include <cstdint>\nusing u32 = std::uint32_t;\n"
            + enum
            + "\nstatic_assert(u32(BlitConversion::S8D24ToRGBA8) == 6);"
            + "\nstatic_assert(u32(BlitConversion::RGBA8ToS8D24) == 7);\nint main() {}"
        )

    def test_d24_shader_numeric_roundtrip_and_guest_byte_order(self):
        # D24 SRV/DSV normalized floats must recover all 24 depth bits; test the
        # entire depth domain, then pack stencil extremes using the CPU oracle.
        run_cpp(r"""
#include <cassert>
#include <cmath>
#include <cstdint>
#include "video_core/renderer_d3d12/d3d12_formats.h"
int main() {
    for (std::uint32_t depth = 0; depth <= 0xffffff; ++depth) {
        const float normalized = float(depth) / 16777215.0f;
        assert(std::uint32_t(std::round(normalized * 16777215.0f)) == depth);
    }
    for (auto name : {"D24_UNORM_S8_UINT", "S8_UINT_D24_UNORM"}) {
    for (auto packed : {0u, 0x12345678u, 0xff000000u, 0xffffffffu}) {
        std::uint8_t guest[4], result[4]{};
        std::memcpy(guest, &packed, 4);
        std::uint32_t host_depth{};
        std::uint8_t host_stencil{};
        D3D12::ConvertDepthRow(name,
            reinterpret_cast<std::uint8_t*>(&host_depth), guest, 1, 0, false);
        D3D12::ConvertDepthRow(name, &host_stencil, guest, 1, 1, false);
        auto shader = host_depth | (std::uint32_t(host_stencil) << 24);
        if (std::string_view{name} == "S8_UINT_D24_UNORM")
            shader = (shader << 8) | (shader >> 24);
        assert(shader == packed);
        D3D12::ConvertDepthRow(name,
            reinterpret_cast<std::uint8_t*>(&host_depth), result, 1, 0, true);
        D3D12::ConvertDepthRow(name, &host_stencil, result, 1, 1, true);
        assert(std::memcmp(guest, result, 4) == 0);
    }
    }
}
""")
