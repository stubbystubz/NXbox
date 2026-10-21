# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise renderer-neutral hooks and cross-thread wait/fence windows without D3D12."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <thread>
#include <vector>
#include "common/gpu_thread_profile.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
using namespace Common::GpuProfile;
struct TestSink final : Sink {
    D3D12::FrameTiming timing;
    Tick now{};
    std::size_t Enter(Phase phase) override {
        auto previous = timing.active;
        timing.Switch(static_cast<D3D12::FramePhase>(
            static_cast<std::size_t>(D3D12::FramePhase::DmaPusher) +
            static_cast<std::size_t>(phase)), now);
        return static_cast<std::size_t>(previous);
    }
    void Leave(std::size_t previous) override {
        timing.Switch(static_cast<D3D12::FramePhase>(previous), now);
    }
};
int main() {
    Reset();
    { WaitScope disabled{Wait::SyncpointHost}; }
    RequestFence(1, 2);
    assert(requests.empty());
    assert(TakeWindow().waits[0].count == 0);
    TestSink owner;
    sink = &owner;
    recording_enabled = true;
    {
        PhaseScope parser{Phase::DmaPusher};
        owner.now = 10;
        {
            PhaseScope methods{Phase::Maxwell3DMethods};
            owner.now = 20;
            try {
                PhaseScope macro{Phase::MacroExecution};
                owner.now = 30;
                throw 1;
            } catch (int) {}
            owner.now = 40;
        }
        owner.now = 50;
    }
    assert(owner.timing.active == D3D12::FramePhase::BetweenDraws);
    assert(owner.timing.cpu[static_cast<std::size_t>(D3D12::FramePhase::DmaPusher)] == 20);
    assert(owner.timing.cpu[static_cast<std::size_t>(D3D12::FramePhase::Maxwell3DMethods)] == 20);
    assert(owner.timing.cpu[static_cast<std::size_t>(D3D12::FramePhase::MacroExecution)] == 10);
    // Queue sleeps stay exclusive, but never enter the renderer-stall bucket.
    owner.timing = {};
    owner.now = 0;
    {
        PhaseScope parser{Phase::DmaPusher};
        owner.now = 10;
        {
            WaitScope idle{Wait::GpuQueueIdle};
            assert(owner.timing.active == D3D12::FramePhase::QueueIdle);
            owner.now = 40;
            {
                WaitScope async_tick{Wait::TickGPU, false};
                assert(owner.timing.active == D3D12::FramePhase::QueueIdle);
            }
            owner.now = 90;
        }
        assert(owner.timing.active == D3D12::FramePhase::DmaPusher);
        owner.now = 100;
        try {
            WaitScope acquire{Wait::PullerAcquire};
            assert(owner.timing.active == D3D12::FramePhase::SyncWait);
            owner.now = 120;
            throw 1;
        } catch (int) {
        }
        assert(owner.timing.active == D3D12::FramePhase::DmaPusher);
        owner.now = 130;
    }
    assert(owner.timing.cpu[static_cast<std::size_t>(D3D12::FramePhase::DmaPusher)] == 30);
    assert(owner.timing.cpu[static_cast<std::size_t>(D3D12::FramePhase::QueueIdle)] == 80);
    assert(owner.timing.cpu[static_cast<std::size_t>(D3D12::FramePhase::SyncWait)] == 20);
    std::thread other([] { assert(sink == nullptr); PhaseScope ignored{Phase::DmaPusher}; });
    other.join();
    // Method counts are exact, units retain bulk words, only 1/64 calls is timed.
    TakeMethodWindow();
    for (int i = 0; i < 128; ++i) {
        MethodScope dirty{MethodClass::DirtyFlags};
    }
    { MethodScope cb{MethodClass::ConstantBuffer, 16}; }
    auto methods = TakeMethodWindow();
    assert(methods[0].count == 128 && methods[0].units == 128);
    assert(methods[0].timed_calls == 2);
    assert(methods[1].count == 1 && methods[1].units == 16);
    assert(TakeMethodWindow()[0].count == 0);
    {
        MethodScope cross_window{MethodClass::QueryReport, 3};
        cross_window.Skip();
        assert(TakeMethodWindow()[3].count == 0);
    }
    const auto completed_method = TakeMethodWindow()[3];
    assert(completed_method.count == 1 && completed_method.units == 3);
    assert(completed_method.timed_calls == 1 && completed_method.skipped == 1);
    recording_enabled = false;
    sink = nullptr;
    { MethodScope disabled{MethodClass::DirtyFlags}; }
    assert(TakeMethodWindow()[0].count == 0);
    enabled = true;
    { WaitScope idle{Wait::GpuQueueIdle}; }
    const auto idle_window = TakeWindow();
    assert(idle_window.waits[static_cast<std::size_t>(Wait::GpuQueueIdle)].count == 1);
    assert(idle_window.waits[static_cast<std::size_t>(Wait::TickGPU)].count == 0);
    { WaitScope async_tick{Wait::TickGPU, false}; }
    assert(TakeWindow().waits[static_cast<std::size_t>(Wait::TickGPU)].count == 0);
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i)
        workers.emplace_back([] {
            for (int j = 0; j < 100; ++j) {
                WaitScope wait{Wait::RequestSyncOperation};
            }
        });
    for (auto& worker : workers) worker.join();
    auto window = TakeWindow();
    const auto& sample = window.waits[static_cast<std::size_t>(Wait::RequestSyncOperation)];
    assert(sample.count == 800 && sample.total_ns >= sample.max_ns);
    assert(TakeWindow().waits[static_cast<std::size_t>(Wait::RequestSyncOperation)].count == 0);
    RequestFence(1, 2);
    assert(!ProcessFence(2, 2).requested);
    auto latency = ProcessFence(1, 2);
    assert(latency.requested && latency.processed >= latency.requested);
    assert(!ProcessFence(1, 2).requested);
    // A request and processing in one window may complete in the next.
    assert(TakeWindow().fence_total.count == 0);
    CompleteFence(latency);
    window = TakeWindow();
    assert(window.fence_total.count == 1 && window.fence_queued.count == 1);
    assert(window.fence_execution.count == 1);
    assert(window.fence_total.total_ns == window.fence_queued.total_ns + window.fence_execution.total_ns);
    for (unsigned i = 0; i < 4097; ++i) RequestFence(1, i);
    assert(requests.size() == 4096 && TakeWindow().dropped_requests == 1);
    Reset();
    assert(requests.empty());
    enabled = false;
}
"""


class GpuThreadProfileTests(unittest.TestCase):
    def test_exclusive_hooks_disabled_path_concurrency_and_latency(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-gpu-profile-") as directory:
            source = Path(directory) / "driver.cpp"
            binary = Path(directory) / "test"
            source.write_text(DRIVER)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-fsanitize=address,undefined",
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
            subprocess.run(
                [str(binary)],
                check=True,
                timeout=15,
                env=dict(os.environ, NXBOX_GPU_METHOD_PROFILE="1"),
            )

    def test_phases_and_guest_wait_line_are_wired(self):
        backend = ROOT / "src/video_core/renderer_d3d12"
        source = (backend / "d3d12_context.cpp").read_text()
        for name in [
            "dma_pusher",
            "maxwell3d_methods",
            "macro_execution",
            "dma_engine",
            "inline_to_memory",
            "compute_dispatch_setup",
            "fermi2d",
            "sync_wait",
            "buffer_cache_bind",
            "texture_cache_update",
            "descriptor_and_pipeline",
            "queue_idle",
        ]:
            self.assertIn(f'"{name}"', source)
        self.assertIn("gpu_queue_idle_wall_ms=", source)
        self.assertIn("if (i == static_cast<size_t>(FramePhase::QueueIdle))", source)
        for field in [
            "D3D12_GUEST_WAITS",
            "_count=",
            "_total_ms=",
            "_max_ms=",
            "fence_queued_avg_ms",
            "fence_gpu_execution_avg_ms",
            "fence_total_avg_ms",
        ]:
            self.assertIn(field, source)
        self.assertIn(
            'BisectEnabled("NXBOX_D3D12_FRAME_PROFILE")', (backend / "d3d12_context.h").read_text()
        )
        rasterizer = (backend / "d3d12_rasterizer.cpp").read_text()
        lifecycle = rasterizer.split("void RasterizerD3D12::ProfileGpuThread")[1].split(
            "void RasterizerD3D12::SignalSyncPoint"
        )[0]
        self.assertLess(
            lifecycle.index("if (!context.Profile().Enabled())"),
            lifecycle.index("enabled.store(true)"),
        )
        self.assertLess(
            lifecycle.index("sink = &context.Profile()"),
            lifecycle.index("recording_enabled = true"),
        )
        self.assertLess(
            lifecycle.index("recording_enabled = false"),
            lifecycle.index("sink = nullptr"),
        )
        for path, marker in [
            ("src/video_core/gpu_thread.cpp", "ProfileGpuThread(true)"),
            ("src/video_core/host1x/syncpoint_manager.cpp", "Wait::SyncpointHost"),
            ("src/video_core/gpu.cpp", "Wait::RequestSyncOperation"),
            ("src/core/hle/service/nvdrv/devices/nvhost_ctrl.cpp", "Wait::NvdrvEvent"),
            ("src/core/hle/service/nvdrv/devices/nvhost_gpu.cpp", "RequestFence("),
        ]:
            self.assertIn(marker, (ROOT / path).read_text())
        fences = (backend / "d3d12_fence_manager.cpp").read_text()
        self.assertIn("ProcessFence(", fences)
        self.assertIn("CompleteFence(latency)", fences)
