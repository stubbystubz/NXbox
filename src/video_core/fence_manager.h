// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>

#include "common/common_types.h"
#include "common/maxwell_method_profile.h"
#include "common/nxbox_wait_diag.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/rasterizer_interface.h"

#ifdef NXBOX_UWP
#include "eden_uwp/crash_logger.h"
#endif

namespace VideoCommon {

class FenceBase {
public:
    explicit FenceBase(bool is_stubbed_) : is_stubbed{is_stubbed_} {}

    bool IsStubbed() const {
        return is_stubbed;
    }

protected:
    bool is_stubbed;
};

template <typename Traits>
class FenceManager {
    using TFence = typename Traits::FenceType;
    using TTextureCache = typename Traits::TextureCacheType;
    using TBufferCache = typename Traits::BufferCacheType;
    using TQueryCache = typename Traits::QueryCacheType;
    static constexpr bool can_async_check = Traits::HAS_ASYNC_CHECK;
    static constexpr bool require_gpu_completion = [] {
        if constexpr (requires { Traits::REQUIRE_GPU_COMPLETION; }) {
            return Traits::REQUIRE_GPU_COMPLETION;
        } else {
            return false;
        }
    }();

public:
    /// Notify the fence manager about a new frame
    void TickFrame() {
        std::unique_lock lock(ring_guard);
        delayed_destruction_ring.Tick();
    }

    // Unlike other fences, this one doesn't
    void SignalOrdering() {
        if constexpr (!can_async_check) {
            TryReleasePendingFences<false>();
        }
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.AccumulateFlushes();
    }

    void SignalReference() {
        std::function<void()> do_nothing([] {});
        SignalFence(std::move(do_nothing));
    }

    void SyncOperation(std::function<void()>&& func) {
        uncommitted_operations.emplace_back(std::move(func));
    }

    // Recording thread only. A native submit alone cannot publish SyncOperation callbacks.
    // Seal the tail before the producer goes idle, even without another guest fence.
    void CommitPendingOperations() {
        if (!uncommitted_operations.empty())
            SignalReference();
    }

    void SignalFence(std::function<void()>&& func) {
        const NxboxWait::Scope publication{NxboxWait::Kind::FencePublication,
                                           reinterpret_cast<std::uintptr_t>(this),
                                           "fence publication"};
        const bool delay_fence =
            require_gpu_completion ||
            (Settings::IsGPUFenceBehaviorDefault() ? Settings::IsGPULevelHigh()
                                                   : Settings::IsGPUFenceBehaviorBalanced() ||
                                                         Settings::IsGPUFenceBehaviorAccurate());
        const bool should_flush = require_gpu_completion || ShouldFlush();
        if constexpr (!can_async_check) {
            TryReleasePendingFences<false>();
        }
        // Query retirement normally adds one operation before the guest signal.
        uncommitted_operations.reserve(2);
        namespace Profile = Common::MaxwellMethodProfile;
        Profile::Measure(Profile::CommitAsyncFlushes, [&] { CommitAsyncFlushes(); });
        TFence new_fence =
            Profile::Measure(Profile::CreateFence, [&] { return CreateFence(!should_flush); });
        std::unique_lock fence_lock{guard, std::defer_lock};
        if constexpr (can_async_check) {
            Profile::Measure(Profile::FenceLock, [&] { fence_lock.lock(); });
            if (thread_error)
                std::rethrow_exception(thread_error);
        }
        if (delay_fence) {
            uncommitted_operations.emplace_back(std::move(func));
        }
        pending_operations.emplace_back(std::move(uncommitted_operations));
        Profile::Measure(Profile::QueueFence, [&] { QueueFence(new_fence); });
        if (!delay_fence) {
            func();
        }
        fences.push(std::move(new_fence));
        if (should_flush && ShouldFlushFence()) {
            Profile::Measure(Profile::FlushCommands, [&] { rasterizer.FlushCommands(); });
        }
        if constexpr (can_async_check) {
            fence_lock.unlock();
            cv.notify_all();
        }
        Profile::Measure(Profile::InvalidateGpuCache, [&] { rasterizer.InvalidateGPUCache(); });
    }

