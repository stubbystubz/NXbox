# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile production graphics range resolution with host clang and counted dependencies."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, name):
    # Locate the template immediately preceding the requested definition.
    start = source.rfind("template <class P>\n", 0, source.index(name))
    return source[start : source.index("\n}", start) + 2]


class GraphicsBindingCacheTests(unittest.TestCase):
    def test_invalidation_wiring_for_inactive_channels_and_recycled_snapshots(self):
        shared = (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text()
        delete = method(shared, "BufferCache<P>::DeleteBuffer(")
        inactive = delete.split("for (const size_t id : active_channel_ids)", 1)[1]
        self.assertIn(
            "channel_storage[id].storage_buffers, replace", inactive.split("\n    }", 1)[0]
        )
        native = ROOT / "src/video_core/renderer_d3d12"
        header = (native / "d3d12_buffer_cache.h").read_text()
        recycle = header.split("uniform_snapshots{", 1)[1].split("}} {}", 1)[0]
        self.assertIn("uniform_copies.Clear();", recycle)
        self.assertIn("InvalidateUniformBindings();", recycle)
        source = (native / "d3d12_buffer_cache.cpp").read_text()
        frame = source.split("void BufferCacheRuntime::TickFrame(", 1)[1].split("\n}", 1)[0]
        self.assertIn("InvalidateUniformBindings();", frame)
        self.assertIn('binding_cache{BisectEnabled("NXBOX_D3D12_BINDING_CACHE")}', header)

    def test_repeated_usage_preserves_retention_and_frame_reset(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_buffer_cache.cpp").read_text()
        header = (ROOT / "src/video_core/renderer_d3d12/d3d12_buffer_cache.h").read_text()
        begin = source.index("void Buffer::MarkUsage(")
        mark = source[begin : source.index("\n}", begin) + 2]
        begin = header.index("    void ResetUsageTracking()")
        reset = header[begin : header.index("\n    }", begin) + 6]
        driver = r"""
#include "video_core/renderer_d3d12/d3d12_usage_cache.h"
#include <cassert>
#include <cstdint>
using u64 = std::uint64_t;
struct Context {
    unsigned retains{};
    u64 current_tick = 1;
    u64 CurrentTick() const { return current_tick; }
    void Retain(bool) { ++retains; }
};
struct Buffer {
    Context* context{};
    bool resource = true;
    struct Usage {
        unsigned tracks{};
        void Track(u64, u64) { ++tracks; }
        void Reset() { tracks = 0; }
    } usage;
    u64 tick{}, usage_offset{}, usage_size{};
    D3D12::UsageCache usage_ranges;
    bool usage_reuse = false;
    bool usage_cached{}, binding_cache = true;
    void MarkUsage(u64, u64);
__RESET__
};
__MARK__
int main() {
    Context context;
    Buffer buffer;
    buffer.context = &context;
    buffer.MarkUsage(0, 256);
    for (unsigned i = 0; i < 1000; ++i) buffer.MarkUsage(0, 256);
    assert(buffer.usage.tracks == 1 && context.retains == 1001);
    buffer.MarkUsage(64, 256);
    assert(buffer.usage.tracks == 2);
    ++context.current_tick;
    buffer.MarkUsage(64, 256);
    assert(buffer.tick == context.current_tick && context.retains == 1003);
    buffer.ResetUsageTracking();
    buffer.MarkUsage(64, 256);
    assert(buffer.usage.tracks == 1);
    buffer.binding_cache = false;
    buffer.MarkUsage(64, 256);
    assert(buffer.usage.tracks == 2);
    buffer.context = nullptr;
    buffer.MarkUsage(64, 256);
    assert(buffer.usage.tracks == 2);
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-binding-usage-") as folder:
            cpp = Path(folder) / "test.cpp"
            binary = Path(folder) / "test"
            cpp.write_text(driver.replace("__MARK__", mark).replace("__RESET__", reset))
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
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_storage_identity_invalidation_and_fences(self):
        source = (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text()
        driver = r"""
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <compare>
#include <limits>
#define LOG_WARNING(...) ((void)0)
using u32 = std::uint32_t;
using GPUVAddr = std::uint64_t;
using DAddr = std::uint64_t;
__SLOT_ID__
using BufferId = SlotId;
constexpr BufferId NULL_BUFFER_ID{0};
__BINDING__
namespace Settings {
bool accurate = false;
bool IsGPUFenceBehaviorAccurate() { return accurate; }
}
struct FastRuntime {
    bool enabled = true;
    bool BindingResolutionCacheEnabled() const { return enabled; }
};
struct LegacyRuntime {};
struct Maxwell {
    struct {
        struct Stage {
            struct Cbuf { GPUVAddr address{}; };
            std::array<Cbuf, 2> const_buffers{};
        };
        std::array<Stage, 2> shader_stages{};
    } state;
};
struct State {
    u32 total_graphics_storage_buffers{};
    std::array<u32, 2> enabled_storage_buffers{}, written_storage_buffers{};
    std::array<std::array<Binding, 2>, 2> storage_buffers{};
};
template <class Runtime> struct BufferCache {
    Runtime runtime;
    State state;
    State* channel_state = &state;
    Maxwell maxwell;
    Maxwell* maxwell3d = &maxwell;
    struct Slots {
        unsigned buffer{};
        unsigned& operator[](BufferId id) {
            assert(id && id.Value() < 64);
            return buffer;
        }
    } slot_buffers;
    unsigned graphics_segments{}, finds{}, fences{};
    Binding incoming{0x1000, 0x2000, 256, BufferId{}};
    Binding StorageBufferBinding(GPUVAddr, u32, bool) { return incoming; }
    BufferId FindBuffer(DAddr address, u32, bool) {
        ++finds;
        return address ? BufferId{42} : NULL_BUFFER_ID;
    }
    void SynchronizeBufferWrites(unsigned&) { ++fences; }
    void ResolveMultiRangeStorage(Binding& binding, bool, unsigned&) {
        // Model the later host bind, which indexes the slot even without accurate fences.
        (void)slot_buffers[binding.buffer_id];
    }
    template <class F> void ForEachEnabledBit(u32 mask, F f) {
        for (u32 index = 0; mask; ++index, mask >>= 1) if (mask & 1) f(index);
    }
    bool BindGraphicsStorageBuffer(std::size_t, std::size_t, u32, u32, bool);
    void UpdateStorageBuffers(std::size_t);
    BufferId ResolveBufferBinding(const Binding&, DAddr, u32);
};
__METHODS__
template <class R> void Legacy(BufferCache<R>& cache) {
    const auto before = cache.finds;
    for (unsigned i = 0; i < 3; ++i) {
        cache.BindGraphicsStorageBuffer(0, 0, 0, 0, false);
        cache.UpdateStorageBuffers(0);
    }
    assert(cache.finds == before + 3);
}
int main() {
    BufferCache<FastRuntime> cache;
    // Fresh bindings have an invalid SlotId, distinct from the valid null-buffer slot.
    assert(!BufferId{} && BufferId{} != NULL_BUFFER_ID);
    Settings::accurate = true;
    cache.BindGraphicsStorageBuffer(0, 0, 0, 0, false);
    assert(!cache.state.storage_buffers[0][0].buffer_id);
    cache.UpdateStorageBuffers(0);
    assert(cache.finds == 1);
    Settings::accurate = false;
    for (unsigned i = 0; i < 1000; ++i) {
        cache.BindGraphicsStorageBuffer(0, 0, 0, 0, false);
        cache.UpdateStorageBuffers(0);
    }
    assert(cache.finds == 1);
    Settings::accurate = true;
    cache.UpdateStorageBuffers(0);
    assert(cache.finds == 1 && cache.fences == 1);
    cache.incoming.device_addr += 0x1000; // Guest remap must invalidate the resolved id.
    cache.BindGraphicsStorageBuffer(0, 0, 0, 0, false);
    assert(!cache.state.storage_buffers[0][0].buffer_id);
    cache.UpdateStorageBuffers(0);
    assert(cache.finds == 2);
    cache.incoming.size += 16;
    cache.BindGraphicsStorageBuffer(0, 0, 0, 0, true);
    cache.UpdateStorageBuffers(0);
    assert(cache.finds == 3 && cache.state.written_storage_buffers[0] == 1);
    // Use DeleteBuffer's production invalidation, then repeat the same guest binding.
    const BufferId buffer_id{42};
    __SCALAR_REPLACE__
    scalar_replace(cache.state.storage_buffers[0][0]);
    assert(!cache.state.storage_buffers[0][0].buffer_id);
    cache.BindGraphicsStorageBuffer(0, 0, 0, 0, true);
    cache.UpdateStorageBuffers(0);
    assert(cache.finds == 4);
    // Without accurate fences, the later host bind must still receive a resolved id.
    scalar_replace(cache.state.storage_buffers[0][0]);
    Settings::accurate = false;
    cache.UpdateStorageBuffers(0);
    assert(cache.finds == 5);
    cache.state.storage_buffers[0][0] = Binding{.buffer_id = NULL_BUFFER_ID};
    cache.UpdateStorageBuffers(0);
    assert(cache.finds == 5 && cache.state.storage_buffers[0][0].buffer_id == NULL_BUFFER_ID);
    cache.runtime.enabled = false;
    Legacy(cache);
    BufferCache<LegacyRuntime> legacy;
    Legacy(legacy);
}
"""
        implementations = "\n".join(
            method(source, "BufferCache<P>::" + name)
            for name in (
                "BindGraphicsStorageBuffer(",
                "UpdateStorageBuffers(",
                "ResolveBufferBinding(",
            )
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-graphics-bindings-") as folder:
            cpp = Path(folder) / "test.cpp"
            binary = Path(folder) / "test"
            slot_source = (ROOT / "src/common/slot_vector.h").read_text()
            slot_begin = slot_source.index("struct SlotId {")
            slot_id = slot_source[slot_begin : slot_source.index("\n};", slot_begin) + 3]
            base = (ROOT / "src/video_core/buffer_cache/buffer_cache_base.h").read_text()
            binding_begin = base.index("struct Binding {")
            binding = base[binding_begin : base.index("\n};", binding_begin) + 3]
            delete = method(source, "BufferCache<P>::DeleteBuffer(")
            replace_begin = delete.index("const auto scalar_replace =")
            scalar_replace = delete[replace_begin : delete.index("\n    };", replace_begin) + 7]
            cpp.write_text(
                driver.replace("__METHODS__", implementations)
                .replace("__SLOT_ID__", slot_id)
                .replace("__BINDING__", binding)
                .replace("__SCALAR_REPLACE__", scalar_replace)
            )
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
