# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production pre-submit validation with host D3D12 interface doubles."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"

DRIVER = r"""
#include <cassert>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
using UINT = unsigned;
using UINT64 = std::uint64_t;
using DXGI_FORMAT = unsigned;
using D3D12_QUERY_TYPE = unsigned;
constexpr unsigned D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER = 1;
constexpr unsigned D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE = 1;
constexpr unsigned D3D12_RESOURCE_DIMENSION_BUFFER = 1;
constexpr unsigned D3D12_RESOURCE_DIMENSION_TEXTURE3D = 4;
constexpr unsigned D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT = 1;
constexpr unsigned D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX = 0;
constexpr unsigned D3D12_QUERY_TYPE_PIPELINE_STATISTICS = 2;
constexpr unsigned D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 = 4;
constexpr unsigned D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3 = 7;
struct D3D12_QUERY_DATA_PIPELINE_STATISTICS { UINT64 counters[11]; };
struct D3D12_QUERY_DATA_SO_STATISTICS { UINT64 counters[2]; };
struct D3D12_RESOURCE_DESC {
    unsigned Dimension{};
    UINT64 Width{};
    unsigned Height{}, DepthOrArraySize{}, MipLevels{}, Format{};
};
struct ID3D12Resource {
    D3D12_RESOURCE_DESC desc;
    auto GetDesc() { return desc; }
};
struct ID3D12QueryHeap {
    // Like the real interface: no GetDesc.
    struct Description { unsigned Count; } desc;
};
struct ID3D12DescriptorHeap {
    struct Description { unsigned Type, Flags, NumDescriptors; } desc;
    struct Handle { UINT64 ptr; } base;
    auto GetDesc() { return desc; }
    auto GetGPUDescriptorHandleForHeapStart() { return base; }
};
constexpr int D3D12_FEATURE_FORMAT_INFO = 1;
constexpr DXGI_FORMAT DXGI_FORMAT_BC1_TYPELESS = 70, DXGI_FORMAT_BC5_SNORM = 84;
constexpr DXGI_FORMAT DXGI_FORMAT_BC6H_TYPELESS = 94, DXGI_FORMAT_BC7_UNORM_SRGB = 99;
struct D3D12_FEATURE_DATA_FORMAT_INFO { DXGI_FORMAT Format; unsigned PlaneCount; };
struct ID3D12Device {
    int CheckFeatureSupport(int, void* data, size_t) {
        auto* info = static_cast<D3D12_FEATURE_DATA_FORMAT_INFO*>(data);
        info->PlaneCount = info->Format == 44 ? 2 : 1; return 0;
    }
    unsigned GetDescriptorHandleIncrementSize(unsigned type) { return type ? 16 : 32; }
};
struct D3D12_TEXTURE_COPY_LOCATION {
    ID3D12Resource* pResource{};
    unsigned Type{}, SubresourceIndex{};
    struct Placed {
        UINT64 Offset{};
        struct FootprintDesc { unsigned Width{}, Height{}, Depth{}, Format{}, RowPitch{}; } Footprint;
    } PlacedFootprint;
};
struct D3D12_BOX { unsigned left, top, front, right, bottom, back; };
namespace D3D12 {
bool flag = true;
std::vector<std::string> logs;
bool BisectEnabled(const char*) { return flag; }
void Diagnostic(const std::string& text) { logs.push_back(text); }
}
#include "video_core/renderer_d3d12/d3d12_command_validation.h"
using namespace D3D12;
template <class F> void rejects(F f, const char* name) {
    auto count = logs.size();
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected && logs.size() == count + 1);
    assert(logs.back().find(name) != std::string::npos);
}
int main() {
    static_assert(ValidCopyRange(32, 24, 8));
    static_assert(!ValidCopyRange(32, 24, 9));
    static_assert(!ValidCopyRange(32, UINT64_MAX, 8));
    static_assert(!ValidCopyRange(UINT64_MAX, UINT64_MAX - 1, 8));
    static_assert(ValidDescriptorRange(4096, 4096 + 31 * 32, 32, 32));
    static_assert(!ValidDescriptorRange(4096, 4096 + 32 * 32, 32, 32));
    static_assert(!ValidDescriptorRange(4096, 4097, 32, 32));
    static_assert(!ValidDescriptorRange(4096, 4095, 32, 32));
    static_assert(!ValidDescriptorRange(4096, 4096 + 31 * 32, 32, 32, 2));
    static_assert(!ValidDescriptorRange(0, 0, 32, 32));
    CommandValidation v;
    v.Begin(123);
    v.Root("cbv", 4, 65536, true);
    v.Root("srv", 2, 65540, false);
    rejects([&] { v.Root("cbv", 4, 0, true); }, "cbv");
    rejects([&] { v.Root("cbv", 4, 65537, true); }, "cbv");
    rejects([&] { v.Root("uav", 64, 65536, false); }, "uav");
    ID3D12Device device;
    ID3D12DescriptorHeap views{{0, 1, 32}, {4096}}, samplers{{1, 1, 16}, {8192}};
    ID3D12DescriptorHeap* heaps[]{&views, &samplers};
    v.Heaps(&device, 2, heaps);
    v.Table("table", 1, 4096 + 31 * 32);
    v.Table("table", 3, 8192 + 15 * 16);
    rejects([&] { v.Table("table", 1, 4096 + 32 * 32); }, "table");
    // The guest handle must never be used as a native descriptor heap index.
    rejects([&] { v.Table("table", 1, 4096ull + 1048575ull * 32); }, "table");
    rejects([&] { v.Table("table", 1, 0xffffffff); }, "table");
    rejects([&] { v.Table("table", 1, 4097); }, "table");
    ID3D12Resource buffer{{1, 256, 1, 1, 1, 0}};
    v.BufferCopy(&buffer, 248, &buffer, 0, 8);
    rejects([&] { v.BufferCopy(&buffer, 249, &buffer, 0, 8); }, "CopyBufferRegion");
    rejects([&] { v.BufferCopy(&buffer, 0, &buffer, UINT64_MAX, 8); }, "CopyBufferRegion");
    v.BufferCopy(&buffer, 0, &buffer, 0, 0);
    assert(logs.back().find("zero_size_copy") != std::string::npos);
    ID3D12QueryHeap query{{32}};
    v.Resolve(&query, 0, 31, 1, &buffer, 248);
    v.Resolve(&query, 2, 0, 1, &buffer, 0);
    rejects([&] { v.Resolve(&query, 0, 0, 1, &buffer, 1); }, "ResolveQueryData");
    rejects([&] { v.Resolve(&query, 2, 0, 3, &buffer, 0); }, "ResolveQueryData");
    ID3D12Resource texture{{3, 8, 8, 6, 4, 28}}, volume{{4, 8, 8, 8, 4, 28}};
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = dst.pResource = &texture;
    v.TextureCopy(&device, &dst, 0, 0, 0, &src, nullptr);
    src.SubresourceIndex = dst.SubresourceIndex = 4 * 5 + 3;
    auto extent = v.CopyExtent(&device, &src);
    assert(extent.valid && extent.width == 1 && extent.height == 1 && extent.depth == 1);
    v.TextureCopy(&device, &dst, 0, 0, 0, &src, nullptr);
    rejects([&] { v.TextureCopy(&device, &dst, 1, 0, 0, &src, nullptr); }, "CopyTextureRegion");
    src = {}; src.pResource = &volume; src.SubresourceIndex = 2;
    extent = v.CopyExtent(&device, &src);
    assert(extent.valid && extent.width == 2 && extent.height == 2 && extent.depth == 2);
    dst = src;
    D3D12_BOX box{0, 0, 0, 3, 2, 2};
    rejects([&] { v.TextureCopy(&device, &dst, 0, 0, 0, &src, &box); }, "CopyTextureRegion");
    box.right = 0;
    v.TextureCopy(&device, &dst, 0, 0, 0, &src, &box);
    assert(logs.back().find("zero_size_copy") != std::string::npos);
    // A layer beyond the last array layer is not an imaginary second plane.
    src = {}; src.pResource = &texture; src.SubresourceIndex = 24;
    assert(!v.CopyExtent(&device, &src).valid);
    texture.desc.Format = 44; // R24G8 typeless has a second plane.
    assert(v.CopyExtent(&device, &src).valid);
    texture.desc.Format = 28;
    ID3D12Resource compressed{{3, 1, 1, 1, 1, 71}};
    dst = {}; dst.pResource = &compressed;
    extent = v.CopyExtent(&device, &dst);
    assert(extent.valid && extent.width == 4 && extent.height == 4);
    src = {}; src.pResource = &buffer; src.Type = 1;
    src.PlacedFootprint.Footprint = {4, 4, 1, 71, 256};
    v.TextureCopy(&device, &dst, 0, 0, 0, &src, nullptr);
    // A 4x4 BC mip occupies one block row, not four 256-byte rows.
    assert(v.CopyExtent(&device, &src).valid);
    const auto summary = v.Summary();
    assert(summary.find("before_execute") != std::string::npos);
    v.ViewFormat("CreateUnorderedAccessView", 9, 12);
    rejects([&] { v.ViewFormat("CreateUnorderedAccessView", 9, 17); }, "CreateUnorderedAccessView");
    rejects([&] { v.ViewFormat("CreateShaderResourceView", 9, 16); }, "CreateShaderResourceView");
    rejects([&] { v.ViewFormat("CreateRenderTargetView", 27, 42); }, "CreateRenderTargetView");
    const std::array<std::uint32_t, 2> castable{12, 17};
    v.ViewFormat("CreateUnorderedAccessView", 9, 17, castable);
    rejects([&] { v.ViewFormat("CreateShaderResourceView", 9, 16, castable); }, "CreateShaderResourceView");
    v.Begin(124);
    assert((v.counts == std::array<UINT64, 5>{}));
    rejects([&] { v.Table("table", 1, 4096); }, "table");
    flag = false;
    CommandValidation disabled;
    const auto prior = logs.size();
    disabled.ViewFormat("CreateUnorderedAccessView", 9, 17);
    disabled.Root("cbv", 0, 0, true);
    disabled.Table("table", 0, 0);
    disabled.BufferCopy(nullptr, 0, nullptr, 0, 8);
    disabled.Resolve(nullptr, 0, 0, 1, nullptr, 0);
    disabled.TextureCopy(&device, nullptr, 0, 0, 0, nullptr, nullptr);
    assert(logs.size() == prior);
}
"""

