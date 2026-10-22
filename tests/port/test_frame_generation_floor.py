# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the native-only floor and real-priority 60 Hz slot schedule on the host."""

import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run
from test_frame_generation import ROOT, function


class FrameGenerationFloorTests(unittest.TestCase):
    def test_floor_hysteresis_cap_and_native_only_measurement(self):
        driver = r"""
#include <cassert>
#include <cmath>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
int main() {
    using namespace D3D12::Present;
    NativeFrameGenerationGate gate;
    double now = 10;
    gate.ObservePresent(now);
    auto run = [&](double fps, unsigned count, bool synthetic = false) {
        for (unsigned frame = 0; frame < count; ++frame) {
            if (synthetic) gate.ObservePresent(now + 0.5 / fps, false);
            now += 1 / fps;
            gate.ObservePresent(now);
        }
    };
    run(30, 29); assert(!gate.Multiplier(60)); // No startup speculation.
    run(30, 31, true);
    assert(std::abs(gate.NativeFps() - 30) < 1e-8);
    assert(gate.Multiplier(60) == 2 && gate.Multiplier(120) == 2);
    assert(gate.Interval(60) == 1 && gate.Interval(120) == 2);
    assert(gate.Multiplier(59.94) == 2 && gate.Interval(119.88) == 2);
    assert(!gate.Multiplier(144) && !gate.Multiplier(0));
    assert(!gate.Multiplier(60.1) && !gate.Multiplier(120.1));
    for (double refresh : {59.94, 60., 119.88, 120.}) {
        GeneratedFrameSchedule schedule{gate.Multiplier(refresh)};
        assert(schedule.Count() == 1);
        assert(refresh / gate.Interval(refresh) <= 60);
        // Multiplier is a candidate limit; actual output is slot-limited.
        assert(schedule.Count() <= 1);
    }
    run(28, 40); assert(gate.Multiplier(60) == 2); // Exact off threshold stays on.
    run(28.5, 40); assert(gate.Multiplier(60) == 2); // Stay on in dead band.
    run(27, 40); assert(!gate.Multiplier(60));
    run(28.5, 70); assert(!gate.Multiplier(60)); // Cannot recover below 29.
    run(30, 30); assert(!gate.Multiplier(60)); // Recovery not held a full second yet.
    run(29, 65); assert(gate.Multiplier(60) == 2); // Exact recovery threshold qualifies.
    for (double fps : {31., 34., 34.8, 40., 45., 49.9}) {
        run(fps, 150);
        assert(gate.Multiplier(60) == 2 && gate.Multiplier(120) == 2);
    }
    for (double fps : {50., 55., 57., 58., 60., 120.}) {
        run(fps, 150);
        assert(!gate.Multiplier(60) && !gate.Multiplier(120));
    }
    run(30, 90); assert(gate.Multiplier(60) == 2);
    now += 2; gate.ObservePresent(now); assert(!gate.Multiplier(60));
    run(30, 29); assert(!gate.Multiplier(60)); // Pause invalidates the window.
    run(30, 65); assert(gate.Multiplier(60) == 2);
    gate.ObservePresent(std::numeric_limits<double>::quiet_NaN());
    assert(!gate.Multiplier(60));
    run(30, 95); assert(gate.Multiplier(60) == 2);
    gate.ObservePresent(now - 1); assert(!gate.Multiplier(60));
    NativeFrameGenerationGate low;
    double low_time = 1;
    for (unsigned frame = 0; frame < 150; ++frame) {
        low.ObservePresent(low_time);
        low.ObservePresent(low_time + 1.0 / 50, false);
        low_time += 1.0 / 25;
    }
    assert(std::abs(low.NativeFps() - 25) < 1e-8);
    assert(!low.Multiplier(60)); // Generated output must never manufacture eligibility.
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                driver,
                Path(directory),
                extra_flags=["-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"],
            )

    def test_real_priority_slots_cap_output_and_do_not_manufacture_native_rate(self):
        driver = r"""
