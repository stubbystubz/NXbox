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
        generated = function(renderer, "    void PresentGenerated(")
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
