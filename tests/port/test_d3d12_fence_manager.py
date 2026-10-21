# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the shared fence protocol with std-only fake caches and a GPU queue."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STUBS = r"""
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <vector>
using u32 = std::uint32_t;
namespace Settings {
inline bool IsGPUFenceBehaviorDefault() { return true; }
inline bool high = true;
inline bool IsGPULevelHigh() { return high; }
inline bool IsGPUFenceBehaviorBalanced() { return false; }
inline bool IsGPUFenceBehaviorAccurate() { return true; }
}
namespace Common {
enum class ThreadPriority { High };
inline void SetCurrentThreadName(const char*) {}
inline void SetCurrentThreadPriority(ThreadPriority) {}
inline void SetCurrentThreadToPerformanceCores() {}
}
namespace VideoCommon {
template <typename T, int N> struct DelayedDestructionRing {
    void Tick() {}
    void Push(T) {}
};
}
namespace Tegra {
namespace Host1x {
struct SyncpointManager {
    std::atomic<int> guest{}, host{};
    u32 GetGuestSyncpointValue(u32) const { return guest.load(); }
    void IncrementGuest(u32, std::function<void()> before_actions = {}) {
        ++guest;
        if (before_actions) before_actions();
    }
    void IncrementHost(u32) { ++host; }
};
}
struct Host { Host1x::SyncpointManager sync; auto& GetSyncpointManager() { return sync; } };
struct GPU { Host host; auto& Host1x() { return host; } };
}
namespace VideoCore {
struct RasterizerInterface {
    int submits{}, invalidations{};
    std::function<void()> on_flush;
    void FlushCommands() { ++submits; if (on_flush) on_flush(); }
    void InvalidateGPUCache() { ++invalidations; }
};
}
"""
DRIVER = r"""
#include <cassert>
#include <memory>
#include "video_core/fence_manager.h"
struct Cache {
    std::mutex mutex;
    bool dirty{}, ready{};
    std::atomic<int> commits{}, pops{}, pending{};
    std::function<void()> on_commit;
    void AccumulateFlushes() {}
    bool HasUncommittedFlushes() { return dirty; }
    bool ShouldWaitAsyncFlushes() { return pending && !ready; }
    void CommitAsyncFlushes() {
        ++commits; ++pending;
        if (on_commit) on_commit();
        dirty = false;
    }
    void PopAsyncFlushes() { assert(pending > 0); --pending; ++pops; }
};
struct Fence : VideoCommon::FenceBase {
    explicit Fence(bool stubbed) : FenceBase(stubbed) {}
    bool ready{};
};
template <bool native> struct Params {
    using FenceType = std::shared_ptr<Fence>;
    using BufferCacheType = Cache;
    using TextureCacheType = Cache;
    using QueryCacheType = Cache;
    static constexpr bool HAS_ASYNC_CHECK = false;
    static constexpr bool REQUIRE_GPU_COMPLETION = native;
};
template <bool native> struct Manager : VideoCommon::FenceManager<Params<native>> {
    using Base = VideoCommon::FenceManager<Params<native>>;
    Manager(VideoCore::RasterizerInterface& r, Tegra::GPU& g, Cache& t, Cache& b, Cache& q)
        : Base(r, g, t, b, q) {}
    std::shared_ptr<Fence> latest;
    int waits{};
    std::shared_ptr<Fence> CreateFence(bool stubbed) override {
        latest = std::make_shared<Fence>(stubbed); return latest;
    }
    void QueueFence(std::shared_ptr<Fence>&) override {}
    bool IsFenceSignaled(std::shared_ptr<Fence>& f) const override {
        return f->IsStubbed() || f->ready;
    }
    void WaitFence(std::shared_ptr<Fence>& f) override { ++waits; f->ready = true; }
};
int main() {
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache texture, buffer, query;
    Manager<false> manager(rasterizer, gpu, texture, buffer, query);
    std::vector<int> order;
    bool committing = false;
    // QueryCache::CommitAsyncFlushes schedules unregister while constructing its batch.
    query.on_commit = [&] {
        committing = true;
        manager.SyncOperation([&] {
            assert(!committing);
            assert(query.pops == query.commits);
            order.push_back(2);
        });
        committing = false;
    };
    manager.SyncOperation([&] { assert(query.pops == 1); order.push_back(1); });
    assert(order.empty() && rasterizer.submits == 0);
    query.dirty = true;
    manager.SignalFence([&] { order.push_back(3); });
    assert(order.empty() && query.commits == 1 && query.pops == 0);
    assert(!manager.latest->IsStubbed() && rasterizer.submits == 1);
    manager.WaitPendingFences(true);
    assert((order == std::vector<int>{1, 2, 3}));
    assert(manager.waits == 1 && query.pending == 0);
    // A syncpoint publishes guest progress now, host progress after the fence.
    query.dirty = true;
    manager.SignalSyncPoint(7);
    assert(gpu.host.sync.guest == 1 && gpu.host.sync.host == 0);
    manager.WaitPendingFences(true);
    assert(gpu.host.sync.host == 1 && manager.waits == 2);
    // Empty cache batches still retire and run unregister exactly once.
    manager.SignalReference();
    assert(manager.latest->IsStubbed());
    manager.WaitPendingFences(true);
    assert(query.commits == 3 && query.pops == 3);
    assert(buffer.commits == buffer.pops && texture.commits == texture.pops);
    // Native GPU work requires a real fence even with no cache readbacks and relaxed settings.
    Settings::high = false;
    query.on_commit = {};
    query.ready = buffer.ready = texture.ready = true;
    Manager<true> native(rasterizer, gpu, texture, buffer, query);
    native.SignalSyncPoint(8);
    assert(!native.latest->IsStubbed());
    assert(gpu.host.sync.guest == 2 && gpu.host.sync.host == 1);
    native.SignalOrdering();
    assert(gpu.host.sync.host == 1 && native.waits == 0);
    native.WaitPendingFences(true);
    assert(gpu.host.sync.host == 2 && native.waits == 1);
    native.SignalReference();
    assert(!native.latest->IsStubbed());
    native.WaitPendingFences(true);
    assert(native.waits == 2);
    assert(buffer.commits == buffer.pops && query.commits == query.pops);
}
"""


ASYNC_DRIVER = DRIVER.split("template <bool native> struct Params", 1)[0] + r"""
#include <chrono>
#include <thread>
struct AsyncParams {
    using FenceType = std::shared_ptr<Fence>;
    using BufferCacheType = Cache;
    using TextureCacheType = Cache;
    using QueryCacheType = Cache;
    static constexpr bool HAS_ASYNC_CHECK = true;
    static constexpr bool REQUIRE_GPU_COMPLETION = true;
};
// Fences become submitted only when the "recording thread" says so. The backstop is 5 s,
// so only NotifySubmitted() can retire a fence in time.
struct Manager : VideoCommon::FenceManager<AsyncParams> {
    using Base = VideoCommon::FenceManager<AsyncParams>;
    Manager(VideoCore::RasterizerInterface& r, Tegra::GPU& g, Cache& t, Cache& b, Cache& q)
        : Base(r, g, t, b, q) { submit_backstop = std::chrono::seconds(5); }
    // Match the native manager: join before the derived vtable and members die.
    ~Manager() override { StopFenceThread(); }
    std::atomic<bool> submitted{};
    mutable std::atomic<int> checks{};
    std::shared_ptr<Fence> CreateFence(bool stubbed) override {
        return std::make_shared<Fence>(stubbed);
    }
    void QueueFence(std::shared_ptr<Fence>&) override {}
    bool IsFenceSignaled(std::shared_ptr<Fence>& f) const override {
        return f->IsStubbed() || f->ready;
    }
    void WaitFence(std::shared_ptr<Fence>& f) override {
        assert(submitted); // The GPU timeout must never start for a deferred tick.
        f->ready = true;
    }
    bool IsFenceSubmitted(const std::shared_ptr<Fence>&) const override {
        ++checks;
        return submitted;
    }
    void FlushPendingFences() override { submitted = true; NotifySubmitted(); }
};
bool WaitFor(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!flag && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return flag;
}
int main() {
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache texture, buffer, query;
    texture.ready = buffer.ready = query.ready = true;
    Settings::high = false;
    Manager manager(rasterizer, gpu, texture, buffer, query);
    // The submission races the fencing thread's check and its sleep; none may be missed.
    for (int i = 0; i < 300; ++i) {
        std::atomic<bool> done{};
        manager.submitted = false;
        manager.SignalFence([&] { done = true; });
        std::this_thread::sleep_for(std::chrono::microseconds(i % 8 * 40));
        manager.submitted = true;
        manager.NotifySubmitted();
        assert(WaitFor(done));
    }
    // A forced drain submits through the hook and is released by the same wake-up.
    manager.submitted = false;
    manager.SignalReference();
    const auto start = std::chrono::steady_clock::now();
    manager.WaitPendingFences(true);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    assert(query.commits == query.pops && buffer.commits == buffer.pops);
    // Stopping with an unsubmitted front fence still joins the fencing thread.
    manager.submitted = false;
    manager.SignalReference();
    while (!manager.checks)
        std::this_thread::yield();
}
"""


class D3D12FenceManagerTests(unittest.TestCase):
    def test_cache_commit_callback_and_syncpoint_ordering(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-fences-") as temp:
            temp = Path(temp)
            (temp / "stubs.h").write_text(STUBS)
            for name in (
                "common/common_types.h",
                "common/settings.h",
                "common/thread.h",
                "video_core/delayed_destruction_ring.h",
                "video_core/gpu.h",
                "video_core/host1x/host1x.h",
                "video_core/host1x/syncpoint_manager.h",
                "video_core/rasterizer_interface.h",
            ):
                header = temp / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "stubs.h"\n')
            source = temp / "fences.cpp"
            binary = temp / "fences"
            source.write_text(DRIVER)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-pthread",
                    "-I",
                    str(temp),
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
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_native_syncpoint_retires_without_another_guest_command(self):
        cache = DRIVER.split("struct Cache {", 1)[1].split("struct Fence :", 1)[0]
        native_stubs = (
            STUBS
            + "\nstruct Cache {"
            + cache
            + r"""
