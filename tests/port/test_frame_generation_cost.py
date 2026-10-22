# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production DXGI refresh polling path with a deterministic host clock."""

import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run
from test_frame_generation import function

ROOT = Path(__file__).resolve().parents[2]
FLAGS = ["-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"]


class FrameGenerationCostTests(unittest.TestCase):
    def test_configuration_caches_mode_and_limits_dxgi_polls(self):
        source = (
            ROOT / "src/video_core/renderer_d3d12/d3d12_present/frame_generation.cpp"
        ).read_text()
        method = function(source, "unsigned FrameGenerator::Configure(")
        # Only replace the clock provider; execute the production control flow.
        method = method.replace("std::chrono::steady_clock::now()", "Clock::now()")
        driver = r"""
#include <cassert>
#include <chrono>
#include <memory>
#include <string_view>
#include "video_core/renderer_d3d12/d3d12_present/display_refresh.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
struct Clock {
    static inline std::chrono::steady_clock::time_point time{};
    static auto now() { return time; }
    static void Advance(unsigned ms) { time += std::chrono::milliseconds{ms}; }
};
struct LARGE_INTEGER { long long QuadPart{}; };
void QueryPerformanceFrequency(LARGE_INTEGER* value) { value->QuadPart = 1000000; }
bool SUCCEEDED(int value) { return value >= 0; }
struct DXGI_SWAP_CHAIN_DESC {
    struct { struct { unsigned Numerator{}, Denominator{}; } RefreshRate; } BufferDesc;
};
struct DXGI_FRAME_STATISTICS {
    unsigned SyncRefreshCount{};
    LARGE_INTEGER SyncQPCTime;
};
struct DXGI_OUTPUT_DESC {};
struct IDXGIOutput { int GetDesc(DXGI_OUTPUT_DESC*) { return 0; } };
template <typename T> struct ComPtr {
    T value;
    T** operator&() { return nullptr; }
    T* operator->() { return &value; }
};
struct IDXGISwapChain3 {
    unsigned descriptions{}, statistics{};
    unsigned numerator{};
    bool available{true};
    int GetDesc(DXGI_SWAP_CHAIN_DESC* desc) {
        ++descriptions;
        desc->BufferDesc.RefreshRate = {numerator, numerator ? 1u : 0u};
        return 0;
    }
    int GetFrameStatistics(DXGI_FRAME_STATISTICS* sample) {
        ++statistics;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::time.time_since_epoch()).count();
        sample->SyncRefreshCount = static_cast<unsigned>(us * 60 / 1000000);
        sample->SyncQPCTime.QuadPart = us;
        return available ? 0 : -1;
    }
    int GetContainingOutput(IDXGIOutput**) { return -1; }
};
namespace fmt {
template <typename... Args> std::string_view format(std::string_view text, Args...) { return text; }
}
void Diagnostic(std::string_view) {}
namespace D3D12::Present {
struct FrameGenerator {
    struct Impl {
        bool queried_refresh{}, logged_refresh{};
        double specified_refresh_hz{}, refresh_hz{};
        std::chrono::steady_clock::time_point next_refresh_poll{};
        DisplayRefreshEstimate refresh_estimate;
        FrameGenerationPolicy policy;
        unsigned mode{};
        float fraction{};
    };
    std::unique_ptr<Impl> impl = std::make_unique<Impl>();
    unsigned Configure(IDXGISwapChain3*, unsigned, bool);
};
// METHOD
}
int main() {
    using D3D12::Present::FrameGenerator;
    FrameGenerator generator;
    IDXGISwapChain3 chain;
    for (unsigned frame = 0; frame < 1000; ++frame) {
        assert(generator.Configure(&chain, 2, true) == 2);
        Clock::Advance(10);
    }
    assert(chain.descriptions == 1);
    assert(chain.statistics == 40); // 1000 real frames, only 4 DXGI polls/second.
    assert(generator.impl->refresh_hz > 59 && generator.impl->refresh_hz <= 60);
    const double known = generator.impl->refresh_hz;
    chain.available = false;
    // Between polls, retain the last accepted refresh.
    generator.impl->next_refresh_poll = Clock::now() + std::chrono::milliseconds{250};
    generator.Configure(&chain, 2, true);
    assert(generator.impl->refresh_hz == known);
    Clock::Advance(250);
    generator.Configure(&chain, 2, true);
    assert(generator.impl->refresh_hz == 0); // Disjoint/unavailable must requalify.
    chain.available = true;
    for (unsigned poll = 0; poll < 24; ++poll) {
        Clock::Advance(250);
        generator.Configure(&chain, 2, true);
    }
    assert(generator.impl->refresh_hz > 59);
    assert(chain.descriptions == 1);

    for (unsigned refresh : {60u, 120u, 144u}) {
        FrameGenerator fixed;
        IDXGISwapChain3 specified;
        specified.numerator = refresh;
        for (unsigned frame = 0; frame < 1000; ++frame) {
            fixed.Configure(&specified, 2, false);
            Clock::Advance(10);
        }
        assert(fixed.impl->refresh_hz == refresh);
        assert(fixed.impl->mode == 2); // Native gate owns display eligibility.
        assert(specified.descriptions == 1 && specified.statistics == 0);
    }
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver.replace("// METHOD", method), Path(directory), extra_flags=FLAGS)

    def test_optional_output_never_waits_or_restores_a_real_buffer(self):
        source = (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text()
        idle = function(source, "    void PresentIdle(")
        generated = function(source, "    bool PresentGenerated(")
        self.assertNotIn("WaitSubmitted", idle + generated)
        self.assertNotIn("frame_generator->Current()", generated)
        self.assertEqual(generated.count("swapchain->Present("), 1)
        self.assertIn("DXGI_PRESENT_DO_NOT_WAIT", generated)
        self.assertLess(idle.index("context.IsFree"), idle.index("PresentGenerated("))
        self.assertIn("CancelBackpressure", idle)
        profile = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        self.assertIn("fg_configure_ms={:.3f} fg_prepare_ms={:.3f}", profile)
