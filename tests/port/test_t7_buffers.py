# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute T7 heap placement and pressure policy without Windows or game data."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"

DRIVER = r"""
#include <cassert>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;
using SIZE_T = size_t;
using D3D12_HEAP_TYPE = int;
using D3D12_RESOURCE_STATES = int;
constexpr int D3D12_HEAP_TYPE_DEFAULT=0, D3D12_HEAP_TYPE_UPLOAD=1, D3D12_HEAP_TYPE_READBACK=2;
constexpr u64 D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT=65536;
constexpr int D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS=1;
#define IID_PPV_ARGS(x) x
namespace D3D12 {
template<class T> struct ComPtr {
    std::shared_ptr<T> value;
    T* Get() const { return value.get(); }
    T* operator->() const { return value.get(); }
    explicit operator bool() const { return bool(value); }
};
struct ID3D12Heap {};
struct FakeResource {
    std::vector<u8> bytes;
    int Map(int, const void*, void** out) { *out=bytes.data(); return 0; }
};
struct D3D12_RESOURCE_DESC { u64 Width; };
struct D3D12_RANGE { SIZE_T Begin, End; };
struct D3D12_HEAP_DESC {
    u64 SizeInBytes{}, Alignment{};
    struct { int Type{}; int CreationNodeMask{}, VisibleNodeMask{}; } Properties;
    int Flags{};
};
struct ID3D12Device {
    int heaps=0;
    u64 last_offset=0;
    struct Allocation { u64 SizeInBytes; };
    Allocation GetResourceAllocationInfo(int, int, const D3D12_RESOURCE_DESC* desc) {
        return {(desc->Width+65535)&~u64{65535}};
    }
    int CreateHeap(const D3D12_HEAP_DESC*, ComPtr<ID3D12Heap>* out) {
        ++heaps; out->value=std::make_shared<ID3D12Heap>(); return 0;
    }
    int CreatePlacedResource(ID3D12Heap*, u64 offset, const D3D12_RESOURCE_DESC* desc,
                             int, void*, ComPtr<FakeResource>* out) {
        last_offset=offset;
        out->value=std::make_shared<FakeResource>(); out->value->bytes.resize(desc->Width);
        return 0;
    }
};
void Check(int result, const char*, ID3D12Device*) { if(result) throw std::runtime_error("fake failure"); }
struct MemoryBudget { std::atomic<u64> used{}; };
struct Resource {
    ComPtr<FakeResource> handle;
    int heap{}, state{};
    u64 allocation_size{};
    std::span<u8> mapped;
};
using ResourcePtr=std::shared_ptr<Resource>;
__POOL__
}
int main() {
    using namespace D3D12;
    constexpr u64 mib=1ULL<<20;
    // Observed Xbox limit is 5 GiB, not the Series X retail game partition.
    assert(BufferMemoryTarget(5120*mib, 3200*mib, 512*mib)==2176*mib);
    assert(BufferMemoryTarget(5120*mib, 5100*mib, 512*mib)==512*mib);
    assert(BufferMemoryTarget(5120*mib, 6000*mib, 512*mib)==512*mib);
    assert(BufferMemoryTarget(16*mib, 16*mib, 0)==0);
    assert(BufferMemoryPressure(512*mib,2560*mib,512*mib)==2560*mib);
    assert(BufferMemoryPressure(512*mib,2560*mib,3000*mib)==512*mib);
    assert(BufferMemoryPressure(UINT64_MAX,2560*mib,0)==UINT64_MAX);
    BufferHeapSlots slots(8);
    assert(!slots.Allocate(0) && !slots.Allocate(9));
    assert(slots.Allocate(3)==0 && slots.Allocate(2)==3 && slots.Allocate(3)==5);
    assert(!slots.Allocate(1));
    slots.Release(3,2); assert(!slots.Allocate(3));
    slots.Release(0,3); assert(slots.Allocate(5)==0);
    slots.Release(0,5); slots.Release(5,3); assert(slots.Used()==0);
    ID3D12Device device;
    auto budget=std::make_shared<MemoryBudget>();
    auto pool=BufferPool(budget);
    auto allocate=[&](u64 size, int type=0) {
        return pool->Allocate(&device,budget,{size},type,0,pool);
    };
    auto first=allocate(256);
    auto second=allocate(256);
    assert(device.heaps==1 && device.last_offset==65536);
    assert(budget->used==16*mib);
    auto fence=first;
    first.reset();
    auto third=allocate(256); assert(device.last_offset==131072);
    // A submitted fence's shared lease prevents reuse, even after cache eviction.
    pool->Trim(); assert(pool->pages.size()==1);
    fence.reset(); auto recycled=allocate(256); assert(device.last_offset==0);
    auto upload=allocate(256,D3D12_HEAP_TYPE_UPLOAD); auto readback=allocate(256,D3D12_HEAP_TYPE_READBACK);
    assert(upload->mapped.size()==256 && readback->mapped.size()==256);
    assert(device.heaps==3 && budget->used==48*mib);
    second.reset(); third.reset(); recycled.reset(); upload.reset(); readback.reset();
    pool->Trim(); assert(pool->pages.empty() && budget->used==0);
    assert(!allocate(17*mib)); // Dedicated committed fallback, no page leak.
    auto next=allocate(256); assert(device.last_offset==0);
    auto same=BufferPool(budget); assert(same==pool);
    auto other=BufferPool(std::make_shared<MemoryBudget>()); assert(other!=pool);
    std::weak_ptr<BufferHeapPool> lifetime=pool;
    same.reset(); pool.reset();
    assert(!lifetime.expired()); // Only the placed resource's deleter keeps the pool alive.
    next.reset();
    assert(lifetime.expired() && budget->used==0);
}
"""