using u64 = std::uint64_t;
namespace D3D12 {
enum class FrameCount { ForcedSubmitGuestFence };
using TextureCache = Cache;
using BufferCache = Cache;
using QueryCache = Cache;
struct Context {
    std::atomic<u64> submitted{}, completed{};
    std::atomic<int> waits{};
    mutable std::atomic<int> submission_checks{};
    std::atomic<int> submit_calls{};
    std::atomic<bool> submit_failed{};
    bool submit_throws = false;
    bool fail = false;
    std::mutex guard;
    std::condition_variable cv;
    bool recording = true;
    void RecordWork() { recording = true; }
    u64 LastSubmittedTick() const { return submitted; }
    bool IsTickSubmitted(u64 tick) const {
        ++submission_checks;
        return submitted >= tick || submit_failed;
    }
    u64 PendingFenceTick() const { return recording ? submitted + 1 : submitted.load(); }
    u64 Submit(FrameCount) {
        ++submit_calls;
        if (!recording) return submitted;
        recording = false;
        if (submit_throws) {
            // Like device removal after the list closed: the tick is never signalled.
            submit_failed = true;
            throw std::runtime_error("simulated submit failure");
        }
        return ++submitted;
    }
    bool IsFree(u64 tick) const { return completed >= tick; }
    void WaitSubmitted(u64 tick) {
        assert(submitted >= tick || submit_failed);
        ++waits;
        if (fail || submit_failed) throw std::runtime_error("simulated device failure");
        std::unique_lock lock(guard);
        cv.wait(lock, [&] { return completed >= tick; });
    }
    void ReportFailure(const std::exception&) {}
    void Complete() {
        std::scoped_lock lock(guard);
        completed.store(submitted.load());
        cv.notify_all();
    }
};
inline bool BisectEnabled(const char* name) {
    const auto* value = std::getenv(name);
    return value && std::string(value) == "1";
}
inline void Diagnostic(const std::string&) {}
}
"""
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-d3d12-native-fences-") as temp:
            temp = Path(temp)
            (temp / "stubs.h").write_text(
                "#include <cassert>\n#include <cstdlib>\n#include <string>\n#include <condition_variable>\n"
                + native_stubs
            )
            for name in (
                "common/common_types.h",
                "common/settings.h",
                "common/thread.h",
                "video_core/delayed_destruction_ring.h",
                "video_core/gpu.h",
                "video_core/host1x/host1x.h",
                "video_core/host1x/syncpoint_manager.h",
                "video_core/rasterizer_interface.h",
                "video_core/renderer_d3d12/d3d12_buffer_cache.h",
                "video_core/renderer_d3d12/d3d12_texture_cache.h",
                "video_core/renderer_d3d12/d3d12_query_cache.h",
                "video_core/buffer_cache/buffer_cache.h",
                "video_core/texture_cache/texture_cache.h",
                "video_core/query_cache/query_cache.h",
            ):
                header = temp / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "stubs.h"\n')
            source = temp / "native.cpp"
            rasterizer_source = (
                ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp"
            ).read_text()
            flush_body = rasterizer_source.split("void RasterizerD3D12::FlushCommands() {", 1)[1]
            flush_body = flush_body.split("\n}", 1)[0]
            flush_body = flush_body[flush_body.index("    // DispatchCalls") :]
            flush_body = (
                flush_body.replace("fence_manager.", "tail.")
                .replace(
                    "context.Submit(FrameCount::ForcedSubmitGuestFlush)",
                    "tail_context.Submit(D3D12::FrameCount::ForcedSubmitGuestFence)",
                )
                .replace("    default_pipeline_cache->FlushDiskCache();", "")
            )
            source.write_text(r"""
