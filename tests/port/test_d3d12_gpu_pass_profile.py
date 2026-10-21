# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production profiler with a delayed GPU and check log consumers."""

import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_d3d12_deferred_recording as recorder_tests

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"
SPEC = importlib.util.spec_from_file_location(
    "gpu_pass_rank", ROOT / "tools/nxbox/gpu_pass_rank.py"
)
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)

MOCK = r"""
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
constexpr int D3D12_QUERY_HEAP_TYPE_TIMESTAMP = 1, D3D12_QUERY_TYPE_TIMESTAMP = 2;
constexpr int D3D12_HEAP_TYPE_READBACK = 3;
constexpr int D3D12_RESOURCE_DIMENSION_BUFFER = 1, DXGI_FORMAT_UNKNOWN = 0;
constexpr int D3D12_TEXTURE_LAYOUT_ROW_MAJOR = 1, D3D12_RESOURCE_FLAG_NONE = 0;
constexpr int D3D12_RESOURCE_STATE_COPY_DEST = 1;
struct D3D12_RESOURCE_DESC {
    int Dimension; u64 Alignment, Width; u32 Height, DepthOrArraySize, MipLevels;
    int Format;
    struct { u32 Count, Quality; } SampleDesc;
    int Layout, Flags;
};
struct D3D12_QUERY_HEAP_DESC { int Type{}; u32 Count{}; };
struct ID3D12QueryHeap { std::vector<u64> stamps; };
template<class T> struct ComPtr {
    std::unique_ptr<T> object;
    T* Get() { return object.get(); }
};
#define IID_PPV_ARGS(ptr) ptr
struct ID3D12Resource { std::vector<std::byte> bytes; };
struct Resource {
    ComPtr<ID3D12Resource> handle;
    std::span<std::byte> mapped;
};
using ResourcePtr = std::shared_ptr<Resource>;
namespace D3D12 {
inline std::vector<std::string> logs;
inline void Diagnostic(std::string_view text) { logs.emplace_back(text); }
inline void Check(int value, const char*, void*) { assert(value == 0); }
struct DeviceMock {
    unsigned allocations{};
    int CreateQueryHeap(D3D12_QUERY_HEAP_DESC* desc, ComPtr<ID3D12QueryHeap>* heap) {
        ++allocations;
        heap->object = std::make_unique<ID3D12QueryHeap>();
        heap->object->stamps.resize(desc->Count); return 0;
    }
};
struct QueueMock {
    unsigned frequency_reads{};
    int GetTimestampFrequency(u64* frequency) {
        ++frequency_reads; *frequency = 1000; return 0;
    }
};
class CommandFacade {
public:
    GpuPassProfile* pass_profile{};
    u64 clock{100};
    unsigned queries{}, resolves{};
    std::vector<std::function<void()>> gpu;
    void EndQuery(ID3D12QueryHeap* heap, int, u32 index) {
        ++queries;
        gpu.emplace_back([=, this] { heap->stamps[index] = clock; });
    }
    void ResolveQueryData(ID3D12QueryHeap* heap, int, u32 first, u32 count,
                          ID3D12Resource* destination, u64 offset) {
        assert(!pass_profile); ++resolves;
        gpu.emplace_back([=] {
            std::memcpy(destination->bytes.data() + offset,
                        heap->stamps.data() + first, count * sizeof(u64));
        });
    }
    void Advance(u64 ticks) { gpu.emplace_back([=, this] { clock += ticks; }); }
    void Execute() { for (auto& call : gpu) call(); gpu.clear(); }
};
class Context {
public:
    DeviceMock device;
    QueueMock queue;
    std::vector<std::function<void()>> completion;
    bool device_ready{true}, queue_ready{true};
    unsigned readback_allocations{}, recording_attempts{};
    DeviceMock* Device() { return device_ready ? &device : nullptr; }
    QueueMock* Queue() { return queue_ready ? &queue : nullptr; }
    ResourcePtr CreateBuffer(u64, int) {
        // Production pooled buffers call Commands() to emit an aliasing barrier.
        // The profiler is constructed before either immediate or deferred lists exist.
        ++recording_attempts;
        throw std::runtime_error("command allocators and recorder are not initialized");
    }
    ResourcePtr CreateResource(const D3D12_RESOURCE_DESC& desc, int heap, int state) {
        assert(device_ready && queue_ready);
        assert(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        assert(desc.Height == 1 && desc.DepthOrArraySize == 1 && desc.MipLevels == 1);
        assert(desc.SampleDesc.Count == 1 && desc.Format == DXGI_FORMAT_UNKNOWN);
        assert(desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && desc.Flags == 0);
        assert(heap == D3D12_HEAP_TYPE_READBACK && state == D3D12_RESOURCE_STATE_COPY_DEST);
        ++readback_allocations;
        const auto size = desc.Width;
        auto resource = std::make_shared<Resource>();
        resource->handle.object = std::make_unique<ID3D12Resource>();
        resource->handle.Get()->bytes.resize(size);
        resource->mapped = resource->handle.Get()->bytes;
        return resource;
    }
    void AfterCompletion(std::function<void()> callback) { completion.push_back(std::move(callback)); }
    void Complete() { for (auto& call : completion) call(); completion.clear(); }
};
} // namespace D3D12
"""

