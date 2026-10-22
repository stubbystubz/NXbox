# SPDX-License-Identifier: GPL-3.0-or-later
"""Host regressions for busy-queue presentation and DXGI profile accounting."""

import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run
from test_frame_generation import function

ROOT = Path(__file__).resolve().parents[2]
FLAGS = ["-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"]


class FrameGenerationPresentationTests(unittest.TestCase):
    def test_real_generated_real_policy_at_steady_and_bimodal_29_7_fps(self):
        source = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        present = function(source, "    void Present(const Layout::FramebufferLayout& layout,")
        policy_start = present.index("                effective_frame_generation_mode =")
        policy_end = present.index("                frame_generator->UseMotion", policy_start)
        driver = (ROOT / "tests/port/fixtures/frame_generation_presenter.cpp").read_text()
        driver = driver.replace("// POLICY", present[policy_start:policy_end])
        driver = driver.replace("// OBSERVATION", function(source, "    void ObserveNativeFrame()"))
        driver = driver.replace("// GENERATED", function(source, "    bool PresentGenerated("))
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver, Path(directory), extra_flags=FLAGS)

    def test_busy_queue_services_deadline_and_ready_real_wins(self):
        source = (ROOT / "src/video_core/gpu_thread.cpp").read_text()
        start = source.index("            // Service a due output slot")
        end = source.index("            bool have_command = true;", start)
        prelude = source[start:end]
        driver = r"""
#include <cassert>
#include <chrono>
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h"
using Clock = std::chrono::steady_clock;
struct Renderer {
    D3D12::Present::FrameGenerationVblankSchedule schedule;
    Clock::time_point now;
    bool real_ready{};
    unsigned generated{};
    Clock::time_point PresentationDeadline() const {
        const double deadline = schedule.Deadline();
        if (real_ready || deadline == 0) return Clock::time_point::max();
        return Clock::time_point{std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>{deadline})};
    }
    void PresentIdle() {
        const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
        if (schedule.Generate(seconds, real_ready)) {
            ++generated;
            schedule.Presented(seconds, 60, false);
        }
    }
};
int main() {
    Renderer renderer;
    // No empty-queue event for 1000 commands. Execute the production loop prelude.
    renderer.now = Clock::now() - std::chrono::seconds{2};
    const double base = std::chrono::duration<double>(renderer.now.time_since_epoch()).count();
    renderer.schedule.Presented(base, 60, true, true);
    for (unsigned command = 0; command < 1000; ++command) {
        renderer.now += std::chrono::milliseconds{1};
        // PRELUDE
    }
    assert(renderer.generated == 1); // Previous empty-queue-only loop produces zero.
    renderer.schedule.Presented(base, 60, true, true);
    renderer.real_ready = true;
    // PRELUDE
    assert(renderer.generated == 1); // Ready real still wins despite the expired slot.
    renderer.real_ready = false;
    renderer.schedule.RealReady(); // New real cancels the candidate.
    // PRELUDE
    assert(renderer.generated == 1);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver.replace("// PRELUDE", prelude), Path(directory), extra_flags=FLAGS)

    def test_prepared_idle_candidate_queues_before_busy_guest_and_counts_cancels(self):
        source = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        methods = "\n".join(
            function(source, signature)
            for signature in (
                "    void CountGeneration(",
                "    void DropGenerated(",
                "    void PresentIdle(",
            )
        )
        driver = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <memory>
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_events.h"
namespace D3D12 {
using ResourcePtr = std::shared_ptr<int>;
using UINT = unsigned;
struct RasterizerD3D12 {};
struct Profile {
    Present::FrameGenerationEvents frame_generation;
    bool enabled = true;
    bool Enabled() const { return enabled; }
};
struct Context {
    struct Profile profile;
    std::atomic<bool>* ready{};
    bool interrupt{};
    struct Profile& Profile() { return profile; }
    bool available{true};
    bool IsFree(unsigned) {
        if (interrupt) ready->store(true);
        return available;
    }
};
struct Swapchain { unsigned GetCurrentBackBufferIndex() const { return 0; } };
struct Presenter {
    Context context;
    Swapchain chain;
    Swapchain* swapchain = &chain;
    ResourcePtr pending_generated;
    unsigned effective_frame_generation_mode = 2;
    bool frame_generation_failed{};
    Present::FrameGenerationVblankSchedule vblank_schedule;
    std::atomic<bool> real_ready{};
    std::atomic<double> idle_deadline{};
    unsigned present_ticks[1]{};
    double output_hz{60}, now{}, last_scanout{};
    unsigned generated{}, displayed{};
    bool failure{}, busy{}, unsupported{};
    Presenter() { context.ready = &real_ready; }
    double PresentationTime() const { return now; }
    void QueuePresent() {
        // Positive interval = one 60 Hz slot, or two 120 Hz vblanks.
        last_scanout = std::max(last_scanout + 1 / output_hz,
                               std::ceil(now * output_hz) / output_hz);
        ++displayed;
    }
    bool PresentGenerated(const ResourcePtr& image, RasterizerD3D12&, unsigned&) {
        assert(image && *image == 15); // Forward prediction, prepared before guest work.
        if (failure) throw 1;
        if (unsupported) { frame_generation_failed = true; return false; }
        if (busy) return false;
        ++generated;
        QueuePresent();
        now += 0.0004; // GPU prediction/copy has a nonzero cost.
        return true;
    }
    // METHODS
};
}
int main() {
    using namespace D3D12;
    using E = Present::FrameGenerationEvent;
    RasterizerD3D12 rasterizer;
    for (double refresh : {59.94, 60., 119.88, 120.}) {
        Presenter presenter;
        presenter.output_hz = refresh > 100 ? refresh / 2 : refresh;
        const double period = 1 / presenter.output_hz;
        for (unsigned real = 0; real < 300; ++real) {
            presenter.now = static_cast<double>(real) * period * 2;
            presenter.DropGenerated(E::CancelReal);
            presenter.QueuePresent();
            presenter.pending_generated = std::make_shared<int>(15);
            presenter.CountGeneration(E::Candidate);
            presenter.vblank_schedule.Presented(presenter.now, presenter.output_hz, true, true);
            presenter.now += 0.001; // Return from real Present, not an ideal zero-cost slot.
            presenter.PresentIdle(rasterizer);
            // The consumer is busy recording the next guest frame until its arrival.
            // The old deadline (real + period) cannot run before this busy interval.
            assert(!presenter.pending_generated);
            presenter.now += period * 2 - 0.0014;
        }
        assert(presenter.generated == 300 && presenter.displayed == 600);
        assert(std::abs(presenter.last_scanout - 600 * period) < 1e-8);
        const auto& events = presenter.context.profile.frame_generation.counts;
        assert(events[static_cast<std::size_t>(E::Submitted)] == 300);
        assert(events[static_cast<std::size_t>(E::CancelReal)] == 0);
        assert(events[static_cast<std::size_t>(E::DeadlineMiss)] == 0);
    }
    Presenter presenter;
    auto prime = [&] {
        presenter.real_ready = false;
        presenter.context.interrupt = false;
        presenter.now += 1;
        presenter.pending_generated = std::make_shared<int>(15);
        presenter.CountGeneration(E::Candidate);
        presenter.vblank_schedule.Presented(presenter.now, 60, true, true);
        presenter.now += 0.001;
    };
    prime(); presenter.DropGenerated(E::CancelReal);
    presenter.DropGenerated(E::CancelReal); // A candidate has exactly one terminal reason.
    prime(); presenter.real_ready = true; presenter.PresentIdle(rasterizer);
    prime(); presenter.context.interrupt = true; presenter.PresentIdle(rasterizer);
    prime(); presenter.context.available = false; presenter.PresentIdle(rasterizer);
    assert(!presenter.pending_generated && presenter.idle_deadline.load() == 0);
    presenter.context.available = true;
    prime(); presenter.busy = true; presenter.PresentIdle(rasterizer);
    assert(!presenter.pending_generated && presenter.idle_deadline.load() == 0);
    presenter.busy = false;
    prime(); presenter.DropGenerated(E::CancelHidden);
    prime(); presenter.now += 0.1; presenter.PresentIdle(rasterizer);
    prime(); presenter.failure = true;
    try { presenter.PresentIdle(rasterizer); assert(false); } catch (int) {}
    assert(!presenter.pending_generated && presenter.idle_deadline.load() == 0);
    presenter.failure = false;
    prime(); presenter.unsupported = true; presenter.PresentIdle(rasterizer);
    assert(!presenter.pending_generated && presenter.idle_deadline.load() == 0);
    const auto& events = presenter.context.profile.frame_generation.counts;
    for (E event : {E::CancelReal, E::CancelReady, E::CancelFence, E::CancelHidden,
                   E::Submitted, E::DeadlineMiss})
        assert(events[static_cast<std::size_t>(event)] == 1);
    assert(events[static_cast<std::size_t>(E::Failure)] == 2);
    assert(events[static_cast<std::size_t>(E::CancelBackpressure)] == 2);
    presenter.context.profile.frame_generation = {}; // Profile window rollover.
    for (auto count : events) assert(count == 0);
    presenter.context.profile.enabled = false;
    prime(); presenter.DropGenerated(E::CancelReal);
    for (auto count : events) assert(count == 0);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver.replace("// METHODS", methods), Path(directory), extra_flags=FLAGS)
        profile = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        self.assertEqual(profile.count("frame_generation = {};"), 3)
        self.assertIn("Present::FrameGenerationEvents::Names[event]", profile)

    def test_dxgi_windows_stale_samples_failures_wrap_and_reset(self):
        driver = r"""
