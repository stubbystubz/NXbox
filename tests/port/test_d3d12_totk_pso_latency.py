# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the runtime wait policy and cache reuse across separate processes."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


POLICY_DRIVER = r"""
#include <cassert>
#include <cstdlib>
#include <future>
#include <string_view>
#include "video_core/renderer_d3d12/d3d12_async_pso.h"
#include "video_core/renderer_d3d12/d3d12_pso_frame_budget.h"
using namespace std::chrono_literals;
bool BisectEnabled(const char* name) {
    const auto* value = std::getenv(name);
    return value && std::string_view{value} == "1";
}
using D3D12::PsoFrameBudget;
struct PipelineCache {
    PsoFrameBudget pso_frame_budget;
    std::chrono::steady_clock::duration PendingPsoWaitBudget();
};
// Insert the actual production policy definitions here, without Windows/D3D12 dependencies.
POLICY
int main(int argc, char** argv) {
    assert(argc == 2);
    PipelineCache cache;
    const std::string_view mode{argv[1]};
    if (mode == "compatibility") {
        assert(!FrameBoundedPsoEnabled());
        assert(cache.PendingPsoWaitBudget() == 5s);
        return 0;
    }
    assert(FrameBoundedPsoEnabled());
    D3D12::AsyncPsoWorkerPool<int, int> pool{2};
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::promise<void> started[2];
    for (int key = 0; key < 4; ++key) {
        assert(pool.Submit(key, [&, key] {
            if (key < 2) started[key].set_value();
            gate.wait();
            return std::make_shared<int>(key);
        }));
    }
    for (auto& signal : started)
        assert(signal.get_future().wait_for(2s) == std::future_status::ready);
    const auto start = PsoFrameBudget::Clock::now();
    for (int key = 0; key < 4; ++key) {
        const auto timeout = cache.PendingPsoWaitBudget();
        // Same policy for protected/small graphics draws, large draws and compute.
        assert(timeout <= 16ms);
        if (mode == "async") assert(timeout == 0ms);
        assert(pool.WaitFor(key, timeout).status == decltype(pool)::Status::Pending);
    }
    assert(PsoFrameBudget::Clock::now() - start < 250ms);
    assert(cache.PendingPsoWaitBudget() == 0ms);
    if (mode == "bounded") {
        cache.pso_frame_budget.Reset();
        const auto next_frame = cache.PendingPsoWaitBudget();
        assert(next_frame > 0ms && next_frame <= 16ms);
    }
    // A skipped draw does not cancel compilation: all jobs can be installed afterwards.
    release.set_value();
    for (int key = 0; key < 4; ++key) {
        const auto entry = pool.WaitFor(key, 2s);
        assert(entry.status == decltype(pool)::Status::Ready && *entry.result == key);
    }
}
"""

DISK_DRIVER = r"""
#include <cassert>
#include <cstdlib>
#include <string_view>
#include "video_core/renderer_d3d12/d3d12_disk_pipeline_cache.h"
using namespace D3D12;
int main(int argc, char** argv) {
    assert(argc == 3);
    const bool write = std::string_view{argv[2]} == "write";
    DiskCacheIdentity identity{4, 2, 7, 0x1002, 1, 0, 0, write ? 11u : 99u, 123};
    DiskPipelineCache cache{argv[1], 0x0100f2c0115b6000ULL, identity};
    const DiskBytes dxil{1, 2, 3, 4};
    if (write) {
        assert(cache.StoreDxil(0x123, dxil));
        assert(cache.RememberPipeline({DiskRecordKind::Compute, 0x456, {5, 6}, {7}, {8}}));
        // Simulate title termination without a destructor or explicit Flush.
        std::_Exit(0);
    }
    assert(cache.FindDxil(0x123) == dxil);
    int builds = 0;
    cache.StartWarmup([&](const DiskPipelineRecord& record, std::stop_token) {
        assert(record.key == 0x456 && record.payload == DiskBytes({5, 6}));
        assert(record.request_key == DiskBytes({7}));
        ++builds;
        return true;
    }, {});
    cache.WaitWarmup();
    assert(builds == 1);
}
"""


class TotkPsoLatencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-totk-pso-")
        cls.directory = Path(cls.temp.name)
        cls.source = (D3D12 / "d3d12_pipeline_cache.cpp").read_text()
        definitions = "\n".join(
            function(cls.source, signature)
            for signature in (
                "std::chrono::milliseconds PsoWaitBudget()",
                "bool NonBlockingPsoEnabled()",
                "bool FrameBoundedPsoEnabled()",
                "std::chrono::steady_clock::duration PipelineCache::PendingPsoWaitBudget()",
            )
        )
        for name, driver, extra in (
            ("policy", POLICY_DRIVER.replace("\nPOLICY\n", "\n" + definitions + "\n"), []),
            ("disk", DISK_DRIVER, [str(D3D12 / "d3d12_disk_pipeline_cache.cpp")]),
        ):
            source = cls.directory / f"{name}.cpp"
            source.write_text(driver)
            subprocess.run(
                [
                    os.environ.get("CXX", "clang++"),
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-pthread",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    *extra,
                    "-o",
                    str(cls.directory / name),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_wait_modes_and_async_completion(self):
        for mode in ("compatibility", "bounded", "async"):
            with self.subTest(mode=mode):
                env = {k: v for k, v in os.environ.items() if not k.startswith("NXBOX_D3D12_")}
                if mode == "bounded":
                    env["NXBOX_D3D12_PSO_FRAME_BUDGET"] = "1"
                if mode == "async":
                    env["NXBOX_D3D12_ASYNC_PSO"] = "1"
                    env["NXBOX_D3D12_PSO_FRAME_BUDGET"] = "0"
                subprocess.run(
                    [str(self.directory / "policy"), mode], env=env, check=True, timeout=10
                )

    def test_disk_hits_after_process_exit_and_boot_luid_change(self):
        root = self.directory / "local-state/eden/cache/shader"
        for mode in ("write", "read"):
            subprocess.run([str(self.directory / "disk"), str(root), mode], check=True, timeout=10)

    def test_gpu_thread_wiring_and_worker_diagnostics(self):
        source = self.source
        self.assertIn("async->WaitFor(request, PendingPsoWaitBudget())", source)
        self.assertIn("async->WaitFor(async_key, PendingPsoWaitBudget())", source)
        self.assertIn("skip_global_fallback && !FrameBoundedPsoEnabled()", source)
        self.assertEqual(source.count("async = CreateAsyncCache();"), 3)
        self.assertIn("priority = THREAD_PRIORITY_NORMAL", source)
        self.assertIn('!std::getenv("NXBOX_D3D12_PSO_WORKER_PRIORITY")', source)
        self.assertIn("RuntimePsoThreadCount(BisectEnabled", source)
        self.assertEqual(source.count("if (trace || profile_build)"), 4)
        for kind in ("graphics", "compute"):
            self.assertIn(f"kind={kind} driver_builds=", source)
        for metric in ("translation_emit_ms", "dxil_disk_hits", "dxil_disk_misses", "driver_ms"):
            self.assertIn(metric, source)


if __name__ == "__main__":
    unittest.main()