#include <cassert>
#include "common/gpu_thread_profile.h"
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"
int main() {
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache texture, buffer, query;
    texture.ready = buffer.ready = query.ready = true;
    Settings::high = false;
    D3D12::Context context;
    D3D12::FenceManager manager(rasterizer, gpu, texture, buffer, query, context);
    // A submitted query reduction is not enough: non-fence reports still need publication.
    // Exercise the production FlushCommands body with no subsequent guest command/frame.
    {
        VideoCore::RasterizerInterface tail_rasterizer;
        Tegra::GPU tail_gpu;
        Cache tail_texture, tail_buffer, tail_query;
        tail_texture.ready = tail_buffer.ready = tail_query.ready = true;
        D3D12::Context tail_context;
        D3D12::FenceManager tail(tail_rasterizer, tail_gpu, tail_texture, tail_buffer,
                                 tail_query, tail_context);
        tail_rasterizer.on_flush = [&] { __FLUSH_COMMANDS__ };
        std::atomic<int> reports{};
        for (int i = 0; i < 79; ++i) {
            tail.SyncOperation([&] {
                assert(tail_query.pops == 1); // Readback retirement precedes guest writes.
                ++reports;
            });
        }
        // Reproduce the old path: hardware completes but no fence owns these callbacks.
        tail_context.Submit(D3D12::FrameCount::ForcedSubmitGuestFence);
        tail_context.Complete();
        assert(reports == 0 && tail_query.commits == 0 && tail.PendingGuestCallbacks() == 79);
        __FLUSH_COMMANDS__
        tail_context.Complete();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (tail.PendingGuestCallbacks() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        assert(reports == 79 && tail.PendingGuestCallbacks() == 0);
        assert(tail_query.commits == 1 && tail_query.pops == 1);
        // An empty boundary must not create another cache batch or native submission.
        const auto submits = tail_context.submitted.load();
        __FLUSH_COMMANDS__
        assert(tail_context.submitted == submits && tail_query.commits == 1);
    }
    // SignalFence flushes under the fence lock when batching is off; the hook must not lock.
    rasterizer.on_flush = [&] { manager.NotifyDeferredSubmit(); };
    if (D3D12::BisectEnabled("NXBOX_D3D12_FENCE_BATCHING") ||
        D3D12::BisectEnabled("NXBOX_D3D12_SYNC_FAST")) {
        // DBSZ logs ~42 guest fences/frame. They share one tick without a producer wait.
        constexpr int fences_per_frame = 42;
        std::atomic<int> callbacks{};
        for (int i = 0; i < fences_per_frame; ++i) {
            manager.SignalSyncPoint(7);
            manager.SignalFence([&, i] {
                assert(context.completed >= 1);
                assert(gpu.host.sync.host == i + 1);
                assert(callbacks == i);
                ++callbacks;
            });
        }
        const auto observed = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!context.submission_checks && std::chrono::steady_clock::now() < observed)
            std::this_thread::yield();
        assert(context.submission_checks > 0 && context.waits == 0);
        assert(context.submitted == 0 && context.completed == 0);
        assert(gpu.host.sync.guest == fences_per_frame && gpu.host.sync.host == 0 && callbacks == 0);
        // Command-list end / present submits once; completion needs no new guest commands.
        context.Submit(D3D12::FrameCount::ForcedSubmitGuestFence);
        context.Complete();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (callbacks != fences_per_frame && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        assert(callbacks == fences_per_frame && gpu.host.sync.host == fences_per_frame && context.submitted == 1);
        // A real guest wait must submit pending work before sleeping on the drain marker.
        context.RecordWork();
        manager.SignalSyncPoint(7);
        std::jthread complete([&] {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (context.submitted < 2 && std::chrono::steady_clock::now() < limit)
                std::this_thread::yield();
            assert(context.submitted == 2);
            context.Complete();
        });
        manager.WaitPendingFences(true);
        assert(gpu.host.sync.host == fences_per_frame + 1 && context.submitted == 2);
        assert(texture.commits == texture.pops && buffer.commits == buffer.pops);
        assert(query.commits == query.pops);
        // Fences with no new recording reuse the completed tick and need no empty submit.
        manager.SignalReference();
        manager.WaitPendingFences(true);
        assert(context.submitted == 2);
        // The drain hook submits the pending tick, and wakes the fencing thread afterwards.
        context.RecordWork();
        const int hook_calls = context.submit_calls;
        manager.FlushPendingFences();
        assert(context.submit_calls == hook_calls + 1 && context.submitted == 3);
        context.Complete();
        // Stopping with an unsubmitted tick cannot strand the completion thread.
        D3D12::Context broken;
        broken.fail = true;
        D3D12::FenceManager failing(rasterizer, gpu, texture, buffer, query, broken);
        bool rejected = false;
        try { failing.WaitPendingFences(true); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
        // A Submit that throws after closing the list never signals the tick the fences
        // captured. The drain that hit it fails, and so must every later drain: Submit is a
        // no-op by then, so only the fencing thread can report the failure.
        D3D12::Context stranded_context;
        stranded_context.submit_throws = true;
        D3D12::FenceManager stranded(rasterizer, gpu, texture, buffer, query, stranded_context);
        stranded.SignalReference();
        for (int drain = 0; drain < 3; ++drain) {
            bool failed = false;
            try { stranded.WaitPendingFences(true); }
            catch (const std::runtime_error&) { failed = true; }
            assert(failed);
        }
        assert(stranded_context.submitted == 0 && stranded_context.waits == 1);
        // SignalFence never flushes while batching: the pending tick belongs to the recording.
        assert(rasterizer.submits == 0);
        context.RecordWork();
        manager.SignalReference();
        return 0;
    }
    namespace MP = Common::MaxwellMethodProfile;
    struct MethodClock : MP::Collector {
        Common::GpuProfile::Tick now{};
        Common::GpuProfile::Tick ReadTicks() override { return ++now; }
    } clock;
    MP::collector = &clock;
    Common::GpuProfile::Reset();
    Common::GpuProfile::enabled = true;
    Common::GpuProfile::RequestFence(7, 1);
    // Queue multiple submissions without waiting for GPU completion on the producer.
    manager.SignalSyncPoint(7);
    context.RecordWork();
    manager.SignalReference();
    context.RecordWork();
    std::atomic<bool> callback{};
    manager.SignalFence([&] {
        assert(context.completed >= 3);
        assert(gpu.host.sync.host == 1);
        callback = true;
    });
    assert(context.submitted == 3 && context.completed == 0);
    for (auto key : {MP::CommitAsyncFlushes, MP::CreateFence, MP::QueueFence,
                     MP::FlushCommands, MP::InvalidateGpuCache, MP::FenceLock}) {
        assert(clock.samples[key].count == 3 && clock.samples[key].ticks == 3);
    }
    assert(!clock.active);
    MP::collector = nullptr;
    assert(gpu.host.sync.guest == 1 && gpu.host.sync.host == 0 && !callback);
    // No further guest command or presentation is needed to retire the last fence.
    context.Complete();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!callback && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    assert(callback && gpu.host.sync.host == 1);
    const auto profile = Common::GpuProfile::TakeWindow();
    assert(profile.fence_total.count == 1 && profile.fence_queued.count == 1);
    assert(profile.fence_execution.count == 1);
    Common::GpuProfile::enabled = false;
    assert(query.commits == query.pops && buffer.commits == buffer.pops);
    assert(texture.commits == texture.pops && context.waits == 3);
    // Without batching every fence submitted when queued: draining adds no submission.
    context.RecordWork();
    const int drain_calls = context.submit_calls;
    manager.FlushPendingFences();
    assert(context.submit_calls == drain_calls && rasterizer.submits > 0);
    D3D12::Context broken;
    broken.fail = true;
    D3D12::FenceManager failing(rasterizer, gpu, texture, buffer, query, broken);
    failing.SignalReference();
    bool rejected = false;
    try { failing.WaitPendingFences(true); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected); // A failed completion thread cannot strand a forced drain.

}
""")
            source.write_text(source.read_text().replace("__FLUSH_COMMANDS__", flush_body))
            binary = temp / "native"
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-pthread",
                    "-I",
                    str(temp),
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    str(ROOT / "src/video_core/renderer_d3d12/d3d12_fence_manager.cpp"),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for batching, fast in (("0", "0"), ("1", "0"), ("0", "1"), ("1", "1")):
                for overlap in ("0", "1"):
                    with self.subTest(fence_batching=batching, sync_fast=fast, overlap=overlap):
                        env = dict(
                            os.environ,
                            NXBOX_D3D12_FENCE_BATCHING=batching,
                            NXBOX_D3D12_SYNC_FAST=fast,
                            NXBOX_D3D12_OVERLAP_SYNCPOINT=overlap,
                        )
                        subprocess.run([str(binary)], check=True, timeout=10, env=env)

    def test_fencing_thread_wakes_when_a_deferred_tick_is_submitted(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-fence-wakeup-") as temp:
            temp = Path(temp)
            (temp / "stubs.h").write_text(STUBS)
            for name in (
                "common/common_types.h",
                "common/settings.h",
                "common/thread.h",
                "video_core/delayed_destruction_ring.h",
                "video_core/gpu.h",
                "video_core/host1x/host1x.h",
                "video_core/host1x/syncpoint_manager.h",
                "video_core/rasterizer_interface.h",
            ):
                header = temp / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "stubs.h"\n')
            source = temp / "wakeup.cpp"
            binary = temp / "wakeup"
            source.write_text(ASYNC_DRIVER)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-pthread",
                    "-I",
                    str(temp),
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
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_submissions_that_defer_fences_wake_the_fencing_thread(self):
        render = ROOT / "src/video_core/renderer_d3d12"
        context = (render / "d3d12_context.cpp").read_text()
        submit = context[context.index("u64 Context::Submit(FrameCount") :]
        submit = submit[: submit.index("void Context::Finish")]
        # Flagged while unwinding from any throw in Submit, cleared once the tick is signalled.
        self.assertIn("std::uncaught_exceptions()", submit)
        self.assertIn("flag.store(true", submit)
        self.assertIn("failure_flag{submit_failed}", submit)
        self.assertLess(
            submit.index("submitted = tick"), submit.index("submit_failed.store(false")
        )
        rasterizer = (render / "d3d12_rasterizer.cpp").read_text()
        flush = rasterizer[rasterizer.index("void RasterizerD3D12::FlushCommands()") :]
        self.assertIn("NotifyDeferredSubmit()", flush[: flush.index("TickFrame")])
        renderer = (render / "renderer_d3d12.cpp").read_text()
        present = renderer.index("context.Submit(FrameCount::ForcedSubmitPresent)")
        self.assertIn("NotifyFencesSubmitted()", renderer[present : present + 200])

    def test_dma_sync_flushes_deferred_fences_only_for_batching(self):
        source = (ROOT / "src/video_core/dma_pusher.cpp").read_text()
        start = source.index("    if (signal_sync && !synced) {")
        end = source.index("\n    if (header.size", start)
        wait_block = source[start:end]
        # Every backend reaches this wait; FlushCommands would submit, flush and invalidate.
        self.assertNotIn("FlushCommands", wait_block)
        self.assertIn("FlushDeferredFences()", wait_block)
        interface = (ROOT / "src/video_core/rasterizer_interface.h").read_text()
        self.assertIn("virtual void FlushDeferredFences() {}", interface)
        for backend in ("renderer_vulkan", "renderer_opengl", "renderer_null"):
            for path in (ROOT / "src/video_core" / backend).rglob("*"):
                if path.suffix in (".h", ".cpp"):
                    self.assertNotIn("FlushDeferredFences", path.read_text(), path)
        driver = r"""
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include "common/gpu_thread_profile.h"
struct Rasterizer {
    int commands{};
    virtual ~Rasterizer() = default;
    virtual void FlushCommands() { ++commands; }
    virtual void FlushDeferredFences() {}
};
// Vulkan, OpenGL and D3D12 with batching off keep the interface default.
struct Other : Rasterizer {};
struct Batching : Rasterizer {
    std::function<void()> complete;
    int submits{};
    void FlushDeferredFences() override { ++submits; complete(); }
};
struct Dma {
    Rasterizer* rasterizer;
    bool signal_sync = true, synced = false;
    std::mutex sync_mutex;
    std::condition_variable sync_cv;
    void Complete() {
        std::scoped_lock lock{sync_mutex};
        synced = true;
        sync_cv.notify_all();
    }
    void Step() {
__WAIT_BLOCK__
    }
};
int main() {
    {
        Other rasterizer;
        Dma dma{&rasterizer};
        // The memory-sync callback arrives from another thread, as on the completion thread.
        std::jthread completion([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            dma.Complete();
        });
        dma.Step();
        assert(rasterizer.commands == 0 && !dma.signal_sync && !dma.synced);
    }
    {
        Batching rasterizer;
        Dma dma{&rasterizer};
        rasterizer.complete = [&] { dma.Complete(); };
        dma.Step();
        assert(rasterizer.submits == 1 && rasterizer.commands == 0);
        assert(!dma.signal_sync && !dma.synced);
    }
}
""".replace("__WAIT_BLOCK__", wait_block)
        with tempfile.TemporaryDirectory(prefix="nxbox-dma-fence-batch-") as temp:
            temp = Path(temp)
            source = temp / "dma.cpp"
            binary = temp / "dma"
            source.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-pthread",
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
            subprocess.run([str(binary)], check=True, timeout=10)
