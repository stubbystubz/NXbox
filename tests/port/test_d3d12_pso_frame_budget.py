# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""First-use compiles share a frame deadline and execute on independent workers."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"
DRIVER = r"""
#include <cassert>
#include <future>
#include "video_core/renderer_d3d12/d3d12_async_pso.h"
#include "video_core/renderer_d3d12/d3d12_pso_frame_budget.h"
using namespace std::chrono_literals;
using Pool = D3D12::AsyncPsoWorkerPool<int, int>;
int main() {
    using D3D12::PsoFrameBudget;
    const auto start = PsoFrameBudget::Clock::time_point{};
    PsoFrameBudget budget;
    assert(budget.Remaining(5s, start) == 16ms);
    assert(budget.Remaining(5s, start + 6ms) == 10ms);
    assert(budget.Remaining(2ms, start + 6ms) == 2ms);
    assert(budget.Remaining(5s, start + 16ms) == 0ms);
    assert(budget.Remaining(5s, start + 1s) == 0ms);
    budget.Reset();
    assert(budget.Remaining(5s, start + 1s) == 16ms);
    assert(budget.Remaining(-1ms, start + 1s) == 0ms);
    assert(D3D12::RuntimePsoThreadCount(false) == 2);
    assert(D3D12::RuntimePsoThreadCount(true) == 3);

    // All three compiler jobs enter before any is released: no shared compiler lock.
    Pool pool{D3D12::RuntimePsoThreadCount(true)};
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::promise<void> started[3];
    for (int key = 0; key < 3; ++key) {
        assert(pool.Submit(key, [&, key] {
            started[key].set_value();
            gate.wait();
            return std::make_shared<int>(key);
        }));
    }
    for (auto& signal : started)
        assert(signal.get_future().wait_for(2s) == std::future_status::ready);
    budget.Reset();
    const auto before = PsoFrameBudget::Clock::now();
    for (int key = 0; key < 3; ++key)
        assert(pool.WaitFor(key, budget.Remaining(5s)).status == Pool::Status::Pending);
    // Scheduler tolerance; the old per-request budget would spend fifteen seconds here.
    assert(PsoFrameBudget::Clock::now() - before < 500ms);
    assert(budget.Remaining(5s) == 0ms);
    release.set_value();
    for (int key = 0; key < 3; ++key) {
        const auto ready = pool.WaitFor(key, 2s);
        assert(ready.status == Pool::Status::Ready && *ready.result == key);
    }
}
"""


class PsoFrameBudgetTests(unittest.TestCase):
    def test_deadline_and_parallel_workers(self):
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            source = Path(directory) / "budget.cpp"
            binary = Path(directory) / "budget"
            source.write_text(DRIVER)
            result = subprocess.run(
                [
                    os.environ.get("CXX", "clang++"),
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
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

    def test_runtime_wiring(self):
        source = (D3D12 / "d3d12_pipeline_cache.cpp").read_text()
        self.assertIn('BisectEnabled("NXBOX_D3D12_PSO_FRAME_BUDGET")', source)
        self.assertIn('BisectEnabled("NXBOX_D3D12_PSO_PARALLEL_DXC")', source)
        self.assertIn("pso_frame_budget.Remaining(PsoWaitBudget())", source)
        self.assertIn("async->WaitFor(request, PendingPsoWaitBudget())", source)
        self.assertIn("async->WaitFor(async_key, PendingPsoWaitBudget())", source)
        # Graphics, compute and disk warmup must all use the runtime worker policy.
        for signature in (
            "IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(",
            "ComputePipeline* PipelineCache::CurrentComputePipeline()",
            "void PipelineCache::LoadDiskResources(",
        ):
            with self.subTest(signature=signature):
                body = source.split(signature, 1)[1].split("\n}", 1)[0]
                self.assertIn("async = CreateAsyncCache();", body)
                self.assertNotIn("std::make_unique<AsyncCache>", body)
        factory = source.split("PipelineCache::CreateAsyncCache()", 1)[1].split("\n}", 1)[0]
        self.assertIn("std::make_unique<AsyncCache>(RuntimePsoThreads(),", factory)
        self.assertNotIn("SetWorkerTarget(2)", source)
        tick = source.split("void PipelineCache::TickFrame()", 1)[1].split("\n}", 1)[0]
        self.assertIn("pso_frame_budget.Reset()", tick)
        rasterizer = (D3D12 / "d3d12_rasterizer.cpp").read_text()
        self.assertIn("default_pipeline_cache->TickFrame()", rasterizer)
        # Protected and RT-history decisions still govern whether a request waits at all.
        self.assertIn("if (!may_skip && pso_waited.insert(request_hash).second)", source)
        self.assertIn("CanSkipPendingGraphicsPsoFromRtHistory(", source)
        self.assertIn('RtHistoryEnabled()', source)

    def test_compute_translation_moves_off_gpu_thread_with_budget(self):
        source = (D3D12 / "d3d12_pipeline_cache.cpp").read_text()
        compute = source.split("ComputePipeline* PipelineCache::CurrentComputePipeline()", 1)[1]
        self.assertIn("skip_global_fallback && !FrameBoundedPsoEnabled()", compute)
        worker = compute.split("auto job =", 1)[1].split("async->Submit(", 1)[0]
        self.assertIn("SkipGlobalFallbackCompute(skip_global_fallback", worker)
        self.assertLess(worker.index("SkipGlobalFallbackCompute("), worker.index("EmitHLSL("))


if __name__ == "__main__":
    unittest.main()
