# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production policy/settings and freeze the disabled presenter command path."""

import hashlib
import re
import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run

ROOT = Path(__file__).resolve().parents[2]
PRESENT = ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp"


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class FrameGenerationTests(unittest.TestCase):
    def test_settings_override_skip_and_failure_latch(self):
        driver = r"""
#include <cassert>
#include <limits>
#include "eden_uwp/renderer_settings.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
int main(int, char** argv) {
    using namespace EdenXbox::RendererSettings;
    using namespace D3D12::Present;
    const std::filesystem::path local(argv[1]);
    assert(!ReadFrameGeneration(local));
    const auto file = local / "eden_settings.txt";
    { std::ofstream out(file); out << "# keep\nupscaler=bilinear\nframe_generation=1\nframe_generation=0\n"; }
    assert(!ReadFrameGeneration(local));
    for (bool enabled : {true, false, true}) {
        assert(SaveFrameGeneration(local, enabled));
        assert(ReadFrameGeneration(local) == enabled);
        assert(!ReadFsr(local));
        std::ifstream in(file);
        const std::string content{std::istreambuf_iterator<char>(in), {}};
        assert(content == std::string("# keep\nupscaler=bilinear\nframe_generation=") +
                          (enabled ? "1\n" : "0\n"));
    }
    { std::ofstream out(file); out << "frame_generation=garbage\n"; }
    assert(!ReadFrameGeneration(local));
    assert(!SaveFrameGeneration(file, true));
    for (bool preference : {false, true}) {
        assert(FrameGenerationEnabled(preference, nullptr) == preference);
        assert(FrameGenerationEnabled(preference, "") == preference);
        assert(FrameGenerationEnabled(preference, "true") == preference);
        assert(FrameGenerationEnabled(preference, "10") == preference);
        assert(FrameGenerationEnabled(preference, "1"));
        assert(!FrameGenerationEnabled(preference, "0"));
    }
    const auto small = CheaperHistoryExtent(1920, 1080, 1280, 720);
    assert(small.width == 1280 && small.height == 720);
    const auto output = CheaperHistoryExtent(1920, 1080, 3840, 2160);
    assert(output.width == 1920 && output.height == 1080);
    const auto missing = CheaperHistoryExtent(1920, 1080, 0, 0);
    assert(missing.width == 1920 && missing.height == 1080);
    FrameGenerationPolicy policy;
    assert(!policy.CanGenerate(false, 1.0 / 30, 0));
    for (double fps : {25., 30., 32., 54.9})
        assert(policy.CanGenerate(true, 1.0 / fps, 0.1));
    for (double fps : {55., 60., 120.})
        assert(!policy.CanGenerate(true, 1.0 / fps, 0));
    assert(!policy.CanGenerate(true, 0, 0));
    assert(!policy.CanGenerate(true, 1, 0));
    assert(!policy.CanGenerate(true, 1.0 / 30, 0.22));
    assert(!policy.CanGenerate(true, 1.0 / 30, 1));
    assert(!policy.CanGenerate(true, std::numeric_limits<double>::quiet_NaN(), 0));
    assert(!policy.CanGenerate(true, 1.0 / 30, std::numeric_limits<double>::quiet_NaN()));
    FrameGenerationCadence cadence;
    assert(cadence.Observe(1.0 / 30) == 1.0 / 30);
    cadence.Completed(true);
    for (int i = 1; i < 30; ++i) {
        assert(cadence.Observe(1.0 / 30) == 1.0 / 30);
        cadence.Completed(true);
    }
    assert(cadence.Observe(1.0 / 30) == 0); // Periodic unthrottled probe.
    cadence.Completed(false);
    assert(cadence.Observe(1.0 / 60) == 1.0 / 60);
    assert(!policy.CanGenerate(true, cadence.measured_seconds, 0));
    cadence.Completed(false);
    assert(cadence.Observe(1.0 / 30) == 1.0 / 30);
    cadence.Completed(true);
    assert(cadence.Observe(0.3) == 0.3); // Stalls invalidate even a generated interval.
    assert(policy.Fail());
    assert(!policy.Fail());
    assert(!policy.CanGenerate(true, 1.0 / 30, 0));
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(driver, Path(directory), extra_flags=["-Wall", "-Wextra", "-Werror"])

    def test_disabled_command_source_matches_pre_framegen_presenter(self):
        # Remove ONLY the two explicitly guarded enabled branches. The remaining
        # production method must match the pre-feature method, including every
        # Draw/transition/submit/notify/Present command and its ordering.
        source = function(
            PRESENT.read_text(), "    void Present(const Layout::FramebufferLayout& layout,"
        )
        for marker in (
            "        if (frame_generation_enabled && !frame_generation_failed && !layers.empty()) {",
            "        if (frame_generation_enabled)\n            Common::FpsOverlay::Presented();",
        ):
            if marker.endswith("{"):
                start = source.index(marker)
                depth = 1
                end = start + len(marker)
                while depth:
                    depth += (source[end] == "{") - (source[end] == "}")
                    end += 1
                source = source[:start] + source[end:]
            else:
                self.assertEqual(source.count(marker), 1)
                source = source.replace(marker, "")
        source = source.replace("UINT index =", "const UINT index =")
        normalized = re.sub(r"\s+", "", source)
        self.assertEqual(
            hashlib.sha256(normalized.encode()).hexdigest(),
            "0551263b7236f40f67677b35167519e946676baeb0498b5830b2212734f54f1a",
        )
        draw = function(
            PRESENT.read_text(), "    void Draw(const Layout::FramebufferLayout& layout,"
        )
        self.assertEqual(
            hashlib.sha256(re.sub(r"\s+", "", draw).encode()).hexdigest(),
            "557f607200e274663f207b672f06194f934ad7cb381fa4ffd4a3cf882a1e1f27",
        )

    def test_failure_cannot_replace_real_swapchain_image(self):
        source = (
            ROOT / "src/video_core/renderer_d3d12/d3d12_present/frame_generation.cpp"
        ).read_text()
        self.assertIn("if (impl->policy.failed)", source)
        self.assertIn("if (impl->policy.Fail())", source)
        self.assertNotIn("swapchain", source)
        self.assertNotIn("Copy(source,", source)
        presenter = PRESENT.read_text()
        branch = function(presenter, "    void PresentGenerated(")
        self.assertLess(
            branch.index("copy(buffers[index], generated)"),
            branch.index('"Present generated frame"'),
        )
        self.assertLess(
            branch.index('"Present generated frame"'),
            branch.index("copy(buffers[index], frame_generator->Current())"),
        )
        self.assertIn(
            "context.Transition(buffers[index], D3D12_RESOURCE_STATE_RENDER_TARGET)", presenter
        )

    def test_production_exception_boundary_keeps_real_frame_and_logs_once(self):
        source = (
            ROOT / "src/video_core/renderer_d3d12/d3d12_present/frame_generation.cpp"
        ).read_text()
        boundary = function(source, "ResourcePtr FrameGenerator::Generate(")
        driver = r"""
