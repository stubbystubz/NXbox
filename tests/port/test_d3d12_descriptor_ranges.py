# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise range compaction, colliding tables and production null-cache lifetime."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"

NULL_DRIVER = r"""
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
using UINT = unsigned;
using u32 = unsigned;
using u64 = std::uint64_t;
using u8 = unsigned char;
using SIZE_T = std::size_t;
enum D3D12_DESCRIPTOR_HEAP_TYPE { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER };
struct D3D12_CPU_DESCRIPTOR_HANDLE { SIZE_T ptr; };
struct D3D12_CONSTANT_BUFFER_VIEW_DESC { u64 BufferLocation; };
struct D3D12_SHADER_RESOURCE_VIEW_DESC { UINT ViewDimension, Format; };
struct D3D12_UNORDERED_ACCESS_VIEW_DESC { UINT Format; };
struct D3D12_SAMPLER_DESC { u64 data; };
struct Resource { struct { int* Get() const { return nullptr; } } handle; };
using ResourcePtr = std::shared_ptr<Resource>;
struct DescriptorKey {
    std::array<u8, 64> description{};
    std::uintptr_t resource{};
    u32 kind{}, size{};
    bool operator==(const DescriptorKey&) const = default;
};
struct KeyHash {
    size_t operator()(const DescriptorKey& key) const {
        return std::hash<std::string_view>{}(
            {reinterpret_cast<const char*>(key.description.data()), key.size}) ^ key.kind ^ key.resource;
    }
};
enum class FrameCount { DescriptorWrites, DescriptorWritesCbv, DescriptorWritesSrv,
                        DescriptorWritesUav, DescriptorWritesSampler };
struct FakeHeap {
    SIZE_T base;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart() { return {base}; }
};
struct FakeDevice {
    unsigned writes{};
    template<class... Args> void CreateConstantBufferView(Args...) { ++writes; }
    template<class... Args> void CreateShaderResourceView(Args...) { ++writes; }
    template<class... Args> void CreateUnorderedAccessView(Args...) { ++writes; }
    template<class... Args> void CreateSampler(Args...) { ++writes; }
};
struct FakeProfile { void Count(FrameCount) {} };
struct FakeContext {
    FakeDevice device;
    FakeProfile profile;
    u64 tick{1};
    u64 CurrentTick() const { return tick; }
    FakeDevice* Device() { return &device; }
    FakeProfile& Profile() { return profile; }
    void ValidateViewFormat(const ResourcePtr& resource, UINT, const char*) {
        assert(!resource); // Null descriptors have no resource format to validate.
    }
};
namespace fmt {
    template<class... Args> std::string format(const char*, Args...) { return {}; }
}
void Diagnostic(const std::string&) {}
bool BisectEnabled(const char*) { return false; }
template<class... Args> void TraceApiCall(Args...) {}
struct D3D12DescriptorAllocator {
    struct CachedDescriptor { D3D12_CPU_DESCRIPTOR_HANDLE handle; ResourcePtr resource; };
    struct DescriptorCache {
        FakeHeap* heap{};
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        std::unordered_map<DescriptorKey, CachedDescriptor, KeyHash> entries;
        u32 cursor{}, stride{32}, capacity{};
        u64 generation{1};
    };
    using CopyBatch = D3D12::DescriptorCopyBatch<D3D12_CPU_DESCRIPTOR_HANDLE>;
    FakeContext context;
    DescriptorCache view_cache, sampler_cache, null_view_cache;
    bool persistent_null_descriptors{true}, new_frame{true};
    u64 cache_tick{};
    std::unordered_map<SIZE_T, DescriptorKey> slot_contents;
    void FlushCopies(D3D12_DESCRIPTOR_HEAP_TYPE, CopyBatch&) {}
    void RefreshCaches();
    D3D12_CPU_DESCRIPTOR_HANDLE ResolveDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE,
        const ResourcePtr&, const DescriptorKey&, CopyBatch*);
};
__PRODUCTION__
DescriptorKey Key(unsigned value) {
    DescriptorKey key{};
    key.kind = 1;
    key.size = sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC);
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{value, 28};
    std::memcpy(key.description.data(), &desc, sizeof(desc));
    return key;
}
int main() {
    FakeHeap views{1024}, samplers{2048}, nulls{4096};
    D3D12DescriptorAllocator allocator;
    allocator.view_cache.heap = &views;
    allocator.view_cache.cpu = {views.base};
    allocator.view_cache.capacity = 2;
    allocator.sampler_cache.heap = &samplers;
    allocator.sampler_cache.cpu = {samplers.base};
    allocator.sampler_cache.capacity = 2;
    allocator.null_view_cache.heap = &nulls;
    allocator.null_view_cache.cpu = {nulls.base};
    allocator.null_view_cache.capacity = 2;
    const auto resolve = [&](unsigned value) {
        allocator.RefreshCaches();
        return allocator.ResolveDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                           {}, Key(value), nullptr).ptr;
    };
    const auto first = resolve(1), second = resolve(2);
    assert(first != second);
    for (unsigned frame = 0; frame < 10; ++frame) {
        allocator.new_frame = true;
        ++allocator.context.tick;
        assert(resolve(1) == first && resolve(2) == second);
    }
    assert(allocator.context.device.writes == 2);
    // Full permanent heap falls back; transient rollover cannot alter null slots.
    for (unsigned i = 3; i < 30; ++i)
        resolve(i);
    assert(allocator.null_view_cache.cursor == 2);
    const auto writes = allocator.context.device.writes;
    assert(resolve(1) == first && resolve(2) == second);
    assert(allocator.context.device.writes == writes);
    // Opt-out recreates the same null description once per frame, as before.
    allocator.persistent_null_descriptors = false;
    for (unsigned frame = 0; frame < 10; ++frame) {
        allocator.new_frame = true;
        ++allocator.context.tick;
        resolve(1);
        resolve(1);
    }
    assert(allocator.context.device.writes == writes + 10);
    std::cout << "null views: 2 creations across 10 frames; rollover safe; opt-out=10 creations\n";
}
"""