# Minimal format implementation for the production log's {} / {:.Nf} / {:016x} fields.
FMT = r"""
#pragma once
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
namespace fmt {
inline void append(std::ostringstream& out, std::string_view text) { out << text; }
template<class T, class... Args>
void append(std::ostringstream& out, std::string_view text, T value, Args... args) {
    const auto first = text.find('{'), last = text.find('}', first);
    out << text.substr(0, first);
    std::ostringstream item;
    const auto spec = text.substr(first, last-first+1);
    if (spec == "{:016x}") item << std::hex << std::setw(16) << std::setfill('0');
    if (spec == "{:.4f}") item << std::fixed << std::setprecision(4);
    if (spec == "{:.3f}") item << std::fixed << std::setprecision(3);
    item << value; out << item.str();
    append(out, text.substr(last+1), args...);
}
template<class... Args> std::string format(std::string_view text, Args... args) {
    std::ostringstream out; append(out, text, args...); return out.str();
}
}
"""

DRIVER = r"""
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <tuple>
#include <vector>
#define private public
#include "video_core/renderer_d3d12/d3d12_gpu_pass_profile.h"
#undef private
#include "video_core/renderer_d3d12/d3d12_gpu_pass_profile.cpp"
#include <iostream>
using namespace D3D12;
int main() {
    unsetenv("NXBOX_D3D12_GPU_PASS_PROFILE"); assert(!GpuPassProfile::Enabled());
    for (const char* value : {"0", "true", "10", "", "01"}) {
        setenv("NXBOX_D3D12_GPU_PASS_PROFILE", value, 1); assert(!GpuPassProfile::Enabled());
    }
    setenv("NXBOX_D3D12_GPU_PASS_PROFILE", "1", 1); assert(GpuPassProfile::Enabled());
    // Reproduce Context startup: device/queue first, profiler next, command lists later.
    Context context;
    for (const bool missing_device : {true, false}) {
        context.device_ready = !missing_device;
        context.queue_ready = missing_device;
        bool rejected{};
        try { GpuPassProfile premature(context); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected && context.device.allocations == 0);
        assert(context.queue.frequency_reads == 0 && context.readback_allocations == 0);
    }
    context.device_ready = context.queue_ready = true;
    GpuPassProfile profile(context);
    assert(context.recording_attempts == 0 && context.readback_allocations == 1);
    assert(context.device.allocations == 1 && context.queue.frequency_reads == 1);
    assert(profile.impl->readback->mapped.size() == 32 * (2 + 4096 * 2) * sizeof(u64));
    assert(!profile.impl->commands && context.completion.empty());
    // Entry points are safe before any submission has attached a command facade.
    profile.Work(GpuPassKind::Draw);
    profile.Boundary(); profile.Target({}); profile.Label("guest draws");
    profile.EndSubmission();
    CommandFacade commands;
    profile.BeginSubmission(commands);
    profile.Target({1280, 720, 28, 45, 0xabc});
    profile.Work(GpuPassKind::Draw); commands.Advance(3);
    profile.Target({1280, 720, 28, 45, 0xdef}); // First PS hash survives later draws.
    profile.Work(GpuPassKind::Draw); commands.Advance(2);
    profile.Boundary();
    profile.Work(GpuPassKind::Compute); commands.Advance(4);
    profile.Work(GpuPassKind::Compute); commands.Advance(1);
    profile.EndFrame(); profile.EndSubmission();
    assert(!commands.pass_profile && commands.resolves == 1);
    assert(profile.impl->sums.empty()); // No mapped reads before fence completion.
    assert(profile.impl->slots[0].busy);
    commands.Execute(); context.Complete();
    assert(!profile.impl->slots[0].busy);
    assert(profile.impl->frame_gpu_ms == 10 && profile.impl->idle_ms == 0);
    assert(profile.impl->sums.size() == 2);
    auto& draw = profile.impl->sums.at({GpuPassKind::Draw, {1280,720,28,45,0xabc}});
    assert(draw.ms == 5 && draw.draws == 2 && draw.calls == 1);
    commands.clock += 7; // A real gap between submissions, independent of CPU/wall time.
    profile.BeginSubmission(commands);
    profile.Work(GpuPassKind::Copy); commands.Advance(2);
    profile.Label("DrawTexture blit");
    profile.Work(GpuPassKind::Draw); commands.Advance(3);
    profile.Label("guest clears");
    profile.Work(GpuPassKind::Clear); commands.Advance(1);
    profile.EndFrame(); profile.EndSubmission();
    profile.impl->last_log -= std::chrono::seconds(6);
    commands.Execute(); context.Complete();
    assert(logs.size() == 6);
    for (const auto& line : logs) std::cout << line << '\n';
    // Fill all slots without completing: the 33rd sample must skip, never wait or reuse.
    for (unsigned i=0; i<32; ++i) {
        profile.BeginSubmission(commands);
        profile.Work(GpuPassKind::Draw); commands.Advance(1);
        profile.EndSubmission();
    }
    const auto queries = commands.queries;
    profile.BeginSubmission(commands);
    assert(!commands.pass_profile && commands.queries == queries);
    profile.EndSubmission();
    commands.Execute(); context.Complete();
    assert(profile.impl->dropped == 1 && profile.impl->last_end == 0);
    profile.BeginSubmission(commands);
    for (unsigned i=0; i<4097; ++i) {
        profile.Boundary(); profile.Work(GpuPassKind::Draw); commands.Advance(1);
    }
    profile.EndSubmission(); commands.Execute(); context.Complete();
    assert(profile.impl->truncated == 1);
    assert(profile.impl->sums.begin()->second.calls == 32 + 4096);
    // The ranking is capped at 15, while totals include every signature.
    profile.impl->sums.clear();
    profile.impl->frames = 0;
    profile.impl->frame_gpu_ms = profile.impl->idle_ms = 0;
    logs.clear();
    profile.BeginSubmission(commands);
    for (unsigned i=1; i<=20; ++i) {
        profile.Boundary(); profile.Target({640,480,28,0,i});
        profile.Work(GpuPassKind::Draw); commands.Advance(i);
    }
    profile.EndFrame(); profile.EndSubmission();
    profile.impl->last_log -= std::chrono::seconds(6);
    commands.Execute(); context.Complete();
    assert(logs.size() == 16);
    assert(logs.front().find("rank=1 ms=20.0000") != std::string::npos);
    assert(logs[14].find("rank=15 ms=6.0000") != std::string::npos);
    assert(logs.back().find("total_ms=210.0000") != std::string::npos);
}
"""


