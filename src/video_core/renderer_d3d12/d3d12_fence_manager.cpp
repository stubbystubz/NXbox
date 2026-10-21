// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/nxbox_wait_diag.h"
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"

#include "common/gpu_thread_profile.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/texture_cache/texture_cache.h"

namespace D3D12 {
namespace {
bool DeferredFenceSubmission() {
    // Reuse the established progress/drain protocol. Fewer fence-only lists also
    // avoid forcing allocator recycling on the next Commands() boundary.
    return BisectEnabled("NXBOX_D3D12_SYNC_FAST") || BisectEnabled("NXBOX_D3D12_FENCE_BATCHING");
}
} // namespace
void InnerFence::Queue() {
    if (!is_stubbed) {
        tick = !DeferredFenceSubmission() ? context.Submit(FrameCount::ForcedSubmitGuestFence)
                                          : context.PendingFenceTick();
    }
}
bool InnerFence::IsSubmitted() const {
    return is_stubbed || context.IsTickSubmitted(tick);
}
bool InnerFence::IsSignaled() const {
    return is_stubbed || context.IsFree(tick);
}
void InnerFence::Wait() {
    if (!is_stubbed) {
        const Common::GpuProfile::WaitScope origin{Common::GpuProfile::Wait::GuestFenceCompletion};
        try {
            context.WaitSubmitted(tick);
        } catch (const std::exception& error) {
            context.ReportFailure(error);
            throw;
        }
    }
}
FenceManager::FenceManager(VideoCore::RasterizerInterface& rasterizer, Tegra::GPU& gpu,
                           TextureCache& texture_cache, BufferCache& buffer_cache,
                           QueryCache& query_cache, Context& context)
    : GenericFenceManager{rasterizer, gpu, texture_cache, buffer_cache, query_cache},
      context{context} {}
FenceManager::~FenceManager() {
    // Stop while the derived vtable and Context reference are still alive.
    StopFenceThread();
}
void FenceManager::SignalFence(std::function<void()>&& func) {
    ++pending_guest_fences;
    GenericFenceManager::SignalFence([this, func = std::move(func)] {
        func();
        --pending_guest_fences;
    });
}
void FenceManager::SyncOperation(std::function<void()> &&func) {
    ++pending_guest_callbacks;
    GenericFenceManager::SyncOperation([this, func = std::move(func)] {
        func();
        --pending_guest_callbacks;
    });
}
void FenceManager::SignalSyncPoint(u32 value) {
    auto publish_fence = [this, value] {
        ++pending_syncpoints;
        NxboxWait::Add(NxboxWait::Counter::SyncpointQueued);
        LogSyncpoints();
        const auto diagnostic_token =
            NxboxWait::Enabled() ? NxboxWait::BeginPending(NxboxWait::PendingKind::Syncpoint, value,
                                                           context.PendingFenceTick())
                                 : 0;
        auto signal = [this, value, diagnostic_token] {
            syncpoint_manager.IncrementHost(value);
            NxboxWait::EndPending(diagnostic_token);
            --pending_syncpoints;
            ++signaled_syncpoints;
            NxboxWait::Add(NxboxWait::Counter::SyncpointSignaled);
            LogSyncpoints();
        };
        if (!Common::GpuProfile::enabled.load(std::memory_order_relaxed)) {
            SignalFence(std::move(signal));
            return;
        }
        const auto latency = Common::GpuProfile::ProcessFence(
            value, syncpoint_manager.GetGuestSyncpointValue(value));
        SignalFence([signal = std::move(signal), latency] {
            Common::GpuProfile::CompleteFence(latency);
            signal();
        });
    };
    if (BisectEnabled("NXBOX_D3D12_OVERLAP_SYNCPOINT")) {
        // The guest counter must be visible before the host can complete. Capture the
        // preceding commands and cache retirements before Composite can append work
        // or block in Present. Host visibility still requires the real GPU fence.
        syncpoint_manager.IncrementGuest(value, std::move(publish_fence));
    } else {
        syncpoint_manager.IncrementGuest(value);
        publish_fence();
    }
}
void FenceManager::SignalReference() {
    SignalFence([] {});
}
void FenceManager::LogSyncpoints() const {
    const auto signaled = signaled_syncpoints.load();
    if (signaled < 8 || signaled % 120 == 0) {
        Diagnostic("D3D12_SYNCPOINTS pending=" + std::to_string(pending_syncpoints.load()) +
                   " signaled=" + std::to_string(signaled));
    }
}
bool FenceManager::ShouldFlushFence() const {
    return !DeferredFenceSubmission();
}
void FenceManager::FlushPendingFences() {
    // Without batching every fence submitted when it was queued; keep that path untouched.
    if (!DeferredFenceSubmission())
        return;
    context.Submit(FrameCount::ForcedSubmitGuestFence);
    NotifySubmitted();
}
void FenceManager::NotifyDeferredSubmit() {
    // Without batching this can run under the fence lock (SignalFence flushes there), and
    // there is no deferred tick to wake the fencing thread for.
    if (DeferredFenceSubmission())
        NotifySubmitted();
}
bool FenceManager::IsFenceSubmitted(const Fence& fence) const {
    return !DeferredFenceSubmission() || fence->IsSubmitted();
}
Fence FenceManager::CreateFence(bool is_stubbed) {
    return std::make_shared<InnerFence>(context, is_stubbed);
}
void FenceManager::QueueFence(Fence& fence) {
    fence->Queue();
}
bool FenceManager::IsFenceSignaled(Fence& fence) const {
    return fence->IsSignaled();
}
void FenceManager::WaitFence(Fence& fence) {
    fence->Wait();
}
} // namespace D3D12
