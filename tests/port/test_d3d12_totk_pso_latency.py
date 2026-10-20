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
#include <functional>
#include <string_view>
#include <unordered_set>
#include "video_core/renderer_d3d12/d3d12_async_pso.h"
#include "video_core/renderer_d3d12/d3d12_pso_frame_budget.h"
using namespace std::chrono_literals;
bool BisectEnabled(const char* name) {
    const auto* value = std::getenv(name);
    return value && std::string_view{value} == "1";
}
using D3D12::PsoFrameBudget;
using D3D12::RuntimePsoThreadCount;
constexpr int THREAD_PRIORITY_BELOW_NORMAL = -1;
constexpr int THREAD_PRIORITY_NORMAL = 0;
// Capture the production factory's pool settings and priority hook without Windows APIs.
struct AsyncCache {
    enum class Status { Pending, Missing };
    struct Entry { Status status; };
    Status status = Status::Pending;
    int waits = 0;
    std::chrono::steady_clock::duration last_timeout{};
    Entry WaitFor(int, std::chrono::steady_clock::duration timeout) {
        ++waits;
        last_timeout = timeout;
        return {status};
    }
    using NativeHandle = int;
    using PriorityHook = std::function<void(NativeHandle, int)>;
    static inline int applied_priority = 99;
    static void SetPriority(NativeHandle, int priority) { applied_priority = priority; }
    AsyncCache(size_t threads_, size_t capacity_, std::chrono::seconds stall_,
               size_t maximum_, PriorityHook hook_)
        : threads{threads_}, capacity{capacity_}, stall{stall_}, maximum{maximum_}, hook{hook_} {}
    size_t threads, capacity;
    std::chrono::seconds stall;
    size_t maximum;
    PriorityHook hook;
};
struct PipelineCache {
    using AsyncCache = ::AsyncCache;
    struct Profile {
        bool Enabled() const { return false; }
    };
    struct Context {
        Profile Profile() const { return {}; }
    } context;
    std::unordered_set<int> pso_waited;
    AsyncCache* async = nullptr;
    PsoFrameBudget pso_frame_budget;
    std::chrono::steady_clock::duration PendingPsoWaitBudget();
    std::unique_ptr<AsyncCache> CreateAsyncCache();
    void GraphicsWait(bool may_skip, int request_hash);
    void ComputeWait(int request);
};
// Insert the actual production policy definitions here, without Windows/D3D12 dependencies.
POLICY
int main(int argc, char** argv) {
    assert(argc == 2);
    PipelineCache cache;
    const std::string_view mode{argv[1]};
    assert(NonBlockingPsoEnabled() == (mode == "async"));
    const auto workers = cache.CreateAsyncCache();
    cache.async = workers.get();
    assert(workers->threads == RuntimePsoThreadCount(
        BisectEnabled("NXBOX_D3D12_PSO_PARALLEL_DXC")));
    assert(workers->capacity == 4 && workers->stall == 5s && workers->maximum == 4);
    for (const int priority : {-2, -1, 0, 1}) {
        workers->hook(0, priority);
        const bool promote = mode == "async" && priority == THREAD_PRIORITY_BELOW_NORMAL &&
                             !std::getenv("NXBOX_D3D12_PSO_WORKER_PRIORITY");
        assert(AsyncCache::applied_priority == (promote ? THREAD_PRIORITY_NORMAL : priority));
    }
    // Execute the actual admission/wait/retry prefix from each production skip path.
    // Repeated protected draws and dispatches spend their wait only once per key.
    cache.GraphicsWait(true, 1);
    assert(workers->waits == 0 && cache.pso_waited.empty());
    for (int repeat = 0; repeat < 3; ++repeat) {
        cache.GraphicsWait(false, 1);
        cache.ComputeWait(2);
    }
    assert(workers->waits == 2 && cache.pso_waited.size() == 2);
    if (mode == "async") assert(workers->last_timeout == 0ms);
    workers->status = AsyncCache::Status::Missing;
    for (int repeat = 0; repeat < 2; ++repeat) {
        cache.GraphicsWait(false, 3);
        cache.ComputeWait(4);
    }
    assert(workers->waits == 6 && cache.pso_waited.size() == 2);
    workers->status = AsyncCache::Status::Pending;
    cache.GraphicsWait(false, 3);
    cache.ComputeWait(4);
    assert(workers->waits == 8 && cache.pso_waited.size() == 4);
    cache.pso_frame_budget.Reset();
    if (mode == "compatibility") {
        assert(!FrameBoundedPsoEnabled());
        const auto* custom = std::getenv("NXBOX_D3D12_PSO_WAIT_MS");
        const auto expected = custom ? std::chrono::milliseconds{std::strtol(custom, nullptr, 10)}
                                     : 5s;
        // Repeated requests keep the original per-request budget unless explicitly bounded.
        for (int request = 0; request < 4; ++request)
            assert(cache.PendingPsoWaitBudget() == expected);
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
                "size_t RuntimePsoThreads()",
                "std::unique_ptr<PipelineCache::AsyncCache> PipelineCache::CreateAsyncCache()",
                "std::chrono::steady_clock::duration PipelineCache::PendingPsoWaitBudget()",
            )
        )
        for method, parameters, variables, guard in (
            (
                "GraphicsWait",
                "bool may_skip, int request_hash",
                "const int request = request_hash;",
                "if (!may_skip && pso_waited.insert(",
            ),
            (
                "ComputeWait",
                "int request",
                "const int async_key = request;",
                "if (pso_waited.insert(",
            ),
        ):
            # Exclude only diagnostics and result installation, which require D3D12 objects.
            # Keep the production guard, WaitFor call, budget and Missing retry unchanged.
            prefix = guard + cls.source.split(guard, 1)[1].split("if (trace_wait) {", 1)[0]
            definitions += (
                f"\nvoid PipelineCache::{method}({parameters}) {{\n{variables}\n"
                f"{prefix}\n(void)wait_start;\n}}\n}}\n"
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
        cases = (
            ("compatibility", {}),
            ("compatibility", {"ASYNC_PSO": "0"}),
            ("compatibility", {"ASYNC_PSO": ""}),
            ("compatibility", {"ASYNC_PSO": "true", "PSO_WAIT_MS": "37"}),
            ("bounded", {"PSO_FRAME_BUDGET": "1"}),
            ("async", {"ASYNC_PSO": "1", "PSO_FRAME_BUDGET": "0"}),
            ("async", {"ASYNC_PSO": "1", "PSO_FRAME_BUDGET": "1"}),
            ("async", {"ASYNC_PSO": "1", "PSO_WORKER_PRIORITY": "below_normal"}),
            ("async", {"ASYNC_PSO": "1", "PSO_PARALLEL_DXC": "1"}),
        )
        for mode, overrides in cases:
            with self.subTest(mode=mode, overrides=overrides):
                env = {k: v for k, v in os.environ.items() if not k.startswith("NXBOX_D3D12_")}
                env.update({f"NXBOX_D3D12_{key}": value for key, value in overrides.items()})
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