class GpuPassProfileTests(unittest.TestCase):
    def test_disabled_facade_preserves_commands_and_allocations(self):
        # Existing transport oracle checks immediate/deferred equivalence and allocation counts;
        # its sink methods abort if the disabled path ever invokes profiling.
        recorder_tests.DeferredRecording().run_binary("address,undefined", recorder_tests.source)

    def test_async_pool_timing_and_production_log_format(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-gpu-pass-") as temp:
            root = Path(temp)
            context = root / "video_core/renderer_d3d12/d3d12_context.h"
            context.parent.mkdir(parents=True)
            context.write_text(MOCK)
            (root / "fmt").mkdir()
            (root / "fmt/format.h").write_text(FMT)
            (root / "test.cpp").write_text(DRIVER)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I" + temp,
                    "-I" + str(ROOT / "src"),
                    str(root / "test.cpp"),
                    "-o",
                    str(root / "test"),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run(
                [str(root / "test")], check=False, capture_output=True, text=True, timeout=30
            )
            self.assertEqual(run.returncode, 0, run.stderr)
        lines = run.stdout.splitlines()
        pattern = (
            r"NXBOX GPU_PASS rank=\d+ ms=\d+\.\d{4} "
            r"kind=(draw|compute|copy|clear|blit) draws=\d+\.\d{3} "
            r"rt=\d+x\d+ fmt=\d+ depth=\d+ shader_hash=[0-9a-f]{16} "
            r"calls/frame=\d+\.\d{3}"
        )
        for line in lines[:-1]:
            self.assertRegex(line, "^" + pattern + "$")
        result = ANALYZER.summarize(lines)
        self.assertEqual(result["windows"], 1)
        self.assertEqual(result["malformed_lines"], 0)
        self.assertEqual(result["totals"], {"total_ms": 8, "frame_gpu_ms": 8, "idle_ms": 3.5})
        self.assertEqual(result["passes"][0]["ms"], 2.5)

    def test_integration_is_guarded_and_does_not_wait(self):
        context = (BACKEND / "d3d12_context.cpp").read_text()
        profiler = (BACKEND / "d3d12_gpu_pass_profile.cpp").read_text()
        self.assertIn("if (GpuPassProfile::Enabled())", context)
        self.assertIn("&& !pass_profile", context)
        self.assertNotRegex(profiler, r"\b(?:Wait|WaitSubmitted|Finish|Submit|Collect)\(")
        commands = (BACKEND / "d3d12_commands.h").read_text()
        indirect = commands.split("void ExecuteIndirect(", 1)[1].split("if (!stream)", 1)[0]
        self.assertIn("GpuPassKind::Compute", indirect)
        self.assertIn(
            'profile->Label("image blit")', (BACKEND / "d3d12_blit_image.cpp").read_text()
        )
        self.assertIn("memory_order_release", profiler)
        self.assertIn("memory_order_acq_rel", profiler)

    def test_startup_initializes_profile_after_queue_before_recorders(self):
        context = (BACKEND / "d3d12_context.cpp").read_text()
        startup = context.split("Context::Context() {", 1)[1].split(
            "void Context::InitializeLists()", 1
        )[0]
        queue = startup.index("device->CreateCommandQueue(")
        fence = startup.index("device->CreateFence(")
        profile = startup.index("std::make_unique<GpuPassProfile>(*this)")
        self.assertLess(queue, fence)
        self.assertLess(fence, profile)
        self.assertLess(profile, startup.index("StartRecorder();"))
        self.assertLess(profile, startup.index("InitializeLists();"))

    def test_analyzer_weights_windows_and_handles_truncation(self):
        def sample(ms, shader="abc"):
            return (
                f"prefix NXBOX GPU_PASS rank=1 ms={ms} kind=draw draws=2 "
                f"rt=1280x720 fmt=28 depth=45 shader_hash={shader} calls/frame=1"
            )

        def total(frames):
            return f"NXBOX GPU_PASS total_ms=4 frame_gpu_ms=8 idle_ms=2 frames={frames}"

        result = ANALYZER.summarize([sample(2), total(10), sample(6), total(30), sample(99)])
        self.assertEqual(result["passes"][0]["ms"], 5)
        self.assertEqual(result["frames"], 40)
        self.assertEqual(result["incomplete_pass_lines"], 1)
        self.assertEqual(result["totals"]["frame_gpu_ms"], 8)
        result = ANALYZER.summarize([sample("nan"), sample(-1), sample(1, "bad-hash"), total(1)])
        self.assertEqual(result["malformed_lines"], 3)
        self.assertEqual(result["passes"], [])


if __name__ == "__main__":
    unittest.main()
