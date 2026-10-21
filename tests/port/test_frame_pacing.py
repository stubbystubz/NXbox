# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production pacing policy, submission adapter, settings, and host queue model."""

import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run

ROOT = Path(__file__).resolve().parents[2]


class FramePacingTests(unittest.TestCase):
    def test_estimator_cadence_and_present_commands(self):
        driver = r"""
#include <cassert>
#include <limits>
#include <utility>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_present/frame_pacing_policy.h"
int main() {
    using namespace D3D12::Present;
    for (double seconds : {1./60, 1./120, .021, .041, .5, 0.,
                           std::numeric_limits<double>::quiet_NaN()}) {
        FramePacingPolicy p;
        for (int i=0; i<100; ++i) assert(p.Observe(seconds, false)==1);
    }
    for (double seconds : {.022, .025, 1./30, .040}) {
        FramePacingPolicy p;
        for (unsigned i=1; i<p.Window; ++i) assert(p.Observe(seconds, false)==1);
        assert(p.Observe(seconds, false)==2);
        assert(std::abs(p.median_seconds-seconds)<1e-9);
        p.Completed(true);
        // Generated arrivals never enter the interval-2 path.
        assert(p.Observe(seconds, true)==1);
        assert(p.Observe(.5, false)==1);
        assert(p.count==0);
    }
    FramePacingPolicy jitter;
    for (int i=0; i<100; ++i) assert(jitter.Observe(i%2 ? .023 : .039, false)==1);
    FramePacingPolicy probe;
    for (unsigned i=1; i<probe.Window; ++i) probe.Observe(1./30, false);
    for (unsigned i=0; i<29; ++i) {
        assert(probe.Observe(1./30, false)==2);
        probe.Completed(true);
    }
    assert(probe.Observe(1./30, false)==1);
    assert(probe.Observe(1./60, false)==1); // Recovery to a fast game.
    using Command = std::pair<unsigned, bool>;
    std::vector<Command> commands;
    int response=0;
    auto submit = [&](unsigned sync, bool nonblocking) {
        commands.emplace_back(sync, nonblocking);
        return nonblocking ? response : 0;
    };
    for (int i=0; i<100; ++i) assert(SubmitPaced(1, submit, -1, -2)==0);
    assert(commands==std::vector<Command>(100, {1, false})); // Exact disabled commands.
    commands.clear();
    assert(SubmitPaced(2, submit, -1, -2)==0);
    assert((commands==std::vector<Command>{{2,true}}));
    for (int failure : {-1, -2, -3}) {
        commands.clear(); response=failure;
        const auto status=SubmitPaced(2, submit, -1, -2);
        assert(commands.front()==Command(2,true));
        if (failure==-3) { assert(status==-3 && commands.size()==1); }
        else { assert(status==0 && commands.size()==2 && commands.back()==Command(1,false)); }
    }
    PresentGapSamples gaps;
    assert(gaps.Percentile(50)==0);
    for (int i=1; i<=100; ++i) gaps.Add(i/1000.);
    assert(gaps.Percentile(50)==50 && gaps.Percentile(95)==95);
    for (int i=0; i<300; ++i) gaps.Add(1./30);
    assert(gaps.count==256 && std::abs(gaps.Percentile(95)-1000./30)<1e-9);
    gaps.Clear(); assert(gaps.count==0 && gaps.Percentile(95)==0);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver, Path(directory), extra_flags=["-Wall", "-Wextra", "-Werror"])

    def test_settings_and_environment_precedence(self):
        driver = r"""
#include <cassert>
#include "eden_uwp/renderer_settings.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_pacing_policy.h"
int main(int, char** argv) {
    using namespace EdenXbox::RendererSettings;
    using D3D12::Present::FramePacingEnabled;
    const std::filesystem::path local(argv[1]);
    assert(!ReadFramePacing(local));
    const auto file=local / "eden_settings.txt";
    { std::ofstream out(file); out << "# keep\nframe_generation=1\nframe_pacing=1\nframe_pacing=0\n"; }
    assert(!ReadFramePacing(local));
    for (bool enabled : {true, false, true}) {
        assert(SaveFramePacing(local,enabled));
        assert(ReadFramePacing(local)==enabled && ReadFrameGeneration(local));
        std::ifstream in(file);
        const std::string content{std::istreambuf_iterator<char>(in),{}};
        assert(content==std::string("# keep\nframe_generation=1\nframe_pacing=")+(enabled?"1\n":"0\n"));
        for (const char* invalid : {"", "true", "10", "garbage"})
            assert(FramePacingEnabled(ReadFramePacing(local),invalid)==enabled);
        assert(FramePacingEnabled(ReadFramePacing(local),"1"));
        assert(!FramePacingEnabled(ReadFramePacing(local),"0"));
        assert(FramePacingEnabled(enabled,nullptr)==enabled);
    }
    { std::ofstream out(file); out << "frame_pacing=garbage\n"; }
    assert(!ReadFramePacing(local));
    assert(!SaveFramePacing(file,true));
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver, Path(directory), extra_flags=["-Wall", "-Wextra", "-Werror"])

    def test_host_model_requires_opt_in(self):
        # A two-vsync display queue has capacity 30 fps at 60 Hz. A stable 32 fps
        # producer eventually fills any finite queue, even with nonblocking submit.
        # This is why opt-in, rather than a claim of universally unchanged real fps.
        vsync = 1 / 60
        for slots in (2, 3):

            def model(interval):
                pending = []
                now = 0.0
                for _ in range(600):
                    now += 1 / 32
                    pending = [end for end in pending if end > now]
                    if len(pending) >= slots:
                        now = pending.pop(0)
                    end = max(now, pending[-1] if pending else 0) + interval * vsync
                    pending.append(end)
                return 600 / now

            self.assertLess(model(2), model(1))
            self.assertGreater(model(1), 31.9)

    def test_ui_runtime_and_profile_wiring(self):
        loader = (ROOT / "src/eden_uwp/mesa_window.cpp").read_text()
        self.assertIn('"NXBOX_FRAME_PACING_SETTING"', loader)
        self.assertNotIn('_putenv_s("NXBOX_FRAME_PACING",', loader)
        renderer = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        self.assertIn('GetEnvironmentVariableA("NXBOX_FRAME_PACING",', renderer)
        self.assertIn("frame_generation_enabled || layers.empty()", renderer)
        self.assertIn("GetFrameStatistics(&stats)", renderer)
        profile = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        for field in ("present_gap_p50_ms", "present_gap_p95_ms", "present_gap_samples"):
            self.assertIn(field, profile)
        for lang in ("en", "pt_br"):
            table = (ROOT / f"src/eden_uwp/ui/lang/{lang}.inc").read_text()
            self.assertIn("Text::FramePacingTitle", table)
            self.assertIn("Text::FramePacingHint", table)