    void SignalSyncPoint(u32 value) {
        syncpoint_manager.IncrementGuest(value);
        std::function<void()> func([this, value] { syncpoint_manager.IncrementHost(value); });
        SignalFence(std::move(func));
    }

    /// Wakes the fencing thread after a submission that can make a deferred fence submitted.
    /// The lock orders the wake-up after the thread's IsFenceSubmitted check, so it cannot be
    /// missed between that check and the sleep. Never call this while holding the fence lock.
    void NotifySubmitted() {
        if constexpr (can_async_check) {
            {
                std::scoped_lock lock{guard};
            }
            cv.notify_all();
        }
    }

    void WaitPendingFences([[maybe_unused]] bool force) {
        if constexpr (!can_async_check) {
            TryReleasePendingFences<true>();
        } else {
            if (!force) {
                return;
            }
            // Shared state remains alive if completion fails before this marker runs.
            auto wait_finished = std::make_shared<std::atomic<bool>>(false);
            SignalFence([this, wait_finished] {
                std::scoped_lock lock{guard};
                wait_finished->store(true, std::memory_order_release);
                cv.notify_all();
            });
            FlushPendingFences();
            std::unique_lock lk(guard);
            cv.wait(lk,
                    [&] { return thread_error || wait_finished->load(std::memory_order_acquire); });
            if (thread_error)
                std::rethrow_exception(thread_error);
        }
    }

protected:
    explicit FenceManager(VideoCore::RasterizerInterface& rasterizer_, Tegra::GPU& gpu_,
                          TTextureCache& texture_cache_, TBufferCache& buffer_cache_,
                          TQueryCache& query_cache_)
        : rasterizer{rasterizer_}, gpu{gpu_}, syncpoint_manager{gpu.Host1x().GetSyncpointManager()},
          texture_cache{texture_cache_}, buffer_cache{buffer_cache_}, query_cache{query_cache_} {
        if constexpr (can_async_check) {
            fence_thread =
                std::jthread([this](std::stop_token token) { ReleaseThreadFunc(token); });
        }
    }

    virtual ~FenceManager() {
        StopFenceThread();
    }

    void StopFenceThread() {
        if constexpr (can_async_check) {
            fence_thread.request_stop();
            {
                // The release thread checks stop_requested() under this lock before it sleeps;
                // without it, the stop could land between that check and the wait and the join
                // would block forever.
                std::scoped_lock lock{guard};
            }
            cv.notify_all();
            if (fence_thread.joinable())
                fence_thread.join();
        }
    }

    /// Creates a Fence Interface, does not create a backend fence if 'is_stubbed' is
    /// true
    virtual TFence CreateFence(bool is_stubbed) = 0;
    /// Queues a fence into the backend if the fence isn't stubbed.
    virtual void QueueFence(TFence& fence) = 0;
    /// Notifies that the backend fence has been signaled/reached in host GPU.
    virtual bool IsFenceSignaled(TFence& fence) const = 0;
    /// Waits until a fence has been signalled by the host GPU.
    virtual void WaitFence(TFence& fence) = 0;

    // Backends may share a submission across fences, but a forced drain must submit it.
    virtual bool ShouldFlushFence() const {
        return true;
    }
    virtual void FlushPendingFences() {}
    virtual bool IsFenceSubmitted([[maybe_unused]] const TFence& fence) const {
        return true;
    }
    // Backstop for a submission that does not call NotifySubmitted().
    std::chrono::milliseconds submit_backstop{1};

    VideoCore::RasterizerInterface& rasterizer;
    Tegra::GPU& gpu;
    Tegra::Host1x::SyncpointManager& syncpoint_manager;
    TTextureCache& texture_cache;
    TBufferCache& buffer_cache;
    TQueryCache& query_cache;

private:
    template <bool force_wait>
    void TryReleasePendingFences() {
        while (!fences.empty()) {
            TFence& current_fence = fences.front();
            if ((require_gpu_completion || ShouldWait()) && !IsFenceSignaled(current_fence)) {
                if constexpr (force_wait) {
                    WaitFence(current_fence);
                } else {
                    return;
                }
            }
            PopAsyncFlushes();
            auto operations = std::move(pending_operations.front());
            pending_operations.pop_front();
            for (auto& operation : operations) {
                operation();
            }
            {
                std::unique_lock lock(ring_guard);
                delayed_destruction_ring.Push(std::move(current_fence));
            }
            fences.pop();
        }
    }

