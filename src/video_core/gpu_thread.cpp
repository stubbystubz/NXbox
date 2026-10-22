// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/gpu_thread_profile.h"
#include "common/nxbox_stall.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"
#ifdef HAS_D3D12
#include "eden_uwp/renderer_boundary.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"
#endif

#ifdef NXBOX_UWP
#include "eden_uwp/crash_logger.h"
// Declared directly: windows.h in the video core clashes with glad.
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
namespace {
std::uint32_t NxboxCurrentThreadId() {
    return static_cast<std::uint32_t>(GetCurrentThreadId());
}
} // namespace
#endif

namespace VideoCommon::GPUThread {

ThreadManager::ThreadManager(Core::System& system_) : system{system_} {}

ThreadManager::~ThreadManager() = default;

void ThreadManager::StartThread(VideoCore::RendererBase& renderer,
                                Core::Frontend::GraphicsContext& context,
                                Tegra::Control::Scheduler& scheduler) {
    rasterizer = renderer.ReadRasterizer();
    thread = std::jthread([&](std::stop_token stop_token) {
#ifdef HAS_D3D12
        const auto report = [&](const std::exception& error) {
            return renderer.ReportFailure(error);
        };
        const bool initialized = EdenXbox::RendererBoundary(
            "GPU initialization: non-standard exception",
            [&] {
#endif
                Common::SetCurrentThreadName("GPU");
#ifdef NXBOX_UWP
                EdenXbox::InstallThreadCrashHandlers();
                NxboxStall::gpu_thread_id.store(NxboxCurrentThreadId());
#endif
                Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
                Common::SetCurrentThreadToPerformanceCores();
                system.RegisterHostThread();

#ifdef HAS_D3D12
                context.MakeCurrent();
            },
            report);
        // GraphicsContext::Scoped has a noexcept destructor which calls DoneCurrent.
        // Invoke release explicitly under the same boundary to prevent termination.
        SCOPE_EXIT {
            if (initialized)
                EdenXbox::RendererBoundary(
                    "GPU context release: non-standard exception", [&] { context.DoneCurrent(); },
                    report);
        };
#else
        auto current_context = context.Acquire();
#endif
        rasterizer->ProfileGpuThread(true);
        SCOPE_EXIT {
            rasterizer->ProfileGpuThread(false);
        };
        CommandDataContainer next;
        while (!stop_token.stop_requested()) {
            bool have_command = true;
            {
                const Common::GpuProfile::WaitScope wait{Common::GpuProfile::Wait::GpuQueueIdle};
                const auto deadline = renderer.PresentationDeadline();
                if (deadline == std::chrono::steady_clock::time_point::max())
                    state.queue.PopWait(next, stop_token);
                else
                    have_command = state.queue.PopWaitUntil(next, stop_token, deadline);
            }
            if (stop_token.stop_requested()) {
                break;
            }
            if (!have_command) {
                // A publication racing the timeout always wins over generation.
                have_command = state.queue.TryPop(next);
                if (!have_command) {
#ifdef HAS_D3D12
                    EdenXbox::RendererBoundary(
                        "GPU idle presentation: non-standard exception",
                        [&] {
                            if (initialized && !renderer.HasFailed())
                                renderer.PresentIdle();
                        },
                        report);
#else
                    renderer.PresentIdle();
#endif
                    continue;
                }
            }
            // Keep consuming queued fences after a renderer failure so synchronous
            // callers and game shutdown cannot deadlock behind a stopped GPU thread.
#ifdef HAS_D3D12
            EdenXbox::RendererBoundary(
                "GPU command: non-standard exception",
                [&] {
                    if (!initialized || renderer.HasFailed()) {
                        system.GPU().CancelSyncRequests();
                    } else {
#endif
                        const NxboxStall::Scope busy_scope{NxboxStall::Kind::GpuBusy};
                        if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                            scheduler.Push(system.GPU(), submit_list->channel,
                                           std::move(submit_list->entries));
                        } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                            system.GPU().TickWork();
                        } else if (const auto* flush =
                                       std::get_if<FlushRegionCommand>(&next.data)) {
                            renderer.ReadRasterizer()->FlushRegion(flush->addr, flush->size);
                        } else if (const auto* invalidate =
                                       std::get_if<InvalidateRegionCommand>(&next.data)) {
                            renderer.ReadRasterizer()->OnCacheInvalidation(invalidate->addr,
                                                                           invalidate->size);
                        } else {
                            ASSERT(false);
                        }
#ifdef HAS_D3D12
                    }
                },
                [&](const std::exception& error) {
                    const bool handled = report(error);
                    if (handled)
                        system.GPU().CancelSyncRequests();
                    return handled;
                });
#endif
            state.signaled_fence.store(next.fence);
            if (next.block) {
                // We have to lock the write_lock to ensure that the condition_variable wait not get
                // a race between the check and the lock itself.
                std::scoped_lock lk{state.write_lock};
                state.cv.notify_all();
            }
        }
    });
}