# Only formatting is replaced; the production validator and D3D12 arithmetic run
# unchanged. This keeps the host test independent of installed fmt/Windows SDKs.
FMT = r"""
#pragma once
#include <sstream>
#include <string>
namespace fmt {
template <class... Args> std::string format(const char* pattern, Args... args) {
    std::ostringstream out;
    out << pattern;
    ((out << ' ' << args), ...);
    return out.str();
}
}
"""


class CommandValidationTests(unittest.TestCase):
    def test_rejects_invalid_arguments_before_native_calls(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-command-validation-") as directory:
            root = Path(directory)
            (root / "fmt").mkdir()
            (root / "fmt/format.h").write_text(FMT)
            source = root / "validation.cpp"
            binary = root / "validation"
            source.write_text("#include <climits>\n" + DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(root),
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_immediate_deferred_and_batched_paths_are_instrumented(self):
        facade = (BACKEND / "d3d12_commands.h").read_text()
        for stage in ("Compute", "Graphics"):
            for kind in ("ConstantBuffer", "ShaderResource", "UnorderedAccess"):
                name = f"Set{stage}Root{kind}View"
                body = facade.split(f"void {name}(", 1)[1]
                self.assertLess(body.index("validation.Root("), body.index("if (!stream)"))
        for call in ("Heaps", "Table", "BufferCopy", "TextureCopy", "Resolve"):
            self.assertIn(f"validation.{call}(", facade)
        context = (BACKEND / "d3d12_context.cpp").read_text()
        self.assertIn("facade.validation.Begin(CurrentTick())", context)
        seal = context.split("u64 Context::SealRecording", 1)[1].split("void Context::", 1)[0]
        self.assertLess(
            seal.index("facade.validation.Summary()"), seal.index("->RecorderSubmit(tick)")
        )
        submit = context.split("u64 Context::Submit(", 1)[1].split("void Context::", 1)[0]
        self.assertLess(
            submit.index("facade.validation.Summary()"), submit.index("queue->ExecuteCommandLists")
        )
        draw = (BACKEND / "d3d12_rasterizer.cpp").read_text()
        self.assertIn("batch_root_bindings && commands->validation.enabled", draw)
        descriptors = (BACKEND / "d3d12_descriptor.cpp").read_text()
        self.assertIn("unwritten_slot={}", descriptors)
        self.assertIn("if (slot >= table.count)", descriptors)


if __name__ == "__main__":
    unittest.main()
