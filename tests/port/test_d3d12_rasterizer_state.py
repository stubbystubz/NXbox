# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay production rasterizer bookkeeping and query methods without the Windows SDK."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_d3d12_subresource_feedback import function

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


def fixture():
    rasterizer = (BACKEND / "d3d12_rasterizer.cpp").read_text()
    query = (BACKEND / "d3d12_query_cache.cpp").read_text()
    interface = (BACKEND / "d3d12_pipeline_iface.h").read_text()
    metadata = rasterizer[
        rasterizer.index("    static const bool rasterizer_state_fastpath =") : rasterizer.index(
            "    buffer_cache.SetUniformBuffersState"
        )
    ]
    counter = rasterizer[rasterizer.index("    if (rasterizer_state_fastpath) {") :]
    counter = counter[: counter.index("    if (feedback_info)")]
    functions = "\n".join(
        function(query, signature)
        for signature in (
            "bool RasterizerStateFastpathEnabled()",
            "void QueryCacheRuntime::EnableCounter(",
            "void QueryCacheRuntime::PauseHostConditionalRendering()",
            "void QueryCacheRuntime::ResumeHostConditionalRendering()",
            "void QueryCacheRuntime::EndHostConditionalRendering()",
        )
    )
    shared = (ROOT / "src/video_core/query_cache/query_cache.h").read_text()
    dispatch = function(shared, "void QueryCacheBase<Traits>::CounterEnable(").replace(
        "QueryCacheBase<Traits>", "QueryCache"
    )
    streamer = "\n".join(
        function(query, signature)
        for signature in ("void StartCounter() override", "void PauseCounter() override")
    )
    counts = function(interface, "inline StageTableCounts CountStageTables(")
    return (
        DRIVER.replace("PRODUCTION_DISPATCH", dispatch)
        .replace("PRODUCTION_STREAMER", streamer)
        .replace("PRODUCTION_FUNCTIONS", functions)
        .replace("PRODUCTION_COUNTS", counts)
        .replace("PRODUCTION_METADATA", metadata)
        .replace("PRODUCTION_COUNTER", counter)
    )