void ThreadManager::SubmitList(s32 channel, Tegra::CommandList&& entries, bool is_async) {
    PushCommand(SubmitListCommand(channel, std::move(entries)), false, is_async);
}

void ThreadManager::FlushRegion(DAddr addr, u64 size, bool is_async) {
    if (!is_async) {
        // Always flush with synchronous GPU mode
        PushCommand(FlushRegionCommand(addr, size), false, is_async);
    }
}

void ThreadManager::TickGPU(bool is_async) {
    const Common::GpuProfile::WaitScope gpu_profile_scope{Common::GpuProfile::Wait::TickGPU,
                                                          !is_async};
    PushCommand(GPUTickCommand(), false, is_async);
}

void ThreadManager::InvalidateRegion(DAddr addr, u64 size) {
#ifdef HAS_D3D12
    EdenXbox::RendererBoundary(
        "GPU cache invalidation: non-standard exception",
        [&] { rasterizer->OnCacheInvalidation(addr, size); },
        [&](const std::exception& error) { return system.GPU().Renderer().ReportFailure(error); });
#else
    rasterizer->OnCacheInvalidation(addr, size);
#endif
}

void ThreadManager::FlushAndInvalidateRegion(DAddr addr, u64 size, bool is_async) {
    if (Settings::IsGPULevelHigh()) {
        if (!is_async) {
            PushCommand(FlushRegionCommand(addr, size), false, is_async);
        } else {
            auto& gpu = system.GPU();
            const u64 fence = gpu.RequestFlush(addr, size);
            TickGPU(is_async);
            const Common::GpuProfile::WaitScope wait{Common::GpuProfile::Wait::RequestFlush};
            gpu.WaitForSyncOperation(fence);
        }
    }
#ifdef HAS_D3D12
    EdenXbox::RendererBoundary(
        "GPU cache invalidation: non-standard exception",
        [&] { rasterizer->OnCacheInvalidation(addr, size); },
        [&](const std::exception& error) { return system.GPU().Renderer().ReportFailure(error); });
#else
    rasterizer->OnCacheInvalidation(addr, size);
#endif
}

u64 ThreadManager::PushCommand(CommandData&& command_data, bool block, bool is_async) {
    if (!is_async) {
        // In synchronous GPU mode, block the caller until the command has executed
        block = true;
    }

    std::unique_lock lk(state.write_lock);
    const u64 fence{++state.last_fence};
    state.queue.EmplaceWait(std::move(command_data), fence, block);

    if (block) {
        const Common::GpuProfile::WaitScope wait{Common::GpuProfile::Wait::GpuThreadCommand};
        state.cv.wait(lk, thread.get_stop_token(), [this, fence] {
            return fence <= state.signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

void ThreadManager::NotifyShutdown() {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
}

} // namespace VideoCommon::GPUThread
