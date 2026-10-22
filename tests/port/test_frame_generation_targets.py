# SPDX-License-Identifier: GPL-3.0-or-later
"""Host execution of target selection, stabilization and title preference persistence."""

import tempfile
import unittest
from pathlib import Path
from test_d3d12_capture import compile_run


class FrameGenerationTargetTests(unittest.TestCase):
    def test_targets_and_hysteresis(self):
        driver = r"""
#include <cassert>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_target.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
int main() {
    using namespace D3D12::Present;
    struct Case { double hz, fps; unsigned output, multiplier; };
    for (auto c : {Case{120,64,120,2}, {120,32,120,4}, {120,24,60,4},
                   {120,11,40,4}, {120,8,30,4}, {60,32,60,2}, {60,24,30,2}, {60,16,30,2}, {60,13,0,0},
                   {59.94,24,30,2}, {119.88,11,40,4}, {0,24,0,0},
                   {60,7,0,0}, {144,24,0,0}}) {
        auto t = SelectFrameGenerationTarget(c.hz,c.fps);
        assert(t.output == c.output && t.multiplier == c.multiplier);
        if (t.output) assert(t.interval * t.output == (c.hz > 100 ? 120 : 60));
    }
    assert(!SelectFrameGenerationTarget(60,std::numeric_limits<double>::quiet_NaN()).output);
    FrameGenerationTargetPolicy p;
    auto run = [&](int n, double fps, double hz=120) {
        for (int i=0;i<n;++i) p.Observe(1/fps,hz);
    };
    run(72,24); assert(p.current.output==60 && p.current.multiplier==4);
    run(48,11); assert(p.current.output==40); // Down within one window.
    run(80,24); assert(p.current.output==40); // No early promotion.
    run(80,24); assert(p.current.output==60);
    run(144,64); assert(p.current.output==60);
    run(300,64); assert(p.current.output==120 && p.current.multiplier==2);
    run(144,24,60); assert(p.current.output==30 && p.current.interval==2);
    p.Observe(1,60); assert(p.stable==0); // Loading pauses do not promote.
    FrameGenerationPolicy policy;
    assert(!policy.CanGenerate(true,1.0/60,0));
    policy.locked=true;
    assert(policy.CanGenerate(true,1.0/60,0));
    assert(!policy.CanGenerate(true,1.0/60,0.3)); // Keep cut rejection.
    assert(FrameGenerationMode(3,"0")==0);
    assert(FrameGenerationMode(0,"auto")==3);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver, Path(directory))

    def test_precedence_and_restore(self):
        driver = r"""
#include <cassert>
#include "eden_uwp/title_settings.h"
int main(int, char** argv) {
    using namespace EdenXbox::TitleSettings;
    const std::filesystem::path local(argv[1]);
    const auto title="0100000000010000";
    assert(DefaultFrameGeneration(title)==3);
    assert(DefaultFrameGeneration("0100152000022000")==2);
    assert(DefaultFrameGeneration("0100000000000000")==0);
    assert(EffectiveFrameGeneration(local,title)==3);
    auto global=local/"eden_settings.txt", game=File(local,title);
    assert(SaveFrameGeneration(global,0));
    assert(EffectiveFrameGeneration(local,title)==0);
    assert(SaveFrameGeneration(game,4));
    assert(EffectiveFrameGeneration(local,title)==4);
    assert(SaveFrameGeneration(game,std::nullopt));
    assert(EffectiveFrameGeneration(local,title)==0);
    assert(SaveFrameGeneration(global,std::nullopt));
    assert(EffectiveFrameGeneration(local,title)==3);
    assert(SaveResolution(game,3));
    assert(SaveFrameGeneration(game,2));
    assert(SaveFrameGeneration(game,3));
    std::ifstream in(game);
    assert(ReadResolution(in)==3);
    assert(EffectiveFrameGeneration(local,title)==3);
    std::istringstream aliases("frame_gen=4x\nframe_generation=bad\n");
    assert(ReadFrameGeneration(aliases)==4);
    assert(!SaveFrameGeneration({},3));
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver, Path(directory))