DRIVER = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_record_args.h"
using u32 = std::uint32_t;
using u64 = std::uint64_t;
constexpr size_t NUM_GRAPHICS_STAGES = 5;
namespace VideoCommon { enum class QueryType { ZPassPixelCount64, Other }; }
using VideoCommon::QueryType;
constexpr size_t TypeCount = 2;
namespace Shader {
struct Descriptor { u32 count; };
struct Info {
    u32 constant_buffer_mask{};
    std::vector<Descriptor> constant_buffer_descriptors, texture_descriptors;
    std::vector<Descriptor> texture_buffer_descriptors, storage_buffers_descriptors;
    std::vector<Descriptor> image_buffer_descriptors, image_descriptors;
};
u32 NumDescriptors(const std::vector<Descriptor>& descriptors) {
    u32 count = 0;
    for (auto descriptor : descriptors) count += descriptor.count;
    return count;
}
}
namespace D3D12 {
bool RasterizerStateFastpathEnabled();
bool BisectEnabled(const char*) { return false; }
struct StageTableCounts { u32 cbv{}, srv{}, uav{}, sampler{}; };
PRODUCTION_COUNTS
struct Native {
    unsigned refs = 1;
    void AddRef() { ++refs; }
    void Release() { --refs; }
};
struct Resource {
    struct Handle { Native* value; Native* Get() { return value; } } handle;
};
constexpr unsigned D3D12_PREDICATION_OP_EQUAL_ZERO = 0;
constexpr unsigned D3D12_RESOURCE_STATE_PREDICATION = 1;
struct State {
    Native* predicate{};
    u64 offset{};
    unsigned operation{};
    bool operator==(const State&) const = default;
    void SetPredication(Native* buffer, u64 offset_, unsigned op) {
        predicate = buffer; offset = offset_; operation = op;
    }
};
struct Commands {
    RecordStream<>* stream{};
    State immediate;
    unsigned packets{};
    void SetPredication(Native* resource, u64 offset, unsigned op) {
        ++packets;
        if (stream) {
            EncodeCall<[](void* target, Native* resource_, u64 offset_, unsigned op_) {
                static_cast<State*>(target)->SetPredication(resource_, offset_, op_);
            }>(*stream, resource, offset, op);
        } else immediate.SetPredication(resource, offset, op);
    }
};
struct Context {
    D3D12::Commands commands;
    auto* Commands() { return &commands; }
    void Transition(const std::shared_ptr<Resource>&, unsigned) {}
};
struct QueryCacheRuntimeImpl {
    Context context;
    std::array<bool, TypeCount> enabled{};
    bool drawing{}, predicate_set{};
    std::shared_ptr<Resource> predicate;
    unsigned predicate_op{};
    void RequireClosed() {
        if (drawing) throw std::logic_error("D3D12 query operation inside draw");
    }
};
struct QueryCacheRuntime {
    std::unique_ptr<QueryCacheRuntimeImpl> impl = std::make_unique<QueryCacheRuntimeImpl>();
    void EnableCounter(QueryType, bool);
    void PauseHostConditionalRendering();
    void ResumeHostConditionalRendering();
    void EndHostConditionalRendering();
};
PRODUCTION_FUNCTIONS
// The shared query cache dispatches to these native streamers on the baseline path.
struct Streamer {
    virtual ~Streamer() = default;
    virtual void StartCounter() = 0;
    virtual void PauseCounter() = 0;
};
struct CounterStreamer final : Streamer {
    QueryCacheRuntime& runtime;
    explicit CounterStreamer(QueryCacheRuntime& runtime_) : runtime(runtime_) {}
    QueryType type = QueryType::ZPassPixelCount64;
    PRODUCTION_STREAMER
};
using StreamerInterface = Streamer;
struct QueryCache {
    struct Impl { std::array<Streamer*, TypeCount> streamers; } storage;
    Impl* impl = &storage;
    explicit QueryCache(Streamer* streamer) : storage{{streamer, streamer}} {}
    __attribute__((noinline)) void CounterEnable(QueryType, bool);
};
#define UNREACHABLE() std::abort()
PRODUCTION_DISPATCH
using GraphicsPipeline = struct Pipeline;
struct Pipeline {
    const Pipeline* AsNative() const { return nullptr; } // Exercise the fallback seam.
    StageTableCounts StageCounts(size_t stage) const { return CountStageTables(*stages[stage]); }
    std::array<const Shader::Info*, 5> stages{};
    virtual ~Pipeline() = default;
    __attribute__((noinline)) virtual const Shader::Info* StageInfo(size_t stage) const {
        return stages[stage];
    }
};
struct Harness {
    QueryCacheRuntime query_runtime;
    CounterStreamer streamer{query_runtime};
    QueryCache query_cache{&streamer};
    std::array<u32, 5> uniform_masks{};
    std::array<std::array<u32, 18>, 5> uniform_sizes{};
    struct { bool zpass_pixel_count_enable{}; } regs;
    u64 checksum{};
    __attribute__((noinline)) void Draw(Pipeline* pipeline) {
        PRODUCTION_METADATA
        PRODUCTION_COUNTER
        // Exercise all three per-draw stage uses outside descriptor_setup.
        for (unsigned pass = 0; pass < 3; ++pass) {
            for (size_t stage = 0; stage < 5; ++stage) {
                const auto* info = rasterizer_state_fastpath ? stage_infos[stage] : pipeline->StageInfo(stage);
                if (info) checksum += info->constant_buffer_mask;
            }
        }
        checksum += totals.cbv + totals.srv + totals.uav + totals.sampler + has_images;
        query_runtime.ResumeHostConditionalRendering();
        query_runtime.PauseHostConditionalRendering();
    }
};
}
using namespace D3D12;
int main(int argc, char**) {
    const bool fast = RasterizerStateFastpathEnabled();
    const char* setting = std::getenv("NXBOX_D3D12_RASTERIZER_STATE_FASTPATH");
    assert(fast == (!setting || std::strcmp(setting, "0") != 0));
    std::array<Shader::Info, 73> infos;
    std::array<Pipeline, 73> pipelines;
    for (unsigned material = 0; material < 73; ++material) {
        auto& info = infos[material];
        info.constant_buffer_mask = 3 | (1u << (material % 16));
        info.constant_buffer_descriptors = {{1}, {1}, {1}};
        info.texture_descriptors = {{1}, {1}, {material % 4 + 1}};
        if (material % 17 == 0) info.image_descriptors = {{1}};
        if (material % 13 == 0) info.storage_buffers_descriptors = {{2}};
        pipelines[material].stages = {&info, nullptr, material % 3 ? nullptr : &info, nullptr, &info};
    }
    // Compare every resulting uniform byte and query enable to an independent rebuild.
    // Insert clears, predicate changes and command-list resets between draws.
    Harness check;
    Native native;
    auto predicate = std::make_shared<Resource>(); predicate->handle.value = &native;
    for (unsigned draw = 0; draw < 3580; ++draw) {
        auto& pipeline = pipelines[(draw / 8) % 73];
        check.regs.zpass_pixel_count_enable = draw % 19 < 4;
        auto& impl = *check.query_runtime.impl;
        impl.predicate_set = draw % 11 < 3;
        impl.predicate = impl.predicate_set ? predicate : nullptr;
        impl.predicate_op = draw % 2;
        impl.context.commands.immediate = {}; // New command list.
        check.Draw(&pipeline);
        assert(impl.context.commands.immediate == State{}); // Paused after draw.
        assert(impl.enabled[0] == check.regs.zpass_pixel_count_enable);
        for (unsigned stage = 0; stage < 5; ++stage) {
            assert(check.uniform_masks[stage] == (pipeline.stages[stage] ? pipeline.stages[stage]->constant_buffer_mask : 0));
            for (u32 size : check.uniform_sizes[stage]) assert(size == (pipeline.stages[stage] ? 65536u : 0u));
        }
        if (impl.predicate_set) {
            check.query_runtime.ResumeHostConditionalRendering();
            assert((impl.context.commands.immediate == State{&native, 0, impl.predicate_op}));
            // External helpers can clear predication behind the runtime's back.
            impl.context.commands.SetPredication(nullptr, 0, 0);
            check.query_runtime.ResumeHostConditionalRendering();
            assert((impl.context.commands.immediate == State{&native, 0, impl.predicate_op}));
            check.query_runtime.EndHostConditionalRendering();
            assert(impl.context.commands.immediate == State{});
            assert(!impl.predicate_set && !impl.predicate);
        }
    }
    check.query_runtime.impl->drawing = true;
    bool rejected = false;
    try { check.query_runtime.EnableCounter(QueryType::ZPassPixelCount64, false); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    if (argc < 2) return 0;
    Harness run;
    RecordStream<> stream;
    run.query_runtime.impl->context.commands.stream = &stream;
    std::vector<double> samples;
    for (unsigned frame = 0; frame < 130; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned draw = 0; draw < 3580; ++draw) {
            run.regs.zpass_pixel_count_enable = draw % 19 < 4;
            run.Draw(&pipelines[(draw / 8) % 73]);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto packets = run.query_runtime.impl->context.commands.packets;
        stream.Flush();
        State replay;
        for (unsigned i = 0; i < packets; ++i) assert(stream.Consume(&replay));
        assert(replay == State{});
        run.query_runtime.impl->context.commands.packets = 0;
        if (frame >= 10) samples.push_back(std::chrono::duration<double, std::nano>(elapsed).count() / 3580);
    }
    stream.Close();
    State replay;
    assert(!stream.Consume(&replay));
    std::sort(samples.begin(), samples.end());
    std::printf("fast=%d ns/draw=%.2f checksum=%llu\n", fast, samples[60], static_cast<unsigned long long>(run.checksum));
}
"""


class RasterizerStateTests(unittest.TestCase):
    def test_production_state_and_recording(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-rasterizer-state-") as temp:
            source = Path(temp) / "state.cpp"
            source.write_text(fixture())
            for sanitize in (True, False):
                binary = Path(temp) / ("checked" if sanitize else "bench")
                command = [
                    "clang++",
                    "-std=c++20",
                    "-O3",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                ]
                if sanitize:
                    command += ["-fsanitize=address,undefined"]
                compiled = subprocess.run(
                    command + [str(source), "-o", str(binary)],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                outputs = []
                for enabled in ("0", "1", None) if sanitize else ("0", "1"):
                    env = dict(os.environ)
                    if enabled is None:
                        env.pop("NXBOX_D3D12_RASTERIZER_STATE_FASTPATH", None)
                    else:
                        env["NXBOX_D3D12_RASTERIZER_STATE_FASTPATH"] = enabled
                    result = subprocess.run(
                        [str(binary)] + ([] if sanitize else ["benchmark"]),
                        check=True,
                        capture_output=True,
                        text=True,
                        env=env,
                        timeout=30,
                    )
                    if not sanitize:
                        print(result.stdout.strip())
                        outputs.append(result.stdout.split("checksum=")[1].strip())
                if outputs:
                    self.assertEqual(outputs[0], outputs[1])


def benchmark():
    """Measure five process-level samples per mode, alternating execution order."""
    with tempfile.TemporaryDirectory(prefix="nxbox-rasterizer-bench-") as temp:
        source = Path(temp) / "state.cpp"
        binary = Path(temp) / "bench"
        source.write_text(fixture())
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-O3",
                "-I",
                str(ROOT / "src"),
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
        )
        samples = {"0": [], "1": []}
        reference = None
        for repeat in range(5):
            for enabled in ("0", "1") if repeat % 2 == 0 else ("1", "0"):
                result = subprocess.run(
                    [str(binary), "benchmark"],
                    check=True,
                    capture_output=True,
                    text=True,
                    env=dict(os.environ, NXBOX_D3D12_RASTERIZER_STATE_FASTPATH=enabled),
                    timeout=30,
                )
                print(result.stdout.strip())
                checksum = result.stdout.split("checksum=")[1].strip()
                if reference is None:
                    reference = checksum
                assert checksum == reference
                samples[enabled].append(float(result.stdout.split("ns/draw=")[1].split()[0]))
        for enabled, values in samples.items():
            print(
                f"fast={enabled} median={sorted(values)[2]:.2f} ns/draw "
                f"range={min(values):.2f}–{max(values):.2f}"
            )


if __name__ == "__main__":
    if sys.argv[1:] == ["--bench"]:
        benchmark()
    else:
        unittest.main()
