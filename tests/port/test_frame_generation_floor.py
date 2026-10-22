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
    run(28, 100); assert(gate.Multiplier(60) == 2); // Recovery threshold stays on.
    run(27, 100); assert(gate.Multiplier(60) == 2); // Exact close threshold stays on.
    run(26, 100); assert(!gate.Multiplier(60));
    run(27.5, 100); assert(!gate.Multiplier(60)); // Cannot recover below 28.
    run(28, 28); assert(!gate.Multiplier(60)); // Most of the recovery window is still slow.
    run(28, 40); assert(gate.Multiplier(60) == 2); // Exact recovery threshold qualifies.
    for (double fps : {31., 34., 34.8, 40., 45., 49.9}) {
        run(fps, 150);
        assert(gate.Multiplier(60) == 2 && gate.Multiplier(120) == 2);
    }
    for (double fps : {50., 55., 57., 58., 60., 120.}) {
        run(fps, 150);
        assert(!gate.Multiplier(60) && !gate.Multiplier(120));
    }
    run(30, 90); assert(gate.Multiplier(60) == 2);
    now += 4; gate.ObservePresent(now); assert(!gate.Multiplier(60));
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

    def test_short_recovery_window_cannot_override_long_window_failure(self):
        driver = r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h"
int main() {
    D3D12::Present::NativeFrameGenerationGate gate;
    double now = 0;
    gate.ObservePresent(now);
    for (unsigned frame = 0; frame < 60; ++frame) {
        now += 1.0 / 15;
        gate.ObservePresent(now);
        assert(!gate.Multiplier(60));
    }
    for (unsigned frame = 0; frame < 60; ++frame) {
        now += 1.0 / 30;
        gate.ObservePresent(now);
        // The recent two seconds recover first, while the full three-second
        // rate still fails. Reopening here would close again on the next real.
        assert(!gate.Multiplier(60));
    }
    for (unsigned frame = 0; frame < 40; ++frame) {
        now += 1.0 / 30;
        gate.ObservePresent(now);
    }
    assert(gate.Multiplier(60) == 2);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                driver,
                Path(directory),
                extra_flags=["-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"],
            )

    def test_jittery_native_floor_for_200_seconds_and_sustained_low_rate(self):
        driver = r"""

#include "video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h"
#include <cassert>
#include <cmath>
int main() {
  using namespace D3D12::Present;
  for (double refresh : {59.94, 60., 119.88, 120.}) {
    NativeFrameGenerationGate gate, real_only;
    double now = 0;
    unsigned frame = 0, transitions = 0, dips = 0;
    bool enabled = false, dipping = false;
    gate.ObservePresent(now);
    real_only.ObservePresent(now);
    while (now < 200) {
      // Each seven-second cycle contains half a second at 26 native FPS.
      const bool next_dipping = std::fmod(now, 7.0) >= 6.5;
      if (next_dipping && !dipping)
        ++dips;
      dipping = next_dipping;
      const double fps =
          dipping ? 26.0 : 28.0 + static_cast<double>((frame * 17u) % 31u) / 10;
      const double interval = frame == 3000 ? 0.5 : 1.0 / fps;
      // Even invalid generated timestamps must not affect real history.
      gate.ObservePresent(-1, false);
      gate.ObservePresent(now + interval / 2, false);
      now += interval;
      ++frame;
      gate.ObservePresent(now);
      real_only.ObservePresent(now);
      assert(gate.NativeFps() == real_only.NativeFps());
      assert(gate.Multiplier(refresh) == real_only.Multiplier(refresh));
      const bool next_enabled = gate.Multiplier(refresh) != 0;
      if (next_enabled != enabled)
        ++transitions;
      if (now < 2.0 - 1e-8)
        assert(!next_enabled);
      if (now >= 2.1)
        assert(next_enabled);
      enabled = next_enabled;
    }
    assert(dips >= 28 &&
           transitions == 1); // Initial opening only; never closes.
    // Genuine low performance must close an already qualified gate.
    const double slowdown = now;
    while (now - slowdown < 3.1) {
      now += 1.0 / 24;
      gate.ObservePresent(now);
    }
    assert(!gate.Multiplier(refresh));
    // A fresh 24 FPS title cannot qualify, even with synthetic observations.
    NativeFrameGenerationGate low;
    for (unsigned sample = 0; sample <= 4800; ++sample) {
      const double timestamp = static_cast<double>(sample) / 24;
      low.ObservePresent(timestamp);
      low.ObservePresent(timestamp + 1.0 / 48, false);
      assert(!low.Multiplier(refresh));
    }
    assert(std::abs(low.NativeFps() - 24) < 1e-8);
  }
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                driver,
                Path(directory),
                extra_flags=["-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"],
            )

    def test_steady_33_with_hitch_and_noisy_display_statistics(self):
        driver = r"""
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_present/display_refresh.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h"
int main() {
    using namespace D3D12::Present;
    for (double refresh : {59.94, 60., 119.88, 120.}) {
        DisplayRefreshEstimate display;
        NativeFrameGenerationGate gate;
        double now = 10000;
        unsigned outages = 0, recoveries = 0, short_window_rejections = 0;
        bool enabled = false, warmed = false;
        std::uint32_t previous_count = 0;
        double previous_qpc = 0;
        for (unsigned frame = 0; frame < 6600; ++frame) {
            now += frame == 3300 ? 1.2 : 1.0 / 33.0;
            gate.ObservePresent(now);
            // Statistics count/QPC occasionally refer to adjacent vblanks.
            // Reproduce the half-rate and above-60 quotients in the console log.
            const auto vblank = static_cast<std::uint32_t>(std::floor(now * refresh));
            const auto count = vblank - (frame % 99 == 70 ? 1u : 0u);
            const double qpc = static_cast<double>(vblank) / refresh +
                               (frame % 37 == 20 ? 0.0002 : 0.0);
            if (previous_qpc > 0 && qpc > previous_qpc && count > previous_count) {
                const double adjacent_hz = static_cast<double>(count - previous_count) /
                                           (qpc - previous_qpc);
                if (!gate.Interval(adjacent_hz)) ++short_window_rejections;
            }
            previous_count = count;
            previous_qpc = qpc;
            display.Observe(count, qpc);
            const bool next_enabled = gate.Multiplier(display.Hertz()) != 0;
            if (frame == 200) {
                assert(next_enabled);
                warmed = true;
            }
            if (warmed) {
                if (enabled && !next_enabled) ++outages;
                if (!enabled && next_enabled && frame > 200) ++recoveries;
                assert(next_enabled);
                assert(display.Hertz() / gate.Interval(display.Hertz()) <= 60);
            }
            enabled = next_enabled;
        }
        assert(short_window_rejections > 100); // Old estimator repeatedly flaps.
        assert(outages == 0 && recoveries == 0); // One hitch cannot close the robust floor.
        assert(enabled && std::abs(gate.NativeFps() - 33) < 1e-7);
    }
    // A genuine display change still gates; one outlying window does not.
    DisplayRefreshEstimate display;
    display.Observe(0, 0);
    display.Observe(120, 2);
    assert(display.Hertz() == 59.94);
    display.Observe(180, 4);
    assert(display.Hertz() == 59.94);
    display.Observe(300, 6);
    assert(display.Hertz() == 59.94);
    display.Observe(360, 8);
    display.Observe(420, 10);
    NativeFrameGenerationGate gate;
    assert(display.Hertz() == 30 && !gate.Interval(display.Hertz()));
    display.Observe(660, 12);
    display.Observe(900, 14);
    assert(display.Hertz() == 119.88 && gate.Interval(display.Hertz()) == 2);
    display.Reset(); // DXGI disjoint/unavailable: no stale estimate.
    assert(display.Hertz() == 0);
    display.Observe(0, 20);
    display.Observe(288, 22);
    assert(display.Hertz() == 144 && !gate.Interval(display.Hertz()));
    display.Observe(1, 1); // Counter/time restart: require a fresh window.
    display.Observe(121, 3);
    assert(display.Hertz() == 144);
    display.Observe(241, 5);
    assert(display.Hertz() == 59.94);
    display.Observe(0, std::numeric_limits<double>::quiet_NaN());
    assert(display.Hertz() == 0);
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
    assert(burst.Generate(1.0, false)); // Queue ahead; DXGI owns the vblank wait.
    assert(burst.Generate(1.02, false));
    burst.RealReady(); // Real burst at the deadline cancels the stale candidate.
    assert(!burst.Generate(1.02, false));
    burst.Presented(5, 60, true, true); // A stall gives no catch-up credit.
    assert(burst.Generate(5.001, false)); // At most one queued synthetic after a stall.
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
        present = function(renderer, "    void Present(const Layout::FramebufferLayout& layout,")
        self.assertIn(
            "1.0f + static_cast<float>(native_frame_gate.NativeFps() / output_hz)", present
        )
        self.assertNotIn("Interpolate(", idle)
        self.assertLess(
            present.index("frame_generator->Generate("), present.index("context.Submit(")
        )
        self.assertIn("PresentGenerated(pending_generated, rasterizer, index)", idle)

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
        self.assertIn("DropGenerated(Present::FrameGenerationEvent::CancelReal)", present)
        self.assertIn("vblank_schedule.Presented(submitted_at, output_hz, true", present)
        self.assertNotIn("sleep_until", present)
        self.assertIn("swapchain->Present(target_interval, 0)", generated)
        gpu_thread = (ROOT / "src/video_core/gpu_thread.cpp").read_text()
        self.assertIn("state.queue.PopWaitUntil(next, stop_token, deadline)", gpu_thread)
        self.assertLess(
            gpu_thread.index("state.queue.TryPop(next)"),
            gpu_thread.index("renderer.PresentIdle()", gpu_thread.index("if (!have_command)")),
        )
        self.assertIn("native_fps={:.2f} gated={} mode={}x target=60", present)
        self.assertIn("logged_native_mode != effective_frame_generation_mode", present)
        profile = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        self.assertIn('" native_fps={:.2f} gated={}"', profile)

    def test_refresh_estimator_and_output_profile_use_production_paths(self):
        base = ROOT / "src/video_core/renderer_d3d12"
        source = (base / "d3d12_present/frame_generation.cpp").read_text()
        configure = function(source, "unsigned FrameGenerator::Configure(")
        self.assertIn("if (specified_refresh)", configure)
        self.assertIn("impl->refresh_estimate.Observe(", configure)
        self.assertIn("impl->refresh_hz = impl->refresh_estimate.Hertz()", configure)
        self.assertNotIn("impl->refresh_sample", configure)
        renderer = (base / "renderer_d3d12.cpp").read_text()
        record = function(renderer, "    void RecordPresentation()")
        self.assertEqual(record.count("++profile.output_presents"), 1)
        self.assertLess(
            record.index("++profile.output_presents"), record.index("GetFrameStatistics")
        )
        for signature in (
            "    void Present(const Layout::FramebufferLayout& layout,",
            "    void PresentGenerated(",
        ):
            self.assertEqual(function(renderer, signature).count("RecordPresentation();"), 1)
        profile = (base / "d3d12_context.cpp").read_text()
        self.assertIn('" output_fps={:.2f} output_presents={}"', profile)
        self.assertIn("static_cast<double>(output_presents) * 10000000.0 / elapsed", profile)
        self.assertIn("frames = fence_wall = output_presents = 0;", profile)
