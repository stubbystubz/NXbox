# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production JIT commitment/accounting and native ASTC opt-in with host API doubles."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
X64 = ROOT / "src/dynarmic/src/dynarmic/backend/x64"


def function(text, signature):
    start = text.index(signature)
    begin = text.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class TotkMemoryP76(unittest.TestCase):
    def run_driver(self, source, env=None):
        with tempfile.TemporaryDirectory(prefix="nxbox-p76-") as folder:
            cpp = Path(folder) / "test.cpp"
            binary = Path(folder) / "test"
            cpp.write_text(source)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-Wconversion",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    "-I/opt/homebrew/opt/boost/include",
                    "-isystem",
                    str(ROOT / "src/dynarmic/src"),
                    "-isystem",
                    "/opt/homebrew/opt/fmt/include",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            clean_env = os.environ.copy()
            clean_env.pop("NXBOX_MEMORY_BREAKDOWN", None)
            clean_env.pop("NXBOX_JIT_EAGER_COMMIT", None)
            clean_env.update(env or {})
            subprocess.run([str(binary)], check=True, env=clean_env, timeout=30)

    def test_tracked_metadata_capacity_nested_move_clear_and_release(self):
        self.run_driver(r"""
#define DYNARMIC_UWP_APPCONTAINER 1
#include <cassert>
#include <vector>
#include <limits>
#include "common/nxbox_memory.h"
#include "common/container/unordered_map.h"
int main() {
    using namespace NxboxMemory;
    using Sites = std::vector<int, JitAllocator<int>>;
    using Map = Common::unordered_map<int, Sites, std::hash<int>, std::equal_to<int>,
                                     JitAllocator<std::pair<const int, Sites>>>;
    {
        Map patches;
        patches.reserve(500);
        auto buckets = Read(Kind::JitMetadata);
        assert(buckets > 0);
        patches[7].reserve(100);
        assert(Read(Kind::JitMetadata) == buckets + 100*sizeof(int));
        Map moved = std::move(patches);
        assert(Read(Kind::JitMetadata) == buckets + 100*sizeof(int));
        moved.clear(); // Values release nested storage; reusable buckets remain charged.
        assert(Read(Kind::JitMetadata) == buckets);
        auto before_failure = Read(Kind::JitMetadata);
        try {
            auto* impossible = JitAllocator<int>{}.allocate(std::numeric_limits<size_t>::max());
            (void)impossible;
            assert(false);
        } catch (const std::bad_alloc&) {}
        assert(Read(Kind::JitMetadata) == before_failure);
    }
    assert(Read(Kind::JitMetadata) == 0);
    {
        Gauge arena{Kind::JitCode, 32};
        Gauge dispatch{Kind::JitDispatch, 16};
        Gauge stacks{Kind::FiberStacks, 64};
        Gauge payload{Kind::DescriptorPayload, 8};
        Gauge moved{std::move(stacks)};
        arena.Set(48);
        assert(Read(Kind::JitCode) == 48 && Read(Kind::FiberStacks) == 64);
    }
    for (auto kind : {Kind::JitCode, Kind::JitDispatch, Kind::FiberStacks, Kind::DescriptorPayload})
        assert(Read(kind) == 0);
}
""")

    def test_production_dualmap_reserve_commit_failure_cleanup_and_accounting(self):
        text = (X64 / "block_of_code.cpp").read_text()
        methods = "\n".join(
            function(text, name)
            for name in (
                "uint8_t* TryAllocDualMap(size_t size)",
                "[[maybe_unused]] void* DualMapExecView(",
                "bool FreeDualMap(uint8_t* rw)",
                "bool BlockOfCode::ConfigureWx()",
                "void BlockOfCode::EnsureMemoryCommitted(",
            )
        )
        driver = r"""
#define _WIN32 1
#define DYNARMIC_UWP_APPCONTAINER 1
#define XBYAK_NXBOX_EXEC_DELTA 1
#include <algorithm>
#include <cassert>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "common/nxbox_memory.h"
using u8 = uint8_t;
using ULONG64 = uint64_t;
using HMODULE = void*;
using HANDLE = void*;
constexpr unsigned PAGE_EXECUTE_READWRITE=1, SEC_RESERVE=2, FILE_MAP_WRITE=4,
 FILE_MAP_EXECUTE=8, FILE_MAP_READ=16, PAGE_EXECUTE_READ=32, MEM_COMMIT=64, PAGE_READWRITE=128;
#define INVALID_HANDLE_VALUE nullptr
#define LOG_INFO(...) ((void)0)
#define UNREACHABLE_MSG(...) throw std::runtime_error("commit")
constexpr size_t MiB = 1024*1024, PRELUDE_COMMIT_SIZE = 16*MiB;
std::unique_ptr<uint8_t[]> storage;
int failure=0, closes=0, unmaps=0, commit_calls=0;
unsigned protection=0;
void* CreateFileMappingFromApp(void*, void*, unsigned flags, ULONG64 size, void*) {
    protection=flags;
    if (failure==1) return nullptr;
    storage.reset(new uint8_t[static_cast<size_t>(size)]);
    return storage.get();
}
void* MapViewOfFileFromApp(void*, unsigned flags, int, size_t) {
    if ((flags==FILE_MAP_WRITE && failure==2) || (flags!=FILE_MAP_WRITE && failure==4))
        return nullptr;
    return storage.get();
}
void CloseHandle(void*) { ++closes; }
void UnmapViewOfFile(void*) { ++unmaps; }
void* GetModuleHandleW(const wchar_t*) { return nullptr; }
using MapViewOfFile3FromAppFn = void*(*)(void*,void*,void*,int,int,int,unsigned,void*,int);
MapViewOfFile3FromAppFn GetProcAddress(void*, const char*) { return nullptr; }
void* GetCurrentProcess() { return nullptr; }
void* Commit(void* ptr, size_t, unsigned, unsigned) {
    ++commit_calls;
    return failure==3 ? nullptr : ptr;
}
#define DYNARMIC_VIRTUAL_ALLOC Commit
bool DualMapRequested() { return true; }
std::string DualMapError(const char* reason) { return reason; }
struct DualMapViews { HANDLE section; void* rx; size_t committed; };
std::mutex dualmap_mutex;
std::unordered_map<const uint8_t*, DualMapViews> dualmap_views;
thread_local std::string dualmap_reason;
size_t JitPageSize() { return 4096; }
class BlockOfCode {
public:
    size_t size_=0, maxSize_=192*MiB, committed_size=0;
    uint8_t* top_=nullptr;
    NxboxMemory::Gauge code_memory{NxboxMemory::Kind::JitCode};
    NxboxMemory::Gauge reserved_memory{NxboxMemory::Kind::JitReserved};
    void setExecDelta(ptrdiff_t) {}
    bool ConfigureWx();
    void EnsureMemoryCommitted(size_t codesize);
};
"""
        driver += (
            methods
            + r"""
int main() {
    using namespace NxboxMemory;
    for (int step=1; step<=4; ++step) {
        failure=step; closes=unmaps=0;
        assert(TryAllocDualMap(192*MiB)==nullptr);
        assert(closes==(step==1 ? 0 : 1));
        assert(unmaps==(step>=3 ? 1 : 0));
        assert(dualmap_views.empty());
    }
    failure=0;
    {
        BlockOfCode code;
        code.top_=TryAllocDualMap(code.maxSize_);
        assert(code.top_ && (protection & SEC_RESERVE));
        assert(code.ConfigureWx());
        assert(Read(Kind::JitReserved)==192*MiB && Read(Kind::JitCode)==16*MiB);
        int before=commit_calls;
        code.EnsureMemoryCommitted(2*MiB);
        assert(commit_calls==before);
        code.size_=16*MiB-4096;
        code.EnsureMemoryCommitted(8192);
        assert(Read(Kind::JitCode)==32*MiB);
        failure=3;
        code.size_=32*MiB;
        try { code.EnsureMemoryCommitted(1); assert(false); }
        catch (const std::runtime_error&) {}
        assert(Read(Kind::JitCode)==32*MiB);
        failure=0;
        code.size_=190*MiB;
        code.EnsureMemoryCommitted(2*MiB);
        assert(Read(Kind::JitCode)==192*MiB);
        assert(FreeDualMap(code.top_));
        assert(!FreeDualMap(code.top_));
    }
    assert(Read(Kind::JitCode)==0 && Read(Kind::JitReserved)==0);
    // No dual map: the same production growth/accounting applies to the W^X reservation.
    {
        BlockOfCode code;
        code.top_=storage.get();
        code.EnsureMemoryCommitted(4096);
        assert(Read(Kind::JitCode)==16*MiB);
    }
    assert(Read(Kind::JitCode)==0);
}
"""
        )
        self.run_driver(driver)
        self.run_driver(
            driver[: driver.index("int main()")]
            + r"""
int main() {
    using namespace NxboxMemory;
    {
        BlockOfCode code;
        code.top_=TryAllocDualMap(code.maxSize_);
        assert(code.top_ && !(protection & SEC_RESERVE));
        assert(commit_calls==0);
        assert(code.ConfigureWx());
        assert(Read(Kind::JitCode)==192*MiB);
        code.EnsureMemoryCommitted(2*MiB);
        assert(commit_calls==0);
        assert(FreeDualMap(code.top_));
    }
    assert(Read(Kind::JitCode)==0);
}
""",
            {"NXBOX_JIT_EAGER_COMMIT": "1"},
        )

    def test_exact_native_only_lossy_opt_in_and_eager_baseline(self):
        driver = r"""
#include <cassert>
#include <cstdlib>
#include "common/nxbox_memory.h"
int main() {
    using namespace NxboxMemory;
    for (const char* value : std::initializer_list<const char*>{nullptr, "", "0", "true", "11", "1"}) {
        assert(!NativeAstcToBc(false, value));
        assert(NativeAstcToBc(true, value)==(value && std::string_view{value}=="1"));
    }
    bool eager=std::getenv("NXBOX_JIT_EAGER_COMMIT")!=nullptr;
    assert(JitEagerCommit()==eager);
    setenv("NXBOX_JIT_EAGER_COMMIT", eager ? "0" : "1", 1);
    assert(JitEagerCommit()==eager); // Latched, independent from gauges.
}
"""
        self.run_driver(driver)
        self.run_driver(driver, {"NXBOX_JIT_EAGER_COMMIT": "1", "NXBOX_MEMORY_BREAKDOWN": "0"})

    def test_production_astc_output_size_mips_layers_and_block_padding(self):
        util = (ROOT / "src/video_core/texture_cache/util.cpp").read_text()
        size_function = function(util, "u32 CalculateConvertedSizeBytes(")
        self.run_driver(
            r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
using u32 = uint32_t;
using s32 = int32_t;
struct Extent2D { u32 width, height; };
struct Extent3D { u32 width, height, depth; };
enum class ImageType { Buffer, e2D };
struct ImageInfo {
 ImageType type=ImageType::e2D;
 Extent3D size{8,8,1};
 struct { s32 levels=4; u32 layers=2; } resources;
 int format=1;
};
namespace Common { u32 AlignUp(u32 value, u32 align) { return (value+align-1)/align*align; } }
namespace Settings {
 enum class AstcRecompression { Uncompressed, Bc1, Bc3 };
 struct Setting { AstcRecompression current=AstcRecompression::Uncompressed;
  AstcRecompression GetValue() const { return current; } };
 struct { Setting astc_recompression; } values;
}
bool IsPixelFormatASTC(int format) { return format==1; }
u32 BytesPerBlock(int) { return 16; }
u32 ConvertedBytesPerBlock(int) { return 4; }
Extent3D AdjustMipSize(Extent3D size, s32 level) {
 return {std::max(1u,size.width>>level), std::max(1u,size.height>>level), 1};
}
u32 NumBlocksPerLayer(const ImageInfo&, Extent2D) { return 64+16+4+1; }
"""
            + size_function
            + r"""
int main() {
 ImageInfo image;
 using Settings::AstcRecompression;
 assert(CalculateConvertedSizeBytes(image)==680); // RGBA mips, two layers.
 Settings::values.astc_recompression.current=AstcRecompression::Bc3;
 assert(CalculateConvertedSizeBytes(image)==224); // (64+16+16+16)*2, padded BC blocks.
 image.resources.levels=1;
 assert(CalculateConvertedSizeBytes(image)==128); // 4:1 for full 4x4 blocks.
 image.size={5,3,1};
 assert(CalculateConvertedSizeBytes(image)==64); // Two 16-byte blocks per layer.
 Settings::values.astc_recompression.current=AstcRecompression::Bc1;
 assert(CalculateConvertedSizeBytes(image)==32); // Preserve the existing BC1 option.
}
"""
        )

    def test_production_fiber_and_descriptor_payload_lifetimes(self):
        fiber = (ROOT / "src/common/fiber.cpp").read_text()
        stacks = fiber[
            fiber.index("    u32 canary_1") : fiber.index("    boost::context::detail::fcontext_t")
        ]
        header = (ROOT / "src/video_core/renderer_d3d12/d3d12_descriptor.h").read_text()
        descriptor = (ROOT / "src/video_core/renderer_d3d12/d3d12_descriptor.cpp").read_text()
        ring = function(header, "struct Ring {") + ";"
        cache = function(header, "struct DescriptorCache {") + ";"
        init = function(descriptor, "void D3D12DescriptorAllocator::Init(")
        init_cache = function(descriptor, "void D3D12DescriptorAllocator::InitCache(")
        self.run_driver(
            r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <unordered_map>
#include "common/nxbox_memory.h"
using u8=uint8_t;
using u32=uint32_t;
using u64=uint64_t;
constexpr size_t DEFAULT_STACK_SIZE=512*1024;
constexpr u32 CANARY_VALUE=0;
struct Stacks {
"""
            + stacks
            + r"""
};
enum D3D12_DESCRIPTOR_HEAP_TYPE { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV };
constexpr unsigned D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE=1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE=0;
struct D3D12_DESCRIPTOR_HEAP_DESC { D3D12_DESCRIPTOR_HEAP_TYPE Type; u32 NumDescriptors; unsigned Flags; };
struct D3D12_CPU_DESCRIPTOR_HANDLE {};
struct D3D12_GPU_DESCRIPTOR_HANDLE {};
struct ID3D12DescriptorHeap {
 D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart() { return {}; }
 D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStart() { return {}; }
};
template <class T> struct ComPtr { T data; T* operator->() { return &data; } };
struct RingAllocator { explicit RingAllocator(u32) {} };
struct FakeDevice {
 int CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_DESC*, ComPtr<ID3D12DescriptorHeap>*) { return 0; }
 u32 GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) { return 32; }
};
struct Context { FakeDevice device; FakeDevice* Device() { return &device; } };
#define IID_PPV_ARGS(ptr) ptr
void Check(int, const char*, FakeDevice*) {}
void Diagnostic(const std::string&) {}
using DescriptorKey=int;
using CachedDescriptor=int;
using DescriptorKeyHash=std::hash<int>;
struct D3D12DescriptorAllocator {
"""
            + ring
            + cache
            + r"""
 Context context;
 void Init(Ring&, D3D12_DESCRIPTOR_HEAP_TYPE, u32, bool);
 void InitCache(DescriptorCache&, D3D12_DESCRIPTOR_HEAP_TYPE, u32);
};
"""
            + init
            + init_cache
            + r"""
int main() {
 using namespace NxboxMemory;
 {
  auto stacks = std::make_unique<Stacks>();
  assert(Read(Kind::FiberStacks)==1024*1024);
 }
 assert(Read(Kind::FiberStacks)==0);
 {
  D3D12DescriptorAllocator allocator;
  D3D12DescriptorAllocator::Ring ring{64};
  D3D12DescriptorAllocator::DescriptorCache cache;
  allocator.Init(ring, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, true);
  allocator.InitCache(cache, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16);
  assert(Read(Kind::DescriptorPayload)==80*32);
 }
 assert(Read(Kind::DescriptorPayload)==0);
}
"""
        )

    def test_production_range_index_counts_nodes_and_nested_sets(self):
        self.run_driver(r"""
#define DYNARMIC_UWP_APPCONTAINER 1
#include <cassert>
#include "dynarmic/backend/block_range_information.cpp"
int main() {
 using namespace NxboxMemory;
 using namespace Dynarmic;
 {
  Backend::BlockRangeInformation<u64> index;
  index.AddRange(boost::icl::discrete_interval<u64>::right_open(0x1000,0x2000), IR::LocationDescriptor{7});
  auto before=Read(Kind::JitMetadata);
  assert(before>sizeof(index));
  index.AddRange(boost::icl::discrete_interval<u64>::right_open(0x1800,0x2800), IR::LocationDescriptor{8});
  assert(Read(Kind::JitMetadata)>before);
  boost::icl::interval_set<u64> range;
  range.add(boost::icl::discrete_interval<u64>::right_open(0x1900,0x1a00));
  auto invalidated=index.InvalidateRanges(range);
  assert(invalidated.size()==2);
  index.ClearCache();
  assert(Read(Kind::JitMetadata)==0);
 }
 assert(Read(Kind::JitMetadata)==0);
}
""")

    def test_disabled_diagnostics_leave_allocator_and_policy_operational(self):
        self.run_driver(
            r"""
#define DYNARMIC_UWP_APPCONTAINER 1
#include <cassert>
#include <vector>
#include "common/nxbox_memory.h"
int main() {
 using namespace NxboxMemory;
 assert(!Enabled());
 assert(!JitEagerCommit());
 {
  std::vector<int, JitAllocator<int>> data(100, 7);
  Gauge code{Kind::JitCode, 1024};
  assert(data.back()==7);
  assert(Read(Kind::JitMetadata)==0 && Read(Kind::JitCode)==0);
 }
 assert(Read(Kind::JitMetadata)==0);
}
""",
            {"NXBOX_MEMORY_BREAKDOWN": "0"},
        )

    def test_frontend_setting_precedes_renderer_and_shared_bc3_paths(self):
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        opt_in = session.index('std::getenv("NXBOX_ASTC_TO_BC")')
        self.assertGreater(opt_in, session.index("// Apply after global and per-title settings"))
        self.assertLess(opt_in, session.index("system.Load("))
        native = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        self.assertIn('host_format = LookupFormat("BC3_UNORM")', native)
        self.assertIn('"BC3_SRGB"', native)
        util = (ROOT / "src/video_core/texture_cache/util.cpp").read_text()
        self.assertIn("Tegra::Texture::BCN::CompressBC3", util)
        self.assertIn("Settings::values.astc_recompression.GetValue()", util)


if __name__ == "__main__":
    unittest.main()