#include <cassert>
#include <cmath>
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h"
int main() {
    using namespace D3D12::Present;
    for (double refresh : {59.94, 60., 119.88, 120.}) {
        for (double fps : {27., 30., 34., 34.8, 45., 50., 55.}) {
            NativeFrameGenerationGate gate;
            // Qualify from real arrivals independently of output slots.
            for (unsigned frame = 0; frame < 500; ++frame)
                gate.ObservePresent(static_cast<double>(frame) / fps);
            FrameGenerationVblankSchedule slots;
            const double output = refresh / gate.Interval(refresh);
            unsigned real_count = 0, generated_count = 0, pending_real = 0;
            double next_real = 0, previous_output = -1;
            for (unsigned slot = 0; slot < 600; ++slot) {
                const double timestamp = static_cast<double>(slot) / output;
                while (next_real <= timestamp + 1e-8) {
                    slots.RealReady();
                    ++pending_real;
                    next_real += 1 / fps;
                }
                const bool ready = pending_real != 0;
                // Even an available synthetic candidate cannot displace real work.
                assert(!slots.Generate(timestamp, true));
                bool presented = false;
                if (ready) {
                    --pending_real;
                    ++real_count;
                    slots.Presented(timestamp, output, true, gate.Multiplier(refresh) != 0);
                    presented = true;
                } else if (slots.Generate(timestamp + 1e-10, false)) {
                    ++generated_count;
                    slots.Presented(timestamp, output, false);
                    assert(!slots.Generate(timestamp + 10, false)); // Single-use.
                    presented = true;
                }
                if (presented) {
                    assert(previous_output < 0 || timestamp - previous_output >= 1 / output - 1e-8);
                    previous_output = timestamp;
                }
                assert(real_count + generated_count <= slot + 1);
                assert(generated_count <= real_count);
            }
            if (fps >= 30 && fps < 50) {
                assert(generated_count > 0);
                assert(real_count + generated_count == 600);
            } else {
                assert(generated_count == 0);
            }
        }
    }
    FrameGenerationVblankSchedule burst;
    burst.Presented(1, 60, true, true);
    assert(!burst.Generate(1.01, false)); // No early/same-slot output.
    assert(burst.Generate(1.02, false));
    burst.RealReady(); // Real burst at the deadline cancels the stale candidate.
    assert(!burst.Generate(1.02, false));
    burst.Presented(5, 60, true, true); // A stall gives no catch-up credit.
    assert(!burst.Generate(5.001, false));
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                driver,
                Path(directory),
                extra_flags=["-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"],
            )

    def test_idle_prediction_uses_current_endpoint_and_retains_failure_guards(self):
        source = (
            ROOT / "src/video_core/renderer_d3d12/d3d12_present/frame_generation.cpp"
        ).read_text()
        prediction = function(source, "ResourcePtr FrameGenerator::Interpolate(")
        driver = r"""
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>
#include <string_view>
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
namespace D3D12::Present {
using ResourcePtr = std::shared_ptr<int>;
enum State { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
void Diagnostic(std::string_view) {}
struct FrameGenerator {
    struct Slot { unsigned query_pairs{}; };
    struct Impl {
        Slot slot;
        Slot* active_slot = &slot;
        FrameGenerationPolicy policy;
        float fraction{};
        ResourcePtr previous = std::make_shared<int>(1), current = std::make_shared<int>(2),
            output = std::make_shared<int>(0), external_motion, motion = std::make_shared<int>(3);
        struct Context {
            unsigned retained{};
            void Transition(const ResourcePtr&, State) {}
            void Retain(const ResourcePtr&) { ++retained; }
            void GlobalUavBarrier() {}
        } context;
        unsigned output_width{1280}, output_height{720}, dispatched{};
        bool fail{};
        void Dispatch(Slot& used, unsigned pass, unsigned width, unsigned height) {
            assert(pass == 2 && width == 1280 && height == 720);
            if (fail) throw 1;
            ++used.query_pairs;
            ++dispatched;
        }
    };
    std::unique_ptr<Impl> impl = std::make_unique<Impl>();
    ResourcePtr Interpolate(float fraction) noexcept;
};
// METHOD
}
int main() {
    using namespace D3D12::Present;
    FrameGenerator generator;
    for (float fraction : {0.5f, 1.5f, 1.75f, 2.0f}) {
        assert(generator.Interpolate(fraction));
        assert(generator.impl->fraction == fraction);
    }
    assert(generator.impl->context.retained == 16);
    assert(!generator.Interpolate(1.5f)); // Query budget still applies.
    generator.impl->slot.query_pairs = 0;
    for (float invalid : {0.f, -1.f, 2.01f, std::numeric_limits<float>::quiet_NaN()})
        assert(!generator.Interpolate(invalid));
    generator.impl->active_slot = nullptr;
    assert(!generator.Interpolate(1.5f));
    generator.impl->active_slot = &generator.impl->slot;
    generator.impl->fail = true;
    assert(!generator.Interpolate(1.5f));
    assert(generator.impl->policy.failed);
    assert(!generator.Interpolate(1.5f));
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                driver.replace("// METHOD", prediction),
                Path(directory),
                extra_flags=["-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"],
            )
        shader = (
            ROOT / "src/video_core/renderer_d3d12/d3d12_present/frame_generation.hlsl"
        ).read_text()
        self.assertIn("saturate(interpolation)", shader)
        renderer = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        idle = function(renderer, "    void PresentIdle(RasterizerD3D12& rasterizer)")
        self.assertIn("1.0f + static_cast<float>(native_frame_gate.NativeFps() / output_hz)", idle)
        self.assertIn("PresentGenerated(predicted, rasterizer, index)", idle)

    def test_idle_wait_yields_to_commands_and_shutdown(self):
        driver = r"""
