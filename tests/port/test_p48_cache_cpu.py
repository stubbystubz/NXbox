# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute p48 production reductions against unconditional host oracles."""

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def definition(source, name):
    start = source.index(name)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class CacheCpuTests(unittest.TestCase):
    def run_cpp(self, source, modes=({},)):
        with tempfile.TemporaryDirectory(prefix="p48-cache-") as directory:
            cpp = Path(directory) / "check.cpp"
            binary = Path(directory) / "check"
            cpp.write_text(source)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O2",
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
            self.assertEqual(result.returncode, 0, result.stderr)
            for mode in modes:
                environment = dict(os.environ)
                for name in ("NXBOX_D3D12_USAGE_REUSE", "NXBOX_D3D12_DESCRIPTOR_READ"):
                    environment.pop(name, None)
                environment.update(mode)
                environment["UBSAN_OPTIONS"] = "halt_on_error=1"
                subprocess.run([str(binary)], check=True, timeout=60, env=environment)

    def test_usage_reuse_matches_unconditional_bits_and_retention(self):
        native = ROOT / "src/video_core/renderer_d3d12"
        mark = definition(
            (native / "d3d12_buffer_cache.cpp").read_text(), "void Buffer::MarkUsage("
        )
        reset = definition(
            (native / "d3d12_buffer_cache.h").read_text(), "void ResetUsageTracking()"
        )
        enabled = (
            definition((native / "d3d12_buffer_cache.h").read_text(), "bool usage_reuse{") + ";"
        )
        usage = (ROOT / "src/video_core/buffer_cache/usage_tracker.h").read_text()
        usage = re.sub(r"^#(?:include[^\n]*|pragma once)\n", "", usage, flags=re.MULTILINE)
        # Avoid the tracker's existing zero-tail shift at page boundaries. Inspect
        # its actual words, without calling IsUsed's zero-tail path.
        usage = usage.replace("private:", "public:")
        self.run_cpp(
            r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <iostream>
#include <random>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_usage_cache.h"
using u64 = std::uint64_t;
namespace Common {
template <class T, class U> T AlignDown(T value, U alignment) {
    return value & ~(T(alignment) - 1);
}
}
"""
            + usage
            + r"""
struct Context {
    u64 tick = 1, retains{};
    u64 CurrentTick() const { return tick; }
    void Retain(bool) { ++retains; if (retains % 31 == 0) ++tick; }
};
struct Buffer {
    Context* context{};
    bool resource = true;
    struct Usage : VideoCommon::UsageTracker {
        Usage() : UsageTracker(65536) {}
        unsigned tracks{};
        void Track(u64 offset, u64 size) {
            ++tracks;
            UsageTracker::Track(offset, size);
        }
    } usage;
    u64 tick{}, usage_offset{}, usage_size{};
    bool usage_cached{}, binding_cache{};
    __ENABLED__
    D3D12::UsageCache usage_ranges;
    void MarkUsage(u64, u64);
""".replace("__ENABLED__", enabled)
            + reset
            + r"""
};
"""
            + mark
            + r"""
int main() {
    Context ca, cb;
    Buffer fast, slow;
    fast.context = &ca;
    slow.context = &cb;
    slow.usage_reuse = false;
    std::mt19937 rng(48);
    for (unsigned draw = 0; draw < 3000; ++draw) {
        if (draw % 400 == 0) { fast.ResetUsageTracking(); slow.ResetUsageTracking(); }
        // Alternating ranges defeat the previous one-entry opt-in memo.
        for (u64 offset : {64, 2048, 8192, 16384}) {
            fast.MarkUsage(offset, 1024);
            slow.MarkUsage(offset, 1024);
        }
        if (draw % 23 == 0) {
            const u64 offset = (rng() % 8) * 4096 + 64;
            const u64 size = (1 + rng() % 16) * 64;
            fast.MarkUsage(offset, size);
            slow.MarkUsage(offset, size);
        }
        assert(fast.tick == slow.tick && ca.retains == cb.retains);
        assert(fast.usage.pages == slow.usage.pages);
    }
    if (fast.usage_reuse) assert(fast.usage.tracks * 10 < slow.usage.tracks);
    else assert(fast.usage.tracks == slow.usage.tracks);
    auto retains = ca.retains;
    fast.resource = false;
    fast.MarkUsage(64, 1024);
    assert(ca.retains == retains);
    std::cout << "usage tracks=" << slow.usage.tracks << "->" << fast.usage.tracks
              << "; usage bits/ticks/retention identical\n";
}
""",
            modes=({}, {"NXBOX_D3D12_USAGE_REUSE": "0"}),
        )

    def test_invalidation_union_order_and_opt_out(self):
        write = "template <class P>\n" + definition(
            (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text(),
            "void TextureCache<P>::WriteMemory(",
        )
        self.run_cpp(
            r"""
#include <array>
#include <cassert>
#include <iostream>
#include <random>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_invalidation_ranges.h"
using Range = std::pair<std::uint64_t, std::size_t>;
using DAddr = std::uint64_t;
using ImageId = unsigned;
using u32 = unsigned;
enum ImageFlagBits { CpuModified = 1, Tracked = 2, GpuModified = 4 };
bool True(unsigned value) { return value != 0; }
struct Image {
    DAddr cpu_addr{}, size{}, gpu_addr{};
    unsigned flags{}, writes{}, untracks{};
    bool operator==(const Image&) const = default;
};
namespace PassDump {
struct Config { bool enabled = false; unsigned frame = 0; };
Config GetConfig() { return {}; }
}
namespace fmt { template <class... T> int format(T...) { return 0; } }
struct Runtime { void RecordTextureWrite(Image& image) { ++image.writes; } };
template <class P> struct TextureCache {
    std::array<Image, 16> images{};
    P runtime;
    unsigned CaptureFrame() { return 0; }
    void ReportPassDump(int) {}
    void UntrackImage(Image& image, ImageId) {
        image.flags &= ~Tracked;
        ++image.untracks;
    }
    template <class F> void ForEachImageInRegion(DAddr a, std::size_t s, F&& f) {
        if (!s) return;
        for (unsigned i = 0; i < images.size(); ++i)
            if (a < images[i].cpu_addr + images[i].size && images[i].cpu_addr < a + s)
                f(i, images[i]);
    }
    void WriteMemory(DAddr, std::size_t);
};
"""
            + write
            + r"""
int main() {
    std::mt19937 rng(48);
    std::mt19937 image_rng(49);
    unsigned old_calls = 0, new_calls = 0;
    for (unsigned trial = 0; trial < 5000; ++trial) {
        std::vector<Range> input;
        std::array<bool, 2048> expected{}, actual{};
        auto address = rng() % 512;
        for (unsigned i = 0; i < 32; ++i) {
            const auto size = rng() % 128;
            if (i % 5 == 0) address = rng() % 1024;
            input.emplace_back(address, size);
            for (auto j = address; j < address + size; ++j) expected[j] = true;
            address += rng() % 32;
        }
        TextureCache<Runtime> original;
        for (auto& image : original.images)
            image = {image_rng() % 2048, 1 + image_rng() % 512, 0, image_rng() % 8, 0, 0};
        auto fast = original;
        for (const auto& [a, s] : input) original.WriteMemory(a, s);
        D3D12::ForEachInvalidationRange(input, true, [&](auto a, auto s) { fast.WriteMemory(a, s); });
        assert(fast.images == original.images);
        std::vector<Range> unchanged;
        D3D12::ForEachInvalidationRange(input, false, [&](auto a, auto s) {
            unchanged.emplace_back(a, s);
        });
        assert(unchanged == input);
        old_calls += input.size();
        D3D12::ForEachInvalidationRange(input, true, [&](auto a, auto s) {
            ++new_calls;
            for (auto j = a; j < a + s; ++j) actual[j] = true;
        });
        assert(actual == expected);
    }
    assert(new_calls * 2 < old_calls);
    std::vector<Range> edge{{~std::uint64_t{0} - 3, 16}, {0, 0}, {64, 32}, {96, 32}, {32, 32}};
    std::vector<Range> result;
    D3D12::ForEachInvalidationRange(edge, true, [&](auto a, auto s) { result.emplace_back(a, s); });
    assert((result == std::vector<Range>{{edge[0]}, {0, 0}, {32, 96}}));
    std::cout << "invalidation calls=" << old_calls << "->" << new_calls << "; exact union\n";
}
"""
        )
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        inner = definition(source, "void RasterizerD3D12::InnerInvalidation(")
        self.assertLess(
            inner.index("query_runtime.FlushPendingReports"),
            inner.index("ForEachInvalidationRange"),
        )
        self.assertEqual(inner.count("ForEachInvalidationRange"), 1)

    def test_descriptor_small_reads_match_general_walk_and_detect_writes(self):
        memory = (ROOT / "src/video_core/memory_manager.cpp").read_text()
        operation = (
            "template <typename FuncMapped, typename FuncReserved, typename FuncUnmapped>\n"
            + definition(memory, "inline void MemoryManager::MemoryOperation(")
        )
        methods = "\n".join(
            definition(memory, name)
            for name in (
                "void MemoryManager::ReadBlockImpl(",
                "void MemoryManager::ReadBlockUnsafe(",
                "void MemoryManager::ReadBlockUnsafeSmall(",
            )
        )
        hook = "template <typename T>\n" + definition(
            (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.h").read_text(),
            "auto ReadDescriptor(",
        )
        table = (ROOT / "src/video_core/texture_cache/descriptor_table.h").read_text()
        table = re.sub(r"^#(?:include[^\n]*|pragma once)\n", "", table, flags=re.MULTILINE)
        self.run_cpp(
            r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <iostream>
#include <memory>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using GPUVAddr = u64;
using DAddr = u64;
#define DEBUG_ASSERT(x) assert(x)
namespace Common {
template <class T, class U> auto DivCeil(T a, U b) { return (a + b - 1) / b; }
}
namespace Core { constexpr u64 DEVICE_PAGESIZE = 4096, DEVICE_PAGEMASK = 4095; }
namespace VideoCommon { enum class CacheType { None }; }
namespace Tegra {
struct MemoryManager {
    enum class EntryType { Free, Reserved, Mapped };
    static constexpr u64 page_bits = 12, page_size = 4096, page_mask = 4095;
    static constexpr u64 big_page_bits = 16, big_page_size = 65536, big_page_mask = 65535;
    static constexpr u64 cpu_page_bits = 12, address_space_size = 4 * 65536;
    std::array<EntryType, 4> big{};
    std::array<EntryType, 64> small{};
    std::array<u32, 4> big_page_table_dev{16, 32, 48, 64};
    std::array<u32, 64> page_table{};
    std::array<bool, 4> continuous{};
    mutable unsigned drains{}, walks{};
    struct DeviceMemory {
        std::array<u8, 512 * 4096> bytes{};
        std::array<u32, 512> mappings{};
        mutable unsigned slow_reads{};
        u32 unmapped = 512;
        template <class T> T* GetPointer(u64 address) const {
            if (address / 4096 == unmapped) return nullptr;
            return reinterpret_cast<T*>(const_cast<u8*>(bytes.data()) +
                mappings[address / 4096] * 4096 + address % 4096);
        }
        void ReadBlockUnsafe(u64 address, void* out, size_t size) const {
            ++slow_reads;
            auto* dest = static_cast<u8*>(out);
            while (size) {
                auto n = std::min(size, size_t(4096 - address % 4096));
                if (const auto* pointer = GetPointer<u8>(address)) std::memcpy(dest, pointer, n);
                else std::memset(dest, 0, n);
                dest += n; address += n; size -= n;
            }
        }
    } memory;
    struct Rasterizer { void FlushRegion(u64, size_t, VideoCommon::CacheType) { assert(false); } } rast;
    Rasterizer* rasterizer = &rast;
    EntryType GetEntry(u64 a, bool large) const { return large ? big[a >> 16] : small[a >> 12]; }
    bool IsBigPageContinuous(size_t i) const { return continuous[i]; }
    void DrainGpuPipeline() const { ++drains; }
    template <class A, class B, class C> void MemoryOperation(u64, size_t, bool, A&&, B&&, C&&) const;
    void ReadBlockImpl(u64, void*, size_t, VideoCommon::CacheType, bool) const;
    void ReadBlockUnsafe(u64, void*, size_t) const;
    void ReadBlockUnsafeSmall(u64, void*, size_t) const;
};
"""
            + operation
            + methods
            + r"""
}
"""
            + table
            + "struct Runtime {"
            + hook
            + "};"
            + r"""
int main() {
    auto allocated = std::make_unique<Tegra::MemoryManager>();
    auto& m = *allocated;
    std::mt19937 rng(48);
    for (auto& b : m.memory.bytes) b = rng();
    for (u32 i = 0; i < m.memory.mappings.size(); ++i) m.memory.mappings[i] = i;
    for (u32 i = 0; i < m.page_table.size(); ++i) m.page_table[i] = 128 + i * 3;
    unsigned fast = 0;
    for (unsigned trial = 0; trial < 24000; ++trial) {
        if (trial % 37 == 0) {
            for (auto& e : m.big) e = static_cast<Tegra::MemoryManager::EntryType>(rng() % 3);
            for (auto& e : m.small) e = static_cast<Tegra::MemoryManager::EntryType>(rng() % 3);
            for (auto& e : m.continuous) e = rng() % 2;
            // Exercise remapping device pages as well as GPU pages.
            for (u32 i = 0; i < m.memory.mappings.size(); ++i)
                m.memory.mappings[i] = i ^ 1;
            for (unsigned i = 0; i < m.continuous.size(); ++i)
                if (m.continuous[i])
                    for (unsigned p = m.big_page_table_dev[i]; p < m.big_page_table_dev[i] + 16; ++p)
                        m.memory.mappings[p] = p;
        }
        const auto size = rng() % 97;
        const u64 address = trial % 2 ? rng() % (m.address_space_size - 128)
                                     : (1 + rng() % 62) * 4096 - rng() % 40;
        std::array<u8, 128> expected{}, actual{};
        auto before = m.drains;
        m.ReadBlockUnsafe(address, expected.data(), size);
        auto after_old = m.drains;
        m.ReadBlockUnsafeSmall(address, actual.data(), size);
        assert(m.drains > after_old && after_old > before);
        fast += m.drains == after_old + 1;
        assert(actual == expected);
    }
    // A mapped GPU big page may contain an unmapped device page. Preserve the
    // general non-contiguous reader's zero fill instead of dereferencing null.
    m.big[0] = Tegra::MemoryManager::EntryType::Mapped;
    m.continuous[0] = false;
    m.memory.unmapped = m.big_page_table_dev[0];
    std::array<u8, 32> zeros{}, actual{};
    m.ReadBlockUnsafeSmall(32, actual.data(), actual.size());
    assert(actual == zeros);
    m.memory.unmapped = 512;
    // All four TIC/TSC tables keep content comparison and invalidation semantics.
    using Entry = std::array<u64, 4>;
    Runtime runtime;
    std::array<VideoCommon::DescriptorTable<Entry>, 4> tables;
    m.big[0] = Tegra::MemoryManager::EntryType::Mapped;
    for (auto& t : tables) {
        (void)t.Synchronize(32, 127);
        auto old_reads = m.memory.slow_reads;
        auto first = runtime.ReadDescriptor(t, m, 0);
        const bool enabled = !std::getenv("NXBOX_D3D12_DESCRIPTOR_READ");
        assert(m.memory.slow_reads == old_reads + (enabled ? 0 : 1));
        assert(first.second && !runtime.ReadDescriptor(t, m, 0).second);
        *m.memory.GetPointer<u8>(m.big_page_table_dev[0] * 4096 + 32) ^= 1;
        assert(t.Read(m, 0, true).second);
        m.big_page_table_dev[0] ^= 16;
        auto changed = t.Read(m, 0, true);
        auto oracle = t.Read(m, 0, false);
        assert(changed.first == oracle.first && !oracle.second);
        t.Invalidate();
        assert(t.Read(m, 0, true).second);
        (void)t.Synchronize(64, 127);
        assert(t.Read(m, 0, true).second);
    }
    assert(fast > 1000);
    std::cout << "24000 descriptor reads: exact bytes; direct reads=" << fast
              << "; writes/remaps/table invalidations detected\n";
}
""",
            modes=({}, {"NXBOX_D3D12_DESCRIPTOR_READ": "0"}),
        )


if __name__ == "__main__":
    unittest.main()