BINDINGS_DRIVER = r"""
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_upload_cache.h"
#include "video_core/renderer_d3d12/d3d12_uniform_stream.h"
using namespace D3D12;
using u64=std::uint64_t; using u32=std::uint32_t; using u8=std::uint8_t;
namespace fmt { template<class... T> std::string format(const char*, T...) { return {}; } }
bool UploadTraceEnabled() { return false; }
bool UniformStreamEnabled() { return true; }
void Diagnostic(const std::string&) {}
namespace Common {
template<class T> T AlignUp(T value, T alignment) { return (value+alignment-1)&~(alignment-1); }
}
namespace VideoCommon { struct BufferCopy { u64 src_offset, dst_offset, size; }; }
struct Resource {
    u64 content_version{};
    mutable unsigned descriptions{};
    std::vector<u8> bytes;
    struct Handle {
        Resource* owner;
        struct Desc { u64 Width; };
        Desc GetDesc() const { return {owner->bytes.size()}; }
        u64 GetGPUVirtualAddress() const { return reinterpret_cast<u64>(owner); }
        Handle* operator->() { return this; }
    } handle{this};
    Handle::Desc Description() const { ++descriptions; return handle.GetDesc(); }
};
using ResourcePtr=std::shared_ptr<Resource>;
struct Buffer { ResourcePtr resource; ResourcePtr Handle() const { return resource; } };
struct StagingRef { ResourcePtr buffer; size_t offset; std::span<u8> mapped_span; };
enum class FramePhase { BufferUpload, UploadAllocation, UniformCopyLookup };
enum class FrameCount { BufferUploadBytes, UniformUploadBytes, UniformCopyCacheHits, UniformCopyCacheMisses };
struct FrameProfile {
    struct Scope { Scope(FrameProfile&, FramePhase) {} };
    u64 upload_bytes{};
    void Count(FrameCount counter, u64 amount=1) {
        if (counter==FrameCount::BufferUploadBytes) upload_bytes += amount;
    }
};
struct Context {
    FrameProfile profile;
    FrameProfile& Profile() { return profile; }
    u64 CurrentTick() const { return 1; }
    ResourcePtr CreateBuffer(size_t size, int) {
        auto r=std::make_shared<Resource>(); r->bytes.resize(size,0xcc); return r;
    }
};
struct Staging {
    Context& context;
    StagingRef Request(size_t size) {
        auto r=context.CreateBuffer(size,0); return {r,0,r->bytes};
    }
};
struct SnapshotStaging {
    Context& context;
    ResourcePtr page;
    size_t cursor{};
    StagingRef Request(size_t size) {
        if (!page) page = context.CreateBuffer(4 * 1024 * 1024, 0);
        auto offset = cursor;
        cursor += (size + 511) & ~size_t{511};
        return {page, offset, {}};
    }
};
struct BoundBuffer { ResourcePtr resource; u32 offset, size; bool written=false; };
struct GeometryBindings {
    static constexpr size_t MaxVertexBuffers=32;
    struct Slot { ResourcePtr resource; u32 offset{}, size{}, stride{}; };
    Slot vertex[32];
};
struct BindingState {
    static constexpr size_t NumStages = 5;
    struct Stage { std::vector<BoundBuffer> uniform, storage; };
    Stage stages[5]; size_t current_stage=0;
    GeometryBindings geometry;
};
static int invalid=0;
void InvalidRange(u32, const char*, u64, u64, u64) { ++invalid; }
struct BufferCacheRuntime {
    bool buffer_fastpath = true;
    bool BufferFastpathEnabled() const { return buffer_fastpath; }
    bool BindingCacheEnabled() const { return binding_cache; }
    bool BindingResolutionCacheEnabled() const { return resolution_cache; }
    bool resolution_cache = false;
    bool TryReuseUniformBinding(const Buffer&, u32, u32, u32);
    bool TryReuseStorageBinding(const Buffer&, u32, u32, bool);
    bool binding_cache = true;
    Context context;
    UniformStream<StagingRef> uniform_stream;
    Staging upload{context};
    SnapshotStaging uniform_snapshots{context, {}, 0};
    BindingState bindings;
    UniformCopyCache<Resource> uniform_copies;
    __RESOLUTION_FIELDS__
    void CopyBuffer(ResourcePtr dst, ResourcePtr src, std::span<const VideoCommon::BufferCopy> copies, bool) {
        ++dst->content_version;
        for (const auto& copy:copies)
            std::memcpy(dst->bytes.data()+copy.dst_offset,src->bytes.data()+copy.src_offset,copy.size);
    }
    void BindVertexBuffer(u32, ResourcePtr, u32, u32, u32);
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32);
    void BindUniformBuffer(const Buffer&, u32, u32);
    u32 uniform_read_limit = ~0U;
    void SetUniformReadLimit(u32 limit) { uniform_read_limit = limit; }
    BoundBuffer CopyUniform(const BoundBuffer&);
    void BindStorageBuffer(const Buffer&, u32, u32, bool);
};
__BINDINGS__
void CheckBindings(bool fastpath, bool binding_cache) {
    invalid = 0;
    BufferCacheRuntime runtime;
    runtime.buffer_fastpath = fastpath;
    runtime.binding_cache = binding_cache;
    // Rebuilt per-draw vectors must still receive entries on a resolution-cache hit.
    runtime.SetUniformReadLimit(256);
    runtime.BindUniformBuffer(Buffer{runtime.context.CreateBuffer(1024, 0)}, 0, 256);
    auto repeated = runtime.bindings.stages[0].uniform.back().resource;
    runtime.bindings.stages[0].uniform.clear();
    const auto descriptions = repeated->descriptions;
    runtime.SetUniformReadLimit(256);
    runtime.BindUniformBuffer(Buffer{repeated}, 0, 256);
    assert(runtime.bindings.stages[0].uniform.size() == 1);
    assert(runtime.bindings.stages[0].uniform.back().resource == repeated);
    assert(!fastpath || repeated->descriptions == descriptions + !binding_cache);
    runtime.bindings.stages[0].uniform.clear();
    ++repeated->content_version;
    runtime.SetUniformReadLimit(256);
    runtime.BindUniformBuffer(Buffer{repeated}, 0, 256);
    assert(!fastpath || repeated->descriptions == descriptions + 1 + !binding_cache);
    runtime.bindings.stages[0].uniform.clear();
    auto memory=runtime.context.CreateBuffer(131072,0);
    for (size_t i=0;i<memory->bytes.size();++i) memory->bytes[i]=u8(i);
    Buffer buffer{memory};
    runtime.BindVertexBuffer(0,memory,7,103,0);
    assert(runtime.bindings.geometry.vertex[0].stride==0);
    assert(runtime.bindings.geometry.vertex[0].size==103);
    const auto vertex_descriptions = memory->descriptions;
    runtime.BindVertexBuffer(0,memory,7,103,0);
    assert(!fastpath || memory->descriptions == vertex_descriptions + !binding_cache);
    runtime.BindVertexBuffer(0,memory,7,103,16);
    assert(runtime.bindings.geometry.vertex[0].stride == 16);
    runtime.BindVertexBuffer(1,memory,7,1000,32);
    assert(runtime.bindings.geometry.vertex[1].offset==7);
    runtime.BindVertexBuffer(1,memory,131071,1000,32);
    assert(!runtime.bindings.geometry.vertex[1].resource && invalid==1);
    auto stream=runtime.BindMappedUniformBuffer(0,0,3);
    assert(stream.size()==3);
    const auto& mapped=runtime.bindings.stages[0].uniform.back();
    assert(mapped.offset%256==0 && mapped.size==256);
    assert(mapped.resource->bytes[mapped.offset + mapped.size - 1]==0);
    stream[0]=42; assert(mapped.resource->bytes[0]==42);
    runtime.BindUniformBuffer(buffer,256,65536);
    assert(runtime.bindings.stages[0].uniform.back().resource==memory);
    runtime.BindUniformBuffer(buffer,4,259);
    const auto& padded=runtime.bindings.stages[0].uniform.back();
    assert(padded.offset==0 && padded.size==512 && padded.resource!=memory);
    for (size_t i=0;i<259;++i) assert(padded.resource->bytes[i]==memory->bytes[i+4]);
    for (size_t i=259;i<512;++i) assert(padded.resource->bytes[i]==0);
    assert(runtime.context.Profile().upload_bytes==256+512);
    auto snapshot=padded.resource;
    runtime.BindUniformBuffer(buffer,4,259);
    assert(runtime.bindings.stages[0].uniform.back().resource==snapshot);
    assert(runtime.context.Profile().upload_bytes==256+512);
    memory->bytes[4]=81; ++memory->content_version;
    runtime.BindUniformBuffer(buffer,4,259);
    auto updated=runtime.bindings.stages[0].uniform.back().resource;
    const auto updated_offset = runtime.bindings.stages[0].uniform.back().offset;
    assert(updated==snapshot && updated_offset==512);
    assert(updated->bytes[updated_offset]==81 && snapshot->bytes[0]==4);
    assert(updated->bytes[updated_offset+259]==0);
    runtime.BindUniformBuffer(buffer,4,259);
    assert(runtime.bindings.stages[0].uniform.back().offset==updated_offset);
    assert(runtime.context.Profile().upload_bytes==256+512+512);
    runtime.BindUniformBuffer(buffer,131071,2);
    assert(!runtime.bindings.stages[0].uniform.back().resource && invalid==2);
    // A shader that reads no further than the binding sees nothing of the rounded tail.
    const auto uploaded = runtime.context.Profile().upload_bytes;
    runtime.SetUniformReadLimit(96);
    runtime.BindUniformBuffer(buffer,512,96);
    const auto& rounded=runtime.bindings.stages[0].uniform.back();
    assert(rounded.resource==memory && rounded.offset==512 && rounded.size==256);
    assert(runtime.context.Profile().upload_bytes==uploaded);
    // The limit is consumed: an unbounded next binding still gets its zero-padded copy.
    runtime.BindUniformBuffer(buffer,512,96);
    assert(runtime.bindings.stages[0].uniform.back().resource!=memory);
    runtime.SetUniformReadLimit(128);
    runtime.BindUniformBuffer(buffer,512,96);
    assert(runtime.bindings.stages[0].uniform.back().resource!=memory);
    runtime.BindStorageBuffer(buffer,16,65536,true);
    const auto& storage=runtime.bindings.stages[0].storage.back();
    assert(storage.size==65536 && storage.offset==16 && storage.written);
    runtime.BindStorageBuffer(buffer,3,16,false);
    assert(!runtime.bindings.stages[0].storage.back().resource && invalid==3);
}
void CheckResolution() {
    BufferCacheRuntime runtime;
    Buffer buffer{runtime.context.CreateBuffer(8192, 0)};
    runtime.BindUniformBuffer(buffer, 4, 259);
    const auto first = runtime.bindings.stages[0].uniform.back();
    const auto descriptions = buffer.resource->descriptions;
    for (unsigned draw = 0; draw < 1000; ++draw) {
        runtime.bindings.stages[0].uniform.clear();
        runtime.BindUniformBuffer(buffer, 4, 259);
        const auto& resolved = runtime.bindings.stages[0].uniform.back();
        assert(resolved.resource == first.resource && resolved.offset == first.offset);
    }
    assert(buffer.resource->descriptions == descriptions);
    ++buffer.resource->content_version;
    runtime.bindings.stages[0].uniform.clear();
    runtime.BindUniformBuffer(buffer, 4, 259);
    assert(runtime.bindings.stages[0].uniform.back().offset != first.offset);
    assert(buffer.resource->descriptions == descriptions + 1);
    // Snapshot page recycle clears both caches before the page can be overwritten.
    runtime.uniform_copies.Clear();
    runtime.InvalidateUniformBindings();
    runtime.bindings.stages[0].uniform.clear();
    runtime.BindUniformBuffer(buffer, 4, 259);
    assert(buffer.resource->descriptions == descriptions + 2);
    // Read bounds are part of the key: a tail that becomes readable must be zero padded.
    runtime.bindings.stages[0].uniform.clear();
    runtime.SetUniformReadLimit(96);
    runtime.BindUniformBuffer(buffer, 512, 96);
    assert(runtime.bindings.stages[0].uniform.back().resource == buffer.resource);
    runtime.bindings.stages[0].uniform.clear();
    runtime.BindUniformBuffer(buffer, 512, 96);
    assert(runtime.bindings.stages[0].uniform.back().resource != buffer.resource);
    runtime.bindings.current_stage = 1;
    runtime.BindUniformBuffer(buffer, 256, 256);
    assert(runtime.bindings.stages[1].uniform.back().offset == 256);
    // Different shader/compute layouts can use more dense slots than the cache supports.
    for (unsigned slot = 0; slot < 24; ++slot) runtime.BindUniformBuffer(buffer, 0, 256);
    assert(runtime.bindings.stages[1].uniform.size() == 25);
    Buffer replacement{runtime.context.CreateBuffer(8192, 0)};
    runtime.bindings.stages[1].uniform.clear();
    runtime.BindUniformBuffer(replacement, 256, 256);
    assert(runtime.bindings.stages[1].uniform.back().resource == replacement.resource);
}
int main() {
    CheckBindings(true, true);
    CheckBindings(false, true);
    CheckBindings(true, false);
    CheckBindings(false, false);
    CheckResolution();
}
"""