#include <cassert>
#include <cmath>
#include <initializer_list>
#include "video_core/renderer_d3d12/d3d12_present/display_statistics.h"
int main() {
    using D3D12::Present::DisplayStatistics;
    for (unsigned refresh_interval : {1u, 2u}) {
        DisplayStatistics stats;
        stats.Observe(100, 200);
        assert(!stats.Valid() && stats.FramesPerSecond(5) == -1);
        for (unsigned frame = 1; frame <= 300; ++frame) {
            stats.Observe(100 + frame, 200 + frame * refresh_interval);
            stats.Observe(100 + frame, 200 + frame * refresh_interval); // Stale.
        }
        assert(stats.Valid() && stats.presents == 300 && stats.samples == 300);
        assert(stats.refreshes == 300 * refresh_interval);
        assert(stats.FramesPerSecond(5) == 60);
        stats.ClearWindow();
        stats.Observe(437, 200 + 337 * refresh_interval);
        assert(stats.presents == 37 && stats.FramesPerSecond(1) == 37);
        // Present IDs may jump: report the raw delta, never claim unique scanouts.
        assert(stats.samples == 1 && stats.refreshes == 37 * refresh_interval);
        stats.Unavailable();
        stats.Observe(900, 1000);
        assert(!stats.Valid() && stats.FramesPerSecond(1) == -1);
        assert(stats.errors == 1 && stats.presents == 37); // No bridge across failure.
        stats.ClearWindow();
        stats.Observe(901, 1000 + refresh_interval);
        assert(stats.Valid() && stats.presents == 1);
        stats.Observe(1, 1); // Counter restart/disjoint.
        assert(!stats.Valid() && stats.errors == 1);
        stats.ClearWindow();
        stats.Observe(2, 2);
        assert(stats.Valid() && stats.presents == 1);
    }
    DisplayStatistics wrap;
    wrap.Observe(0xfffffffeu, 0xfffffffdu);
    wrap.Observe(1, 3);
    assert(wrap.Valid() && wrap.presents == 3 && wrap.refreshes == 6);
    wrap.Observe(2, 3); // Impossible advancing present with unchanged refresh.
    assert(!wrap.Valid());
    wrap = {}; // Ownership changes must discard both endpoints and window data.
    assert(!wrap.Valid() && wrap.presents == 0 && wrap.errors == 0);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver, Path(directory), extra_flags=FLAGS)

    def test_present_call_kind_counter_executes_production_method(self):
        renderer = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        method = function(renderer, "    void CountPresentCall(")
        driver = r"""
#include <cassert>
struct Profile {
    bool enabled = true;
    unsigned real_present_calls{}, generated_present_calls{};
    bool Enabled() const { return enabled; }
};
struct Context {
    struct Profile profile;
    struct Profile& Profile() { return profile; }
};
struct Presenter {
    Context context;
    // METHOD
};
int main() {
    Presenter presenter;
    presenter.CountPresentCall(false);
    presenter.CountPresentCall(false); // A retry is another API call.
    presenter.CountPresentCall(true);
    assert(presenter.context.profile.real_present_calls == 2);
    assert(presenter.context.profile.generated_present_calls == 1);
    presenter.context.profile.enabled = false;
    presenter.CountPresentCall(true);
    presenter.CountPresentCall(false);
    assert(presenter.context.profile.real_present_calls == 2);
    assert(presenter.context.profile.generated_present_calls == 1);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver.replace("// METHOD", method), Path(directory), extra_flags=FLAGS)

    def test_every_swapchain_call_counts_kind_before_submission(self):
        renderer = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        real = function(renderer, "    void Present(const Layout::FramebufferLayout& layout,")
        paced = function(renderer, "    HRESULT PresentPaced(")
        generated = function(renderer, "    bool PresentGenerated(")
        self.assertEqual(real.count("CountPresentCall(false);"), 2)
        self.assertEqual(paced.count("CountPresentCall(false);"), 1)
        self.assertEqual(generated.count("CountPresentCall(true);"), 1)
        self.assertLess(paced.index("CountPresentCall"), paced.index("swapchain->Present"))
        self.assertLess(generated.index("CountPresentCall"), generated.index("swapchain->Present"))
        record = function(renderer, "    void RecordPresentation()")
        self.assertIn("profile.display_statistics.Unavailable()", record)
        self.assertIn(
            "profile.display_statistics.Observe(stats.PresentCount, stats.PresentRefreshCount)",
            record,
        )
        profile = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        self.assertIn("displayed_fps={:.2f}", profile)
        self.assertIn("dxgi_present_refresh_delta={}", profile)
        self.assertEqual(profile.count("real_present_calls = generated_present_calls = 0;"), 3)
        self.assertEqual(profile.count("display_statistics = {};"), 2)
        self.assertEqual(profile.count("display_statistics.ClearWindow();"), 1)