#include <array>
#include <cassert>
#include <chrono>
#include <future>
#include <thread>
#include "common/bounded_threadsafe_queue.h"
int main() {
    using Clock = std::chrono::steady_clock;
    Common::SPSCQueue<unsigned, 4> queue;
    unsigned value = 0;
    assert(!queue.PopWaitUntil(value, {}, Clock::now())); // Empty slot deadline.
    queue.EmplaceWait(7u);
    assert(queue.PopWaitUntil(value, {}, Clock::now())); // Real already ready wins.
    assert(value == 7);
    std::promise<void> entered;
    std::jthread producer([&] {
        entered.get_future().wait();
        queue.EmplaceWait(9u);
    });
    entered.set_value();
    assert(queue.PopWaitUntil(value, {}, Clock::now() + std::chrono::seconds{2}));
    assert(value == 9);
    producer.join();
    std::promise<void> waiting;
    std::jthread consumer([&](std::stop_token token) {
        waiting.set_value();
        assert(!queue.PopWaitUntil(value, token, Clock::now() + std::chrono::hours{1}));
    });
    waiting.get_future().wait();
    consumer.request_stop();
    consumer.join(); // Shutdown must interrupt the idle deadline.
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                driver,
                Path(directory),
                extra_flags=["-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"],
            )

    def test_presenter_uses_gate_for_every_mode_and_profiles_native_rate(self):
        renderer = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        present = function(renderer, "    void Present(const Layout::FramebufferLayout& layout,")
        generated = function(renderer, "    void PresentGenerated(")
        observation = function(renderer, "    void ObserveNativeFrame()")
        self.assertEqual(present.count("ObserveNativeFrame();"), 1)
        self.assertIn("if (!layers.empty())\n            ObserveNativeFrame();", present)
        self.assertNotIn("ObserveNativeFrame", generated)
        self.assertNotIn("ObservePresent", generated)
        self.assertIn("native_frame_gate.ObservePresent(timestamp)", observation)
        self.assertNotIn("previous_generation_wait", renderer)
        self.assertNotIn("target_policy", renderer)
        self.assertNotIn("frame_generation_cadence", renderer)
        self.assertIn("frame_generator->Configure(swapchain.Get(), 2, true)", present)
        self.assertIn("native_frame_gate.Multiplier(refresh)", present)
        self.assertIn("native_frame_gate.Interval(refresh)", present)
        self.assertIn("if (gated) {", present)
        self.assertNotIn("PresentGenerated(", present)
        idle = function(renderer, "    void PresentIdle(RasterizerD3D12& rasterizer)")
        self.assertIn("vblank_schedule.Generate(PresentationTime(), real_ready.load())", idle)
        self.assertLess(idle.index("if (real_ready.load())"), idle.index("PresentGenerated("))
        self.assertIn("vblank_schedule.RealReady()", present)
        self.assertIn("vblank_schedule.Presented(submitted_at, output_hz, true", present)
        self.assertIn("sleep_until(slot)", present)
        self.assertIn("swapchain->Present(target_interval, 0)", generated)
        gpu_thread = (ROOT / "src/video_core/gpu_thread.cpp").read_text()
        self.assertIn("state.queue.PopWaitUntil(next, stop_token, deadline)", gpu_thread)
        self.assertLess(
            gpu_thread.index("state.queue.TryPop(next)"), gpu_thread.index("renderer.PresentIdle()")
        )
        self.assertIn("native_fps={:.2f} gated={} mode={}x target=60", present)
        self.assertIn("logged_native_mode != effective_frame_generation_mode", present)
        profile = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        self.assertIn('" native_fps={:.2f} gated={}"', profile)
