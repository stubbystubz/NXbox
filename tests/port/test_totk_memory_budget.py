# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared-memory admission, production refresh fallback and allocation lifetime regressions."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class TotkMemoryBudgetTests(unittest.TestCase):
    def driver(self, source, disabled=False):
        with tempfile.TemporaryDirectory(prefix="nxbox-totk-budget-") as folder:
            cpp = Path(folder) / "test.cpp"
            binary = Path(folder) / "test"
            cpp.write_text(source)
            build = subprocess.run(
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
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            env = os.environ.copy()
            for name in (
                "NXBOX_MEMORY_BREAKDOWN",
                "NXBOX_ASTC_MEMORY_BUDGET",
                "NXBOX_TEXTURE_MEMORY_PRESSURE",
            ):
                if disabled:
                    env[name] = "0"
                else:
                    env.pop(name, None)
            subprocess.run([str(binary), str(int(disabled))], check=True, timeout=20, env=env)

    def test_decode_pressure_overflow_and_recovery(self):
        self.driver(r"""
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
int main() {
    using namespace D3D12;
    constexpr std::uint64_t mib = 1ULL << 20;
    assert(DecodeMemoryBudget(5120*mib, 3300*mib, 0) == 128*mib);
    assert(DecodeMemoryBudget(5120*mib, 3300*mib, 1) == 32*mib);
    assert(DecodeMemoryBudget(5120*mib, 3300*mib, 2) == 0);
    assert(DecodeMemoryBudget(5120*mib, 4500*mib, 0) == 27*mib);
    assert(DecodeMemoryBudget(5120*mib, 4800*mib, 0) == 0);
    assert(DecodeMemoryBudget(5120*mib, UINT64_MAX, 0) == 0);
    assert(DecodeMemoryBudget(0, 0, 0) == 0);
    std::uint64_t queued = 0;
    unsigned admitted = 0;
    while (AdmitDecode(128*mib, queued, 4*mib, 32*mib)) {
        queued += 36*mib;
        ++admitted;
    }
    assert(admitted == 3 && queued == 108*mib);
    assert(!AdmitDecode(32*mib, queued, 4*mib, 32*mib));
    assert(!AdmitDecode(128*mib, 0, 4*mib, 256*mib)); // Oversized goes synchronous.
    assert(!AdmitDecode(128*mib, UINT64_MAX, 1, 1));
    assert(!AdmitDecode(UINT64_MAX, 0, UINT64_MAX, 1));
    queued -= 36*mib; // Retirement of completed/cancelled work restores admission.
    assert(AdmitDecode(128*mib, queued, 4*mib, 32*mib));
    assert(!AdmitDecode(0, 0, 1, 1));
    auto normal = TextureMemoryThresholds(5120*mib, 4900*mib, 512*mib);
    auto pressure = TextureMemoryThresholds(5120*mib, 4900*mib, 512*mib, 2);
    assert(normal.critical == 448*mib && pressure.critical == 220*mib);
    assert(pressure.expected == 156*mib);
}
""")

    def test_switches_and_pool_gauge_lifetimes(self):
        source = r"""
#define NXBOX_UWP 1
#include <cassert>
#include <utility>
#include "common/nxbox_memory.h"
#include "shader_recompiler/object_pool.h"
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
int main(int, char** argv) {
    using namespace NxboxMemory;
    const bool enabled = argv[1][0] == '0';
    assert(Enabled() == enabled);
    assert(D3D12::AstcMemoryBudgetEnabled() == enabled);
    assert(D3D12::TextureMemoryPressureEnabled() == enabled);
    {
        Gauge a{Kind::DecodeReserved, 100};
        Gauge b{std::move(a)};
        b.Add(50);
        a.Set(25);
        b = std::move(a);
        assert(Read(Kind::DecodeReserved) == (enabled ? 25 : 0));
        Shader::ObjectPool<int> pool{2};
        assert(Read(Kind::ShaderIr) == (enabled ? 2*sizeof(int) : 0));
        (void)pool.Create(1); (void)pool.Create(2); (void)pool.Create(3);
        assert(Read(Kind::ShaderIr) == (enabled ? 4*sizeof(int) : 0));
        pool.ReleaseContents();
        assert(Read(Kind::ShaderIr) == (enabled ? 4*sizeof(int) : 0));
    }
    assert(Read(Kind::DecodeReserved) == 0 && Read(Kind::ShaderIr) == 0);
}
"""
        self.driver(source)
        self.driver(source, disabled=True)

    def test_production_refresh_uses_sync_fallback_without_dropping_upload(self):
        text = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        refresh = text.split("void TextureCache<P>::RefreshContents(", 1)[1]
        branch = refresh[
            refresh.index(
                "    if (True(image.flags & ImageFlagBits::AsynchronousDecode))"
            ) : refresh.index("    const bool gpu_unswizzle_enabled")
        ]
        self.driver(
            r"""
#define NXBOX_UWP 1
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
using u64 = std::uint64_t;
constexpr u64 mib = 1ULL << 20;
enum ImageFlagBits { AsynchronousDecode = 1, IsDecoding = 2 };
bool True(int flags) { return flags != 0; }
struct Image { int flags = AsynchronousDecode; u64 unswizzled_size_bytes = 4*mib; };
struct Runtime {
    u64 budget = 128*mib;
    bool AllowAsyncDecode(u64 queued, u64 input, u64 output) {
        return D3D12::AdmitDecode(budget, queued, input, output);
    }
};
struct OldRuntime {};
struct Decode { unsigned image_id; bool cancelled = false; };
template<class R> struct Cache {
    R runtime;
    u64 async_decode_bytes = 0;
    unsigned queued = 0, uploaded = 0;
    std::vector<std::unique_ptr<Decode>> async_decodes;
    u64 MapSizeBytes(const Image&) { return 32*mib; }
    void QueueAsyncDecode(Image&, unsigned) { ++queued; async_decode_bytes += 36*mib; }
    void Refresh(Image& image, unsigned image_id) {
"""
            + branch
            + r"""
        ++uploaded; // Existing synchronous UploadImageContents follows the tested branch.
    }
};
int main() {
    Cache<Runtime> cache;
    Image image;
    for (unsigned i=0; i<4; ++i) cache.Refresh(image, i);
    assert(cache.queued == 3 && cache.uploaded == 1);
    cache.runtime.budget = 0; // OS pressure level 2.
    image.flags |= IsDecoding;
    cache.async_decodes.push_back(std::make_unique<Decode>(Decode{4}));
    cache.async_decodes.push_back(std::make_unique<Decode>(Decode{99}));
    cache.Refresh(image, 4);
    assert(cache.async_decodes[0]->cancelled && !cache.async_decodes[1]->cancelled);
    assert(!(image.flags & IsDecoding));
    assert(cache.queued == 3 && cache.uploaded == 2);
    cache.async_decode_bytes -= 36*mib;
    cache.runtime.budget = 128*mib;
    cache.Refresh(image, 5);
    assert(cache.queued == 4 && cache.uploaded == 2);
    Cache<OldRuntime> old_backend;
    old_backend.Refresh(image, 0);
    assert(old_backend.queued == 1 && old_backend.uploaded == 0);
}
"""
        )

    def test_production_gc_downloads_dirty_images_and_protects_live_decodes(self):
        text = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        body = text.split("void TextureCache<P>::RunGarbageCollector() {", 1)[1].split(
            "\ntemplate <class P>", 1
        )[0]
        self.driver(
            r"""
#include <cassert>
#include <cstdint>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
using u64 = std::uint64_t;
using ImageId = unsigned;
constexpr u64 mib = 1ULL << 20;
enum ImageFlagBits { IsDecoding=1, BadOverlap=2, CostlyLoad=4, Tracked=8 };
bool True(int x) { return x != 0; }
bool False(int x) { return !True(x); }
struct Image {
    int flags = 0;
    bool dirty = false, deleted = false, downloaded = false;
    u64 unswizzled_size_bytes = 1, gpu_addr = 0, scale_tick = 0;
    int info = 0;
    template<class T> void DownloadMemory(T&, int) { downloaded = true; }
};
int FullDownloadCopies(int) { return 0; }
int FixSmallVectorADL(int x) { return x; }
void SwizzleImage(int, u64, int, int, int, int) {}
struct Map { int mapped_span = 0; };
struct Runtime {
    unsigned evictions = 0;
    void RecordMemoryEviction() { ++evictions; }
    Map DownloadStagingBuffer(u64) { return {}; }
    void Finish() {}
};
struct Lru {
    bool removed[4]{};
    template<class F> void ForEachItemBelow(u64 tick, F f) {
        for (unsigned i=0; i<4; ++i)
            if (!removed[i] && (i == 3 ? 100ULL : 0ULL) < tick && f(i)) break;
    }
};
template<class P> struct TextureCache {
    Runtime runtime;
    Lru lru_cache;
    std::vector<Image> slot_images = std::vector<Image>(4);
    int memory = 0, *gpu_memory = &memory, swizzle_data_buffer = 0;
    u64 frame_tick = 100, total_used_memory = 400*mib;
    u64 expected_memory, critical_memory;
    bool IsDownloadable(const Image& image) { return image.dirty; }
    void UntrackImage(Image&, ImageId) {}
    void UnregisterImage(ImageId id) { lru_cache.removed[id] = true; }
    void DeleteImage(ImageId id, bool) {
        auto& image = slot_images[id];
        assert(!image.dirty || image.downloaded);
        image.deleted = true;
        total_used_memory -= 100*mib;
    }
    void RunGarbageCollector();
};
template<class P> void TextureCache<P>::RunGarbageCollector() {
"""
            + body
            + r"""
int main() {
    TextureCache<int> cache;
    const auto budget = D3D12::TextureMemoryThresholds(5120*mib, 5000*mib, 400*mib, 2);
    cache.expected_memory = budget.expected;
    cache.critical_memory = budget.critical;
    cache.slot_images[0].flags = IsDecoding;
    cache.slot_images[1].dirty = true;
    cache.slot_images[2].flags = CostlyLoad;
    cache.RunGarbageCollector();
    assert(!cache.slot_images[0].deleted);
    assert(cache.slot_images[1].deleted && cache.slot_images[1].downloaded);
    assert(cache.slot_images[2].deleted);
    assert(!cache.slot_images[3].deleted && cache.runtime.evictions == 2);
}
"""
        )

    def test_native_decode_adapter_uses_os_pressure_and_opt_out(self):
        text = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        adapter = text.split("u64 NativeDecodeMemoryBudget() noexcept {", 1)[1].split(
            "\nnamespace {", 1
        )[0]
        source = (
            r"""
#define NXBOX_UWP 1
#include <cassert>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
using u64 = std::uint64_t;
namespace winrt::Windows::System {
struct MemoryManager {
    static inline unsigned pressure = 0;
    static inline bool fail = false;
    static u64 AppMemoryUsageLimit() {
        if (fail) throw std::runtime_error("unavailable");
        return 5120ULL << 20;
    }
    static u64 AppMemoryUsage() { return 3300ULL << 20; }
    static unsigned AppMemoryUsageLevel() { return pressure; }
};
}
namespace D3D12 {
u64 NativeDecodeMemoryBudget() noexcept {
"""
            + adapter
            + r"""
}
int main(int, char** argv) {
    using winrt::Windows::System::MemoryManager;
    if (argv[1][0] == '1') {
        MemoryManager::fail = true;
        assert(D3D12::NativeDecodeMemoryBudget() == UINT64_MAX);
        return 0;
    }
    assert(D3D12::NativeDecodeMemoryBudget() == (128ULL << 20));
    MemoryManager::pressure = 1;
    assert(D3D12::NativeDecodeMemoryBudget() == (32ULL << 20));
    MemoryManager::pressure = 2;
    assert(D3D12::NativeDecodeMemoryBudget() == 0);
    MemoryManager::fail = true;
    assert(D3D12::NativeDecodeMemoryBudget() == (32ULL << 20));
}
"""
        )
        self.driver(source)
        self.driver(source, disabled=True)

    def test_live_guest_and_sparse_scans_exclude_decommitted_and_freed_ranges(self):
        host = (ROOT / "src/common/host_memory.cpp").read_text()
        host_query = host.split("HostBackingUsage QueryHostBackingUsage() {", 1)[1].split(
            "std::uint64_t HostMemoryCommittedBytes()", 1
        )[0]
        sparse = (ROOT / "src/common/sparse_memory.cpp").read_text()
        sparse_query = sparse.split("std::uint64_t CommittedBytes(const void* base", 1)[1].split(
            "void* Allocate(std::size_t size)", 1
        )[0]
        self.driver(
            r"""
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
struct MEMORY_BASIC_INFORMATION { void* BaseAddress; std::size_t RegionSize; unsigned State; };
constexpr unsigned MEM_COMMIT = 1, MEM_RESERVE = 2;
constexpr std::uintptr_t begin = 0x100000000ULL;
constexpr std::size_t gib = 1ULL << 30;
std::array<unsigned, 4> states{MEM_RESERVE, MEM_COMMIT, MEM_COMMIT, MEM_RESERVE};
unsigned queries = 0;
std::size_t VirtualQuery(const void* pointer, MEMORY_BASIC_INFORMATION* out, std::size_t size) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto index = (address - begin) / gib;
    assert(index < states.size());
    ++queries;
    *out = {reinterpret_cast<void*>(begin + index*gib), gib, states[index]};
    return size;
}
namespace Common {
std::atomic<unsigned char*> g_backing_base{reinterpret_cast<unsigned char*>(begin)};
std::atomic<std::size_t> g_backing_size{4*gib};
struct HostBackingUsage { std::uint64_t reserved, committed; };
HostBackingUsage QueryHostBackingUsage() {
"""
            + host_query
            + r"""
}
namespace Common::SparseMemory {
auto query_pages = VirtualQuery;
std::mutex allocation_mutex;
struct Range { std::atomic<void*> base{}; std::atomic<std::size_t> size{}; };
std::array<Range, 2> ranges;
std::uint64_t CommittedBytes(const void* base
"""
            + sparse_query
            + r"""
}
int main() {
    auto usage = Common::QueryHostBackingUsage();
    assert(usage.reserved == 4*gib && usage.committed == 2*gib && queries == 4);
    states[1] = MEM_RESERVE; // A zero-clear decommitted a previously faulted chunk.
    usage = Common::QueryHostBackingUsage();
    assert(usage.committed == gib);
    using namespace Common::SparseMemory;
    ranges[0].base = reinterpret_cast<void*>(begin + 2*gib);
    ranges[0].size = gib;
    assert(LiveCommittedBytes() == gib);
    ranges[0].base = nullptr; // Freed reservations must not remain in cumulative accounting.
    assert(LiveCommittedBytes() == 0);
}
"""
        )


if __name__ == "__main__":
    unittest.main()