class DescriptorRangesTests(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory(prefix="nxbox-descriptor-ranges-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(source)
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

    def test_ranges_and_colliding_tables(self):
        self.compile_run((ROOT / "tests/port/d3d12_descriptor_ranges_test.cpp").read_text())

    def test_production_null_cache(self):
        source = (NATIVE / "d3d12_descriptor.cpp").read_text()
        functions = []
        for signature in (
            "void D3D12DescriptorAllocator::RefreshCaches()",
            "D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::ResolveDescriptor(",
        ):
            body = source[source.index(signature) :]
            functions.append(body[: body.index("\n}\n") + 3])
        self.compile_run(NULL_DRIVER.replace("__PRODUCTION__", "\n".join(functions)))

    def test_production_default_and_zero_opt_out(self):
        source = (NATIVE / "d3d12_descriptor.cpp").read_text()
        checks = []
        for knob in (
            "NXBOX_D3D12_DESCRIPTOR_RANGES",
            "NXBOX_D3D12_PERSISTENT_NULL_DESCRIPTORS",
            "NXBOX_D3D12_BATCH_DESCRIPTOR_COPIES",
            "NXBOX_D3D12_HANDLE_TABLE_CACHE",
        ):
            start = source.index(f'const char* value = std::getenv("{knob}")')
            end = source.index('return !value || std::strcmp(value, "0") != 0;', start)
            body = source[start : end + len('return !value || std::strcmp(value, "0") != 0;')]
            checks.append(
                "{ auto enabled = [] { "
                + body
                + " }; "
                + f'unsetenv("{knob}"); assert(enabled()); '
                + f'setenv("{knob}", "0", 1); assert(!enabled()); '
                + f'setenv("{knob}", "1", 1); assert(enabled()); }}'
            )
        self.compile_run(
            "#include <cassert>\n#include <cstdlib>\n#include <cstring>\nint main() {"
            + "\n".join(checks)
            + "}"
        )

    def test_opt_out_and_production_wiring(self):
        source = (NATIVE / "d3d12_descriptor.cpp").read_text()
        for knob in (
            "NXBOX_D3D12_DESCRIPTOR_RANGES",
            "NXBOX_D3D12_PERSISTENT_NULL_DESCRIPTORS",
            "NXBOX_D3D12_BATCH_DESCRIPTOR_COPIES",
            "NXBOX_D3D12_HANDLE_TABLE_CACHE",
        ):
            self.assertIn(f'std::getenv("{knob}")', source)
        self.assertIn("sources.starts.data(), sources.sizes.data()", source)
        self.assertIn("destination_ranges.sizes.data()", source)
        self.assertIn("if (descriptor_ranges)", source)
        self.assertIn("if (persistent_null_descriptors)", source)


if __name__ == "__main__":
    unittest.main()
