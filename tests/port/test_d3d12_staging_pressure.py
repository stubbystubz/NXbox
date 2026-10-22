# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production staging allocator against a std-only simulated queue."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
using u64 = std::uint64_t;
using u8 = std::uint8_t;
bool emergency = true;
bool NativeAllocationPressureWait(u64 bytes) { assert(bytes == 0); return emergency; }
bool BisectEnabled(const char*) { return false; }
constexpr u64 Align(u64 size, u64 alignment) { return (size + alignment - 1) & ~(alignment - 1); }
struct Resource {
    std::vector<u8> bytes;
    std::span<u8> mapped;
    explicit Resource(u64 size) : bytes(size), mapped(bytes) {}
};
using ResourcePtr = std::shared_ptr<Resource>;
struct StagingBufferRef { ResourcePtr buffer; size_t offset; std::span<u8> mapped_span; };
enum class FrameCount { ForcedSubmitStagingPressure, StagingRequests, StagingFastRequests, StagingCollections, StagingNewResources, UniformSnapshotRequests, UniformSnapshotNewResources };
enum class FramePhase { UploadAllocation, StagingCollect };
struct FrameProfile {
    struct Scope { Scope(FrameProfile&, FramePhase) {} };
    int requests{}, fast{}, collections{};
    void Count(FrameCount counter) {
        if (counter == FrameCount::StagingRequests) ++requests;
        if (counter == FrameCount::StagingFastRequests) ++fast;
        if (counter == FrameCount::StagingCollections) ++collections;
    }
};
constexpr int D3D12_HEAP_TYPE_DEFAULT = 1;
struct Context {
    u64 tick = 1, completed = 0;
    int finishes = 0, fence_polls = 0, resources = 0;
    FrameProfile profile;
    FrameProfile& Profile() { return profile; }
    std::vector<ResourcePtr> retained;
    bool IsFree(u64 value) const { return value <= completed; }
    u64 CompletedTick() { ++fence_polls; return completed; }
    u64 CurrentTick() const { return tick; }
    void Collect() { ++fence_polls; }
    void Finish(FrameCount = FrameCount::ForcedSubmitStagingPressure) { ++finishes; completed = tick++; retained.clear(); }
    ResourcePtr CreateBuffer(u64 size, int heap) {
        ++resources;
        auto resource = std::make_shared<Resource>(size);
        if (heap == D3D12_HEAP_TYPE_DEFAULT) resource->mapped = {};
        return resource;
    }
};
struct StagingRing {
    struct Page { ResourcePtr resource; u64 cursor; u64 tick; u64 capacity; };
    Context& context;
    int heap = 0;
    std::vector<Page> pages;
    std::function<void()> release_leases;
    void Collect(bool trim = false);
    StagingBufferRef Request(size_t size);
};
__IMPLEMENTATION__
int main() {
    constexpr u64 mib = 1ULL << 20;
    Context context;
    StagingRing ring{context};
    std::vector<std::weak_ptr<Resource>> old;
    for (int i = 0; i < 16; ++i) {
        auto lease = ring.Request(4 * mib);
        assert(lease.offset == 0);
        old.push_back(lease.buffer);
        context.retained.push_back(lease.buffer);
    }
    assert(context.finishes == 0 && ring.pages.size() == 16);
    auto next = ring.Request(4 * mib);
    assert(context.finishes == 1 && ring.pages.size() == 16);
    assert(next.offset == 0 && next.buffer == old[0].lock());
    // A larger size class evicts completed smaller pages, preserving a caller's lease.
    next.mapped_span[0] = 42;
    auto large = ring.Request(8 * mib);
    assert(context.finishes == 2 && large.mapped_span.size() == 8 * mib);
    assert(next.mapped_span[0] == 42 && large.buffer != next.buffer);
    assert(ring.pages.size() == 2);
    // A completed page cannot reset its cursor until all CPU leases are released.
    context.Finish();
    ring.Collect();
    assert(ring.pages[0].cursor == 4 * mib);
    next = {};
    ring.Collect();
    assert(ring.pages[0].cursor == 0);
    // Appending small uploads amortizes fence queries, including a still-live lease.
    const int polls = context.fence_polls;
    auto small = ring.Request(256);
    small.mapped_span[0] = 99;
    const auto first_offset = small.offset;
    for (int i = 0; i < 1000; ++i) {
        auto sub = ring.Request(256);
        assert(sub.buffer == small.buffer && sub.offset > first_offset);
        assert((sub.offset & 511) == 0);
    }
    assert(small.mapped_span[0] == 99);
    assert(context.fence_polls == polls);
    assert(context.profile.fast >= 1001);
    // DEFAULT snapshots suballocate even though they have no CPU mapping. A cache
    // lease pins all old offsets across fence retirement; another page is used.
    Context gpu_context;
    StagingRing snapshots{gpu_context, D3D12_HEAP_TYPE_DEFAULT};
    auto cached = snapshots.Request(256);
    assert(cached.mapped_span.empty() && cached.offset == 0);
    for (int i = 1; i < 8192; ++i) {
        auto sub = snapshots.Request(256);
        assert(sub.buffer == cached.buffer && sub.offset == size_t(i) * 512);
    }
    assert(gpu_context.resources == 1);
    gpu_context.Finish();
    auto following = snapshots.Request(256);
    assert(following.buffer != cached.buffer && gpu_context.resources == 2);
    cached = {};
    snapshots.Collect();
    auto reused = snapshots.Request(256);
    assert(reused.offset == 0 && reused.buffer != following.buffer);
    assert(gpu_context.resources == 2);
    // Old hot cache entries cannot force a permanent dedicated-allocation fallback.
    Context pressure_context;
    StagingRing pressure{pressure_context, D3D12_HEAP_TYPE_DEFAULT};
    std::vector<ResourcePtr> cache;
    pressure.release_leases = [&] { cache.clear(); };
    for (int i = 0; i < 16; ++i) {
        auto sub = pressure.Request(4 * mib);
        cache.push_back(sub.buffer);
        pressure_context.retained.push_back(sub.buffer);
    }
    const int sweep_polls = pressure_context.fence_polls;
    pressure.Collect();
    assert(pressure_context.fence_polls == sweep_polls + 2); // Not one poll per page.
    auto recycled = pressure.Request(256);
    assert(cache.empty() && pressure_context.finishes == 1);
    assert(pressure_context.resources == 16 && recycled.offset == 0);
    // Host pressure drops only completed, unleased pages, including oversized idle pages.
    Context trim_context;
    StagingRing trimming{trim_context};
    auto pinned = trimming.Request(8 * mib);
    auto idle = trimming.Request(16 * mib);
    std::weak_ptr<Resource> idle_lifetime = idle.buffer;
    idle = {};
    trimming.Collect(true);
    assert(trimming.pages.size() == 2); // Still in flight.
    trim_context.Finish();
    trimming.Collect();
    assert(trimming.pages.size() == 2); // Keep reusable pages without pressure.
    trimming.Collect(true);
    assert(trimming.pages.size() == 1 && idle_lifetime.expired());
    assert(trimming.pages.front().resource == pinned.buffer);
    pinned = {};
    trimming.Collect(true);
    assert(trimming.pages.empty());
    auto after_trim = trimming.Request(256);
    assert(after_trim.offset == 0 && after_trim.mapped_span.size() == 256);
    // Soft pressure retains recent pages without relaxing GPU/CPU ownership.
    emergency = false;
    Context history_context;
    StagingRing history{history_context};
    auto hot = history.Request(4 * mib);
    auto hot_lifetime = std::weak_ptr<Resource>{hot.buffer};
    hot = {};
    history_context.Finish();
    history.Collect(true);
    assert(history.pages.size() == 1 && history_context.resources == 1);
    hot = history.Request(4 * mib);
    assert(hot.buffer == hot_lifetime.lock() && history_context.resources == 1);
    const auto last_use = history.pages.front().tick;
    history_context.tick = last_use + 15;
    history_context.completed = last_use;
    history.Collect(true);
    assert(history.pages.size() == 1);
    history_context.tick = last_use + 16;
    history.Collect(true);
    assert(history.pages.size() == 1); // CPU lease survives age threshold.
    hot = {};
    history_context.completed = last_use - 1;
    history.Collect(true);
    assert(history.pages.size() == 1); // An old page can still be busy.
    history_context.completed = last_use;
    history.Collect(true);
    assert(history.pages.empty() && hot_lifetime.expired());
    // Freshly reused pages must survive another sweep; real exhaustion overrides age.
    hot = history.Request(4 * mib);
    hot_lifetime = hot.buffer;
    hot = {};
    history_context.Finish();
    history.Collect(true);
    assert(history.pages.size() == 1);
    emergency = true;
    history.Collect(true);
    assert(history.pages.empty() && hot_lifetime.expired());
    // Repeated critical sweeps: identical transfer work creates one hot page,
    // versus one new page per frame under immediate eligible-page trimming.
    for (bool reclaim_all : {false, true}) {
        emergency = reclaim_all;
        Context steady_context;
        StagingRing steady{steady_context};
        for (int frame = 0; frame < 32; ++frame) {
            {
                auto transfer = steady.Request(4 * mib);
                assert(transfer.offset == 0 && transfer.mapped_span.size() == 4 * mib);
                transfer.mapped_span[0] = static_cast<u8>(frame);
            }
            steady_context.Finish();
            steady.Collect(true);
        }
        assert(steady_context.resources == (reclaim_all ? 32 : 1));
    }
    bool rejected = false;
    try { ring.Request(0); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    // Images larger than the ring get a temporary lease without permanently growing it.
    const auto pages = ring.pages.size();
    auto oversized = ring.Request(65 * mib);
    assert(oversized.mapped_span.size() == 65 * mib && ring.pages.size() == pages);
}
"""


class D3D12StagingPressureTests(unittest.TestCase):
    def test_pressure_submits_before_reusing_completed_pages(self):
        context = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        implementation = context.split("void StagingRing::Collect(bool trim)", 1)[1].split(
            "} // namespace D3D12", 1
        )[0]
        source_text = DRIVER.replace(
            "__IMPLEMENTATION__", "void StagingRing::Collect(bool trim)" + implementation
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-staging-pressure-") as folder:
            source = Path(folder) / "staging.cpp"
            binary = Path(folder) / "staging"
            source.write_text(source_text)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-missing-field-initializers",
                    "-fsanitize=address,undefined",
                    str(source),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)
