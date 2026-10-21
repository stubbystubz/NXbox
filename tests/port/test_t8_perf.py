# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute profiling, async admission and descriptor recording lifetime on the host."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"
DRIVER = r"""
#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
using namespace D3D12;
using namespace std::chrono_literals;
template<class F> void until(F done) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
int main(int argc, char** argv) {
    assert(argc == 2);
    switch (argv[1][0]) {
    case 't': {
        FrameTiming timing;
        timing.Reset(100);
        timing.Switch(FramePhase::DescriptorWrites, 110);
        timing.Switch(FramePhase::Barriers, 130);
        timing.Switch(FramePhase::DescriptorWrites, 135);
        timing.Switch(FramePhase::DrawSetup, 150);
        timing.Switch(FramePhase::FenceWait, 160);
        timing.Switch(FramePhase::DrawSetup, 160); // Sleeping consumes no CPU.
        timing.Switch(FramePhase::DrawSetup, 180);
        assert(timing.cpu[static_cast<size_t>(FramePhase::DrawSetup)] == 30);
        assert(timing.cpu[static_cast<size_t>(FramePhase::BetweenDraws)] == 10);
        assert(timing.cpu[static_cast<size_t>(FramePhase::DescriptorWrites)] == 35);
        assert(timing.cpu[static_cast<size_t>(FramePhase::Barriers)] == 5);
        assert(timing.cpu[static_cast<size_t>(FramePhase::FenceWait)] == 0);
        std::uint64_t total = 0;
        for (auto time : timing.cpu) total += time;
        assert(total == 80); // Nested phases do not double-count parent CPU.
        timing.Reset(500);
        for (auto time : timing.cpu) assert(time == 0);
        break;
    }
    case 'c': {
        RecordingTableCache<unsigned> views, samplers;
        assert(!views.Find(1, "null"));
        views.Remember(1, "null", 42);
        samplers.Remember(1, "null", 99);
        assert(views.Find(1, "null") == 42);
        assert(samplers.Find(1, "null") == 99);
        assert(!views.Find(1, "different range"));
        const std::string binary{"a\0b", 3};
        views.Remember(1, binary, 73);
        assert(views.Find(1, binary) == 73);
        assert(!views.Find(1, "a"));
        assert(!views.Find(2, "null")); // Submission retires all cached tables.
        views.Remember(2, "null", 7);
        assert(views.Find(2, "null") == 7);
        assert(!views.Find(2, binary));
        break;
    }
    case 'a': {
        std::promise<void> started, release;
        auto gate = release.get_future().share();
        std::atomic<unsigned> compiled{};
        AsyncPsoCache<int, int> cache{true, [](auto) { assert(false); }};
        assert(!cache.HasRequest(1));
        assert(!cache.Request(1, [&] {
            ++compiled;
            started.set_value();
            gate.wait();
            return std::make_shared<int>(123);
        }));
        assert(started.get_future().wait_for(5s) == std::future_status::ready);
        for (int i = 0; i < 100; ++i)
            assert(!cache.Request(1, [] { assert(false); return std::shared_ptr<int>{}; }));
        assert(cache.HasRequest(1));
        release.set_value();
        std::shared_ptr<int> result;
        until([&] { result = cache.Request(1, [] { assert(false); return std::shared_ptr<int>{}; }); return bool(result); });
        assert(*result == 123 && compiled == 1);
        cache.Stop();
        assert(!cache.Request(2, [] { assert(false); return std::shared_ptr<int>{}; }));
        break;
    }
    case 'f': {
        unsigned reports = 0;
        std::string reason;
        AsyncPsoCache<int, int> cache{true, [&](auto message) { ++reports; reason = message; }};
        assert(!cache.Request(1, []() -> std::shared_ptr<int> { throw std::runtime_error("DXC exact reason"); }));
        until([&] { cache.Request(1, [] { assert(false); return std::shared_ptr<int>{}; }); return reports == 1; });
        for (int i = 0; i < 100; ++i) assert(!cache.Request(1, [] { assert(false); return std::shared_ptr<int>{}; }));
        assert(reports == 1 && reason == "DXC exact reason");
        assert(!cache.Request(2, []() -> std::shared_ptr<int> { throw 17; }));
        until([&] { cache.Request(2, [] { assert(false); return std::shared_ptr<int>{}; }); return reports == 2; });
        assert(reason.find("non-standard exception") != std::string::npos);
        assert(!cache.Request(3, [] { return std::shared_ptr<int>{}; }));
        until([&] { cache.Request(3, [] { assert(false); return std::shared_ptr<int>{}; }); return reports == 3; });
        assert(reason.find("no pipeline") != std::string::npos);
        break;
    }
    case 'b': {
        std::promise<void> started, release;
        auto gate = release.get_future().share();
        std::atomic<unsigned> completed{};
        AsyncPsoCache<int, int> cache{true, [](auto) { assert(false); }};
        cache.Request(0, [&] { started.set_value(); gate.wait(); ++completed; return std::make_shared<int>(0); });
        assert(started.get_future().wait_for(5s) == std::future_status::ready);
        for (size_t i = 1; i < cache.MaxPending; ++i)
            cache.Request(static_cast<int>(i), [&] { ++completed; return std::make_shared<int>(1); });
        cache.Request(999, [] { assert(false); return std::shared_ptr<int>{}; });
        assert(!cache.HasRequest(999));
        release.set_value();
        until([&] { return completed == cache.MaxPending; });
        std::shared_ptr<int> result;
        until([&] { result = cache.Request(999, [] { return std::make_shared<int>(999); }); return bool(result); });
        assert(*result == 999); // Full queue skips without poisoning admission.
        break;
    }
    case 's': {
        unsigned reports = 0;
        AsyncPsoCache<int, int> cache{false, [&](auto) { ++reports; }};
        assert(*cache.Request(1, [] { return std::make_shared<int>(9); }) == 9);
        bool thrown = false;
        try { cache.Request(2, []() -> std::shared_ptr<int> { throw std::runtime_error("sync fallback"); }); }
        catch (const std::runtime_error& error) { thrown = std::string{error.what()} == "sync fallback"; }
        assert(thrown && reports == 0);
        break;
    }
    default: assert(false);
    }
}
"""


