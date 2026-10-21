# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production placed allocation and pressure ordering with D3D12 stubs."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp"


class BufferPressureReuseTests(unittest.TestCase):
    def test_reuse_growth_lifetime_retry_and_opt_out(self):
        source = SOURCE.read_text()
        pool = source.split("struct BufferHeapPage {", 1)[1].split(
            "std::shared_ptr<BufferHeapPool> BufferPool(", 1
        )[0]
        heap_kind = source.split("NxboxMemory::Kind HeapMemoryKind(", 1)[1].split(
            "// Each placed resource", 1
        )[0]
        create = source.split("ResourcePtr Context::CreateBuffer(", 1)[1].split(
            "void StagingRing::Collect(", 1
        )[0]
        driver = r"""
#include "common/nxbox_memory.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"
using u8 = unsigned char;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using UINT = unsigned;
using SIZE_T = std::size_t;
using D3D12_HEAP_TYPE = int;
using D3D12_RESOURCE_STATES = int;
constexpr int D3D12_HEAP_TYPE_DEFAULT=0, D3D12_HEAP_TYPE_UPLOAD=1,
    D3D12_HEAP_TYPE_READBACK=2, D3D12_RESOURCE_STATE_COMMON=0,
    D3D12_RESOURCE_STATE_GENERIC_READ=1, D3D12_RESOURCE_STATE_COPY_DEST=2,
    D3D12_RESOURCE_DIMENSION_BUFFER=1, D3D12_TEXTURE_LAYOUT_ROW_MAJOR=1,
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS=1,
    D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT=65536,
    D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS=1, D3D12_RESOURCE_BARRIER_TYPE_ALIASING=1;
struct D3D12_RESOURCE_DESC {
    int Dimension{}, Height{}, DepthOrArraySize{}, MipLevels{}, Layout{}, Flags{};
    u64 Width{};
    struct { int Count{}; } SampleDesc;
};
struct D3D12_HEAP_DESC {
    u64 SizeInBytes{}, Alignment{};
    struct { int Type{}, CreationNodeMask{}, VisibleNodeMask{}; } Properties;
    int Flags{};
};
struct D3D12_RANGE { SIZE_T Begin, End; };
struct Handle {
    u64 offset{};
    std::vector<u8> bytes;
    int Map(int, const D3D12_RANGE*, void** pointer) {
        bytes.resize(16ULL << 20); *pointer=bytes.data(); return 0;
    }
};
template<class T> struct ComPtr {
    std::shared_ptr<T> value;
    T* Get() const { return value.get(); }
    T* operator->() const { return Get(); }
    explicit operator bool() const { return bool(value); }
};
using ID3D12Heap=Handle;
#define IID_PPV_ARGS(pointer) pointer
struct ID3D12Device {
    unsigned heaps{}, placed{};
    bool fail_placed{};
    struct Allocation { u64 SizeInBytes; };
    Allocation GetResourceAllocationInfo(int, int, const D3D12_RESOURCE_DESC* desc) {
        return {(desc->Width+65535)/65536*65536};
    }
    int CreateHeap(const D3D12_HEAP_DESC*, ComPtr<Handle>* output) {
        ++heaps; output->value=std::make_shared<Handle>(); return 0;
    }
    int CreatePlacedResource(Handle*, u64 offset, const D3D12_RESOURCE_DESC*, int,
                             void*, ComPtr<Handle>* output) {
        ++placed;
        if (fail_placed) { fail_placed=false; return -1; }
        output->value=std::make_shared<Handle>(); output->value->offset=offset; return 0;
    }
};
void Check(int result, const char*, ID3D12Device*) {
    if (result) throw std::runtime_error("allocation failed");
}
void Diagnostic(const std::string&) {}
namespace fmt { template<class... T> std::string format(const char*, T&&...) { return {}; } }
u64 Align(u64 size, u64 alignment) { return (size+alignment-1)/alignment*alignment; }
unsigned GetEnvironmentVariableA(const char* name, char* output, unsigned capacity) {
    const auto* value=std::getenv(name);
    if (!value) return 0;
    const auto size=std::strlen(value);
    if (size < capacity) std::memcpy(output,value,size+1);
    return static_cast<unsigned>(size);
}
bool BisectEnabled(const char* name) { return std::getenv(name)!=nullptr; }
namespace D3D12 {
struct MemoryBudget { u64 used{}; };
struct Resource {
    ComPtr<Handle> handle;
    int heap{}, state{};
    u64 allocation_size{};
    std::shared_ptr<MemoryBudget> budget;
    std::span<u8> mapped;
};
using ResourcePtr=std::shared_ptr<Resource>;
__HEAP_KIND__
struct BufferHeapPage {__POOL__
std::shared_ptr<BufferHeapPool> active_pool;
std::shared_ptr<BufferHeapPool> BufferPool(const std::shared_ptr<MemoryBudget>&) {
    return active_pool;
}
u64 NativeBufferMemoryTarget(u64) noexcept { return 1ULL << 30; }
enum class FrameCount { ForcedSubmitMemoryPressure, ForcedSubmitAllocationRetry, Barriers };
enum class FramePhase { Barriers };
struct FrameProfile {
    struct Scope { Scope(FrameProfile&, FramePhase) {} };
    void Count(FrameCount) {}
};
struct D3D12_RESOURCE_BARRIER {
    int Type{};
    struct { Handle* pResourceAfter{}; } Aliasing;
};
struct Context {
    ComPtr<ID3D12Device> device{std::make_shared<ID3D12Device>()};
    std::shared_ptr<MemoryBudget> budget=std::make_shared<MemoryBudget>();
    FrameProfile profile;
    unsigned finishes{}, retries{}, barriers{}, committed{};
    std::vector<ResourcePtr> retained;
    void Collect() {}
    void Finish(FrameCount reason) {
        if (reason==FrameCount::ForcedSubmitMemoryPressure) ++finishes;
        else ++retries;
        retained.clear();
    }
    Context* Commands() { return this; }
    void ResourceBarrier(unsigned count, const D3D12_RESOURCE_BARRIER* barrier) {
        assert(count==1 && barrier->Aliasing.pResourceAfter); ++barriers;
    }
    void Retain(const ResourcePtr& resource) { retained.push_back(resource); }
    ResourcePtr CreateResource(const D3D12_RESOURCE_DESC&, int, int) {
        ++committed; return std::make_shared<Resource>();
    }
    ResourcePtr CreateBuffer(u64 size, D3D12_HEAP_TYPE heap);
};
ResourcePtr Context::CreateBuffer(__CREATE__
}
int main() {
    using namespace D3D12;
    const auto* knob=std::getenv("NXBOX_D3D12_BUFFER_PRESSURE_REUSE");
    const bool fast=!knob || std::string(knob)!="0";
    active_pool=std::make_shared<BufferHeapPool>();
    Context context;
    if (std::getenv("NXBOX_D3D12_NO_PLACED")) {
        context.budget->used=2ULL << 30;
        auto committed=context.CreateBuffer(17,D3D12_HEAP_TYPE_DEFAULT);
        assert(committed && context.committed==1 && context.finishes==1);
        assert(context.device->heaps==0 && context.barriers==0);
        return 0;
    }
    // Start over target with one live allocation and free slots in its heap.
    auto live=context.CreateBuffer(256,D3D12_HEAP_TYPE_DEFAULT);
    context.retained.clear();
    context.budget->used=2ULL << 30;
    const auto heaps=context.device->heaps;
    auto reused=context.CreateBuffer(17,D3D12_HEAP_TYPE_DEFAULT);
    assert(context.finishes==(fast ? 0u : 1u));
    assert(context.device->heaps==heaps && context.budget->used==(2ULL << 30));
    assert(reused->handle.Get()!=live->handle.Get());
    assert(reused->allocation_size==65536 && reused->state==D3D12_RESOURCE_STATE_COMMON);
    assert(context.barriers==2 && context.retained.back()==reused);
    assert(live->handle->offset==0 && reused->handle->offset==65536);
    // Only releasing the last lease makes that range eligible for reuse.
    live.reset();
    const auto before_released=context.finishes;
    auto released=context.CreateBuffer(256,D3D12_HEAP_TYPE_DEFAULT);
    assert(released->handle->offset==0);
    assert(context.finishes==before_released+(fast ? 0u : 1u));
    // A no-growth miss must not allocate a heap or bypass pressure handling.
    const auto before=context.finishes;
    auto large=context.CreateBuffer(16ULL << 20,D3D12_HEAP_TYPE_DEFAULT);
    assert(context.finishes==before+1 && context.device->heaps==heaps+1);
    // Heap types stay isolated; mapped staging buffers retain their requested extent.
    const auto before_upload=context.finishes;
    auto upload=context.CreateBuffer(33,D3D12_HEAP_TYPE_UPLOAD);
    assert(context.finishes==before_upload+1 && upload->mapped.size()==256);
    assert(upload->heap==D3D12_HEAP_TYPE_UPLOAD);
    // A failed placed allocation releases its slot and falls back to committed memory.
    const auto used=active_pool->pages.front()->slots.Used();
    context.device->fail_placed=true;
    auto fallback=context.CreateBuffer(1,D3D12_HEAP_TYPE_DEFAULT);
    assert(fallback && context.retries==1 && context.committed==1);
    assert(active_pool->pages.front()->slots.Used()==used);
    // Oversized buffers also follow the committed fallback.
    auto oversized=context.CreateBuffer(32ULL << 20,D3D12_HEAP_TYPE_DEFAULT);
    assert(oversized && context.committed==2);
}
""".replace("__POOL__", pool).replace("__CREATE__", create)
        with tempfile.TemporaryDirectory(prefix="nxbox-pressure-reuse-") as folder:
            cpp = Path(folder) / "pressure.cpp"
            binary = Path(folder) / "pressure"
            driver = driver.replace(
                "__HEAP_KIND__", "NxboxMemory::Kind HeapMemoryKind(" + heap_kind
            )
            cpp.write_text('#include "common/maxwell_method_profile.h"\n' + driver)
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
            for value, no_placed in (
                (None, False),
                ("0", False),
                ("1", False),
                ("0", True),
                ("1", True),
            ):
                environment = dict(os.environ)
                environment.pop("NXBOX_D3D12_NO_PLACED", None)
                environment.pop("NXBOX_D3D12_BUFFER_PRESSURE_REUSE", None)
                if no_placed:
                    environment["NXBOX_D3D12_NO_PLACED"] = "1"
                if value is not None:
                    environment["NXBOX_D3D12_BUFFER_PRESSURE_REUSE"] = value
                subprocess.run([str(binary)], env=environment, check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