#include <cassert>
#include <memory>
#include <stdexcept>
#include <string_view>
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
namespace D3D12::Present {
using ResourcePtr = std::shared_ptr<int>;
int diagnostics = 0;
void Diagnostic(std::string_view) noexcept { ++diagnostics; }
struct FrameGenerator {
    struct Impl {
        FrameGenerationPolicy policy;
        int calls{}, failure{}, released{};
        void ReleaseResources() noexcept { ++released; }
        ResourcePtr Generate(const ResourcePtr&, double) {
            ++calls;
            if (failure == 1) throw std::runtime_error("shader compilation");
            if (failure == 2) throw std::runtime_error("PSO creation");
            if (failure == 3) throw std::bad_alloc();
            if (failure == 4) throw 4;
            return std::make_shared<int>(99);
        }
    };
    std::unique_ptr<Impl> impl = std::make_unique<Impl>();
    ResourcePtr Generate(const ResourcePtr&, double) noexcept;
};
// BOUNDARY
}
int main() {
    using namespace D3D12::Present;
    for (int failure : {1, 2, 3, 4}) {
        FrameGenerator generator;
        generator.impl->failure = failure;
        auto real = std::make_shared<int>(42);
        diagnostics = 0;
        for (int frame = 0; frame < 10; ++frame) {
            const auto generated = generator.Generate(real, 1.0 / 30);
            assert(!generated);
            assert(*real == 42);
        }
        assert(diagnostics == 1);
        assert(generator.impl->calls == 1);
        assert(generator.impl->released == 1);
    }
    FrameGenerator success;
    auto real = std::make_shared<int>(42);
    assert(*success.Generate(real, 1.0 / 30) == 99);
    assert(*real == 42);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                driver.replace("// BOUNDARY", boundary),
                Path(directory),
                extra_flags=["-Wall", "-Wextra", "-Werror"],
            )

    def test_production_generated_present_order_and_slot_reuse(self):
        body = function(PRESENT.read_text(), "    void PresentGenerated(")
        driver = r"""
#include <array>
#include <cassert>
#include <memory>
#include <string_view>
#include <vector>
using UINT = unsigned;
using ResourcePtr = std::shared_ptr<struct Resource>;
struct Resource {
    int pixel;
    struct Handle { Resource* value; Resource* Get() { return value; } } handle{this};
};
enum State { D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_PRESENT };
enum class FrameCount { ForcedSubmitPresent };
struct Commands {
    void CopyResource(Resource* dst, Resource* src) { dst->pixel = src->pixel; }
};
struct Context {
    struct Commands commands;
    unsigned tick{}, acknowledged{}, waited{};
    void Transition(const ResourcePtr&, State) {}
    void Retain(const ResourcePtr&) {}
    struct Commands* Commands() { return &commands; }
    unsigned Submit(FrameCount) { return ++tick; }
    void Acknowledge(unsigned value) { acknowledged = value; }
    void WaitSubmitted(unsigned value, const char*) { waited = value; }
    void* Device() { return nullptr; }
};
struct Swapchain {
    std::array<ResourcePtr, 2>* buffers;
    unsigned index{};
    std::vector<int> displayed;
    int Present(unsigned sync, unsigned flags) {
        assert(sync == 1 && flags == 0);
        displayed.push_back((*buffers)[index]->pixel);
        index ^= 1;
        return 0;
    }
    unsigned GetCurrentBackBufferIndex() { return index; }
};
struct RasterizerD3D12 { unsigned notifications{}; void NotifyFencesSubmitted() { ++notifications; } };
struct Generator { ResourcePtr real; ResourcePtr Current() { return real; } };
void Check(int value, const char*, void*) { assert(value == 0); }
namespace Common::FpsOverlay { unsigned displayed{}; void Presented() { ++displayed; } }
struct Presenter {
    Context context;
    std::array<ResourcePtr, 2> buffers;
    std::array<unsigned, 2> present_ticks{7, 8};
    Swapchain* swapchain;
    Generator* frame_generator;
// BODY
};
int main() {
    Presenter presenter;
    presenter.buffers = {std::make_shared<Resource>(Resource{20}), std::make_shared<Resource>(Resource{0})};
    // The test resource has a self pointer, so reset after copying the aggregate.
    for (auto& resource : presenter.buffers) resource->handle.value = resource.get();
    Swapchain swapchain{&presenter.buffers, 0, {}};
    Generator generator{std::make_shared<Resource>(Resource{20})};
    generator.real->handle.value = generator.real.get();
    presenter.swapchain = &swapchain;
    presenter.frame_generator = &generator;
    auto generated = std::make_shared<Resource>(Resource{15});
    generated->handle.value = generated.get();
    RasterizerD3D12 rasterizer;
    UINT index = 0;
    presenter.PresentGenerated(generated, rasterizer, index);
    assert(index == 1);
    assert(presenter.context.waited == 8);
    assert(presenter.present_ticks[0] == 1);
    assert(presenter.context.acknowledged == 1 && rasterizer.notifications == 1);
    assert(Common::FpsOverlay::displayed == 1);
    assert(presenter.buffers[1]->pixel == 20);
    swapchain.Present(1, 0);
    assert((swapchain.displayed == std::vector<int>{15, 20}));
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                driver.replace("// BODY", body),
                Path(directory),
                extra_flags=["-Wall", "-Wextra", "-Werror"],
            )

    def test_shader_embedding_and_settings_wiring(self):
        cmake = (ROOT / "src/video_core/renderer_d3d12/d3d12_present/CMakeLists.txt").read_text()
        self.assertIn('file(READ "${CMAKE_CURRENT_SOURCE_DIR}/frame_generation.hlsl"', cmake)
        self.assertIn("frame_generation.cpp", cmake)
        loader = (ROOT / "src/eden_uwp/mesa_window.cpp").read_text()
        self.assertIn('"NXBOX_FRAME_GEN_SETTING"', loader)
        self.assertNotIn('_putenv_s("NXBOX_FRAME_GEN",', loader)
        self.assertIn('GetEnvironmentVariableA("NXBOX_FRAME_GEN",', PRESENT.read_text())
        for lang in ("en", "pt_br"):
            table = (ROOT / f"src/eden_uwp/ui/lang/{lang}.inc").read_text()
            self.assertIn("Text::FrameGenerationTitle", table)
            self.assertIn("Text::FrameGenerationHint", table)