class T7BuffersTests(unittest.TestCase):
    def test_heap_recycling_fence_lifetime_and_budget(self):
        context = (BACKEND / "d3d12_context.cpp").read_text()
        pool = context.split("struct BufferHeapPage {", 1)[1].split(
            "} // namespace\nResourcePtr Context::CreateResource", 1
        )[0]
        source_text = DRIVER.replace("__POOL__", "struct BufferHeapPage {" + pool)
        with tempfile.TemporaryDirectory(prefix="nxbox-t7-") as folder:
            source = Path(folder) / "buffers.cpp"
            binary = Path(folder) / "buffers"
            source.write_text('#include "common/maxwell_method_profile.h"\n' + source_text)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_production_binding_ranges_and_uniform_bytes(self):
        source = (BACKEND / "d3d12_buffer_cache.cpp").read_text()
        vertex = source.split("void BufferCacheRuntime::BindVertexBuffer(", 1)[1].split(
            "void BufferCacheRuntime::BindVertexBuffers", 1
        )[0]
        uniforms = source.split("std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer", 1)[
            1
        ].split("void BufferCacheRuntime::BindTextureBuffer", 1)[0]
        implementation = "void BufferCacheRuntime::BindVertexBuffer(" + vertex
        implementation += "std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer" + uniforms
        with tempfile.TemporaryDirectory(prefix="nxbox-t7-bindings-") as folder:
            source_file = Path(folder) / "bindings.cpp"
            binary = Path(folder) / "bindings"
            header = (BACKEND / "d3d12_buffer_cache.h").read_text()
            resolution = header.split("    struct UniformBindingResolution {", 1)[1].split(
                "    UniformCopyCache<Resource> uniform_copies;", 1
            )[0]
            resolution = "    struct UniformBindingResolution {" + resolution
            resolution = resolution.replace("VideoCommon::NUM_GRAPHICS_UNIFORM_BUFFERS", "18")
            resolution = resolution.replace("BindingState::NumStages", "5")
            resolution = resolution.replace("VideoCommon::NUM_STORAGE_BUFFERS", "16")
            source_file.write_text(
                BINDINGS_DRIVER.replace("__BINDINGS__", implementation).replace(
                    "__RESOLUTION_FIELDS__", resolution
                )
            )
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(source_file),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_pressure_is_not_a_hard_allocation_failure(self):
        context = (BACKEND / "d3d12_context.cpp").read_text()
        self.assertNotIn("exceeds 512 MiB budget", context)
        allocation = context.split("ResourcePtr Context::CreateResource", 1)[1]
        self.assertIn("NativeBufferMemoryTarget", allocation)
        self.assertIn("result == E_OUTOFMEMORY", allocation)
        self.assertIn("pool->Trim()", allocation)

    def test_uniform_padding_and_vertex_range_contract(self):
        source = (BACKEND / "d3d12_buffer_cache.cpp").read_text()
        self.assertIn("upload.Request(padded)", source)
        self.assertNotIn("upload.Request(65536)", source)
        self.assertIn("staging.mapped_span.end(), u8{0}", source)
        self.assertIn("size > width - offset", source)
        self.assertIn("offset & 255", source)
        self.assertIn("size != padded", source)
        self.assertIn("{std::move(buffer), offset, size, stride}", source)
        self.assertIn("Snapshot every source range", source)