    void ReleaseThreadFunc(std::stop_token stop_token) {
#ifdef NXBOX_UWP
        EdenXbox::InstallThreadCrashHandlers();
#endif
        Common::SetCurrentThreadName("GPUFencingThread");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        Common::SetCurrentThreadToPerformanceCores();

        try {
            TFence current_fence;
            std::vector<std::function<void()>> current_operations;
            while (!stop_token.stop_requested()) {
                {
                    std::unique_lock lock(guard);
                    cv.wait(lock, [&] { return stop_token.stop_requested() || !fences.empty(); });
                    if (stop_token.stop_requested()) [[unlikely]] {
                        return;
                    }
                    if (!IsFenceSubmitted(fences.front())) {
                        // A deferred tick must not start the backend's GPU timeout yet.
                        // NotifySubmitted() ends this wait; the timeout only covers submissions
                        // that do not notify. Stop still wakes it, so the thread can be joined.
                        cv.wait_for(lock, submit_backstop, [&] {
                            return stop_token.stop_requested() || IsFenceSubmitted(fences.front());
                        });
                        continue;
                    }
                    current_fence = std::move(fences.front());
                    current_operations = std::move(pending_operations.front());
                    fences.pop();
                    pending_operations.pop_front();
                }
                if (!current_fence->IsStubbed()) {
                    WaitFence(current_fence);
                }
                {
                    const NxboxWait::Scope retirement{NxboxWait::Kind::CacheRetirement,
                                                      reinterpret_cast<std::uintptr_t>(this),
                                                      "fence cache retirement"};
                    PopAsyncFlushes();
                }
                {
                    const NxboxWait::Scope callbacks{NxboxWait::Kind::GuestCallbacks,
                                                     reinterpret_cast<std::uintptr_t>(this),
                                                     "fence guest callbacks"};
                    for (auto& operation : current_operations) {
                        operation();
                    }
                }
                {
                    std::unique_lock lock(ring_guard);
                    delayed_destruction_ring.Push(std::move(current_fence));
                }
            }
        } catch (...) {
            // Propagate to the producer's renderer boundary instead of terminating
            // the process or leaving a forced drain asleep forever.
            std::scoped_lock lock{guard};
            thread_error = std::current_exception();
            cv.notify_all();
        }
    }

    bool ShouldWait() const {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        return texture_cache.ShouldWaitAsyncFlushes() || buffer_cache.ShouldWaitAsyncFlushes() ||
               query_cache.ShouldWaitAsyncFlushes();
    }

    bool ShouldFlush() const {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        return texture_cache.HasUncommittedFlushes() || buffer_cache.HasUncommittedFlushes() ||
               query_cache.HasUncommittedFlushes();
    }

    void PopAsyncFlushes() {
        {
            std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
            texture_cache.PopAsyncFlushes();
            buffer_cache.PopAsyncFlushes();
        }
        query_cache.PopAsyncFlushes();
    }

    void CommitAsyncFlushes() {
        {
            std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
            texture_cache.CommitAsyncFlushes();
            buffer_cache.CommitAsyncFlushes();
        }
        query_cache.CommitAsyncFlushes();
    }

    std::queue<TFence> fences;
    // Moving a deque allocates a replacement map/block even for an empty batch.
    // A vector transfers its storage without allocating on every fence publication.
    std::vector<std::function<void()>> uncommitted_operations;
    std::deque<std::vector<std::function<void()>>> pending_operations;

    std::exception_ptr thread_error;
    std::mutex guard;
    std::mutex ring_guard;
    std::condition_variable cv;

    std::jthread fence_thread;

    DelayedDestructionRing<TFence, 8> delayed_destruction_ring;
};

} // namespace VideoCommon