class T8PerfTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-t8-perf-")
        source = Path(cls.temp.name) / "perf.cpp"
        cls.binary = Path(cls.temp.name) / "perf"
        source.write_text(DRIVER)
        subprocess.run(
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
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, case):
        subprocess.run([str(self.binary), case], check=True, timeout=15)

    def test_exclusive_cpu_accounting(self):
        self.run_case("t")

    def test_immutable_table_recording_lifetime(self):
        self.run_case("c")

    def test_async_skip_deduplicate_publish_and_shutdown(self):
        self.run_case("a")

    def test_async_failures_report_exact_reason_once(self):
        self.run_case("f")

    def test_async_queue_is_bounded_and_retries_admission(self):
        self.run_case("b")

    def test_disabled_async_preserves_synchronous_fallback(self):
        self.run_case("s")

    def test_native_integration_contract(self):
        context = (BACKEND / "d3d12_context.cpp").read_text()
        descriptors = (BACKEND / "d3d12_descriptor.cpp").read_text()
        helpers = (BACKEND / "d3d12_compute_pass.cpp").read_text()
        dispatch = (BACKEND / "d3d12_dispatch.cpp").read_text()
        renderer = (BACKEND / "renderer_d3d12.cpp").read_text()
        self.assertIn("GetThreadTimes(GetCurrentThread()", context)
        self.assertIn("wall - start_wall < 50000000", context)
        self.assertIn("Profile().EndFrame()", renderer)
        self.assertIn("FirstInfoWarning(message->Severity, message->ID)", context)
        reserve = descriptors.split("D3D12DescriptorAllocator::Reserve(")[1].split(
            "D3D12DescriptorAllocator::SetHeaps("
        )[0]
        self.assertNotIn("context.Finish()", reserve)
        self.assertIn("context.Wait(oldest_blocked)", reserve)
        self.assertIn("CopyDescriptorsSimple", descriptors)
        writer = descriptors.split("D3D12DescriptorAllocator::WriteCached(")[1].split(
            "D3D12DescriptorAllocator::FindTable("
        )[0]
        self.assertNotIn("std::function", writer)
        self.assertNotIn("[&](auto handle)", descriptors)
        self.assertNotIn("std::string key", writer)
        self.assertIn("MakeDescriptorKey(resource, kind, desc, size)", writer)
        run = helpers.split("void ComputePasses::Run(")[1]
        self.assertNotIn("CreateDescriptorHeap", run)
        self.assertIn("descriptors.FindTable(view_key)", run)
        self.assertIn("context.TransitionBatch(transitions)", dispatch)
        self.assertEqual(dispatch.count("Count(FrameCount::Dispatches)"), 2)
        for phase in (
            "draw_setup",
            "descriptor_writes",
            "barriers",
            "pso",
            "buffer_upload",
            "texture_upload",
            "submit",
            "fence_wait",
        ):
            self.assertIn(f'"{phase}"', context)


if __name__ == "__main__":
    unittest.main()
