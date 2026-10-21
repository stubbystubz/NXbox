# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host draw-cost fixtures: production lookup branches, key hash and recording transport."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_d3d12_pipeline_generation_lookup import function

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


def pipeline_source():
    header = (BACKEND / "d3d12_pipeline_cache.h").read_text()
    source = (BACKEND / "d3d12_pipeline_cache.cpp").read_text()
    current = function(source, "IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline")
    declarations = current[
        current.index("    Pso::GraphicsPsoState state;") : current.index(
            "    const auto compute_request"
        )
    ]
    result = (
        r"""
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <vector>
#include "common/cityhash.h"
#include "video_core/renderer_d3d12/d3d12_state_cache.h"
namespace D3D12 {
bool key_fast = true, lookup_fast = true;
bool PipelineKeyFastpathEnabled() noexcept { return key_fast; }
bool PipelineLookupFastpathEnabled() { return lookup_fast; }
bool StateFastpathEnabled() { return true; }
bool StateVerifyEnabled() { return false; }
KEY
HASH
EQUALITY
struct IGraphicsPipeline { unsigned id{}; };
enum class FrameCount { PrewarmedPsoHits };
struct ProfileData { bool Enabled() const { return false; } void Count(FrameCount) {} };
struct Context { ProfileData data; ProfileData& Profile() { return data; } };
struct Prewarmed { bool contains(const GraphicsPipelineKey&) { return false; } };
struct PipelineCache {
    Context context;
    Prewarmed prewarmed_graphics;
    StateInputCache<GraphicsStateInputs> graphics_state_inputs;
    GraphicsPipelineKey cached_graphics_request{}, last_graphics_request{};
    Pso::TopologyInfo cached_topology{};
    IGraphicsPipeline* last_graphics_pipeline{};
    bool last_graphics_request_current{}, last_graphics_prewarmed{};
    std::unordered_map<GraphicsPipelineKey, IGraphicsPipeline, GraphicsPipelineKeyHash> pipelines;
    IGraphicsPipeline* RememberGraphicsPipeline(const GraphicsPipelineKey&, IGraphicsPipeline*);
    // The unavailable Maxwell conversion is represented by packed captured-style states.
    // Input comparison, generation-hit branch, request storage, normalization, hashing,
    // equality, pointer installation and unordered lookup all use production code.
    IGraphicsPipeline* Lookup(const GraphicsStateInputs& inputs, const Pso::GraphicsPsoState& fixture) {
        const bool reuse = StateFastpathEnabled() && graphics_state_inputs.Matches(inputs);
        HIT
        DECLARATIONS
        if (!reuse || StateVerifyEnabled()) {
            last_graphics_request_current = false;
            state = fixture;
            Pso::ApplyFramebufferFormats(state, inputs.formats);
            Pso::NormalizeGraphicsState(state);
            request = GraphicsPipelineKey{inputs.hashes, state};
            cached_graphics_request = request;
            cached_topology = topology;
            graphics_state_inputs.Store(inputs);
        } else {
            if (&request != &cached_graphics_request)
                request = cached_graphics_request;
            topology = cached_topology;
            state = request.state;
        }
        if (last_graphics_pipeline && request == last_graphics_request) {
            last_graphics_request_current = true;
            return last_graphics_pipeline;
        }
        auto [entry, added] = pipelines.try_emplace(request);
        if (added) entry->second.id = static_cast<unsigned>(pipelines.size());
        return RememberGraphicsPipeline(request, &entry->second);
    }
};
REMEMBER
}
using namespace D3D12;
struct DrawInput { GraphicsStateInputs inputs; Pso::GraphicsPsoState state; };
std::vector<DrawInput> MakeFrame(unsigned run) {
    std::vector<DrawInput> draws;
    for (unsigned i = 0; i < 3580; ++i) {
        unsigned material = (i / run) % 73;
        DrawInput draw{};
        draw.inputs.generation = i / run;
        draw.inputs.hashes[0] = material % 11 + 1;
        draw.inputs.hashes[5] = material % 13 + 1;
        draw.inputs.formats.colors[0] = 28;
        draw.inputs.formats.depth = 45;
        draw.inputs.formats.sample_count = 1;
        draw.inputs.topology = 3;
        draw.inputs.indexed = true;
        auto& state = draw.state;
        state.topology_type = 3; state.fill_mode = 3; state.cull_mode = 2;
        state.depth_enable = 1; state.depth_write = material % 2; state.depth_func = 4;
        for (auto& blend : state.blend) blend = {1, 15, 5, 6, 1, 2, 1, 1};
        for (unsigned j = 0; j < 8; ++j) {
            state.attributes[j].enabled = 1;
            state.attributes[j].slot = j / 4;
            state.attributes[j].offset = j * 4;
            state.attributes[j].dxgi = 2 + material % 4;
        }
        draws.push_back(draw);
    }
    return draws;
}
int main() {
    for (unsigned run : {1u, 8u}) {
        const auto frame = MakeFrame(run);
        std::vector<unsigned> reference;
        std::vector<GraphicsPipelineKey> reference_keys;
        for (unsigned mode : {0u, 1u, 3u, 2u}) {
            lookup_fast = mode >= 2;
            key_fast = mode != 0;
            PipelineCache cache;
            std::vector<double> samples;
            unsigned long long checksum = 0;
            for (unsigned iteration = 0; iteration < 130; ++iteration) {
                auto start = std::chrono::steady_clock::now();
                for (const auto& draw : frame) {
                    auto* result = mode == 3 ? cache.LookupBefore(draw.inputs, draw.state)
                                            : cache.Lookup(draw.inputs, draw.state);
                    checksum += result->id;
                }
                auto elapsed = std::chrono::steady_clock::now() - start;
                if (!iteration) {
                    // Keep exact-output validation outside the measured region.
                    std::vector<unsigned> actual;
                    std::vector<GraphicsPipelineKey> keys;
                    for (const auto& draw : frame) {
                        auto* result = mode == 3 ? cache.LookupBefore(draw.inputs, draw.state)
                                                : cache.Lookup(draw.inputs, draw.state);
                        actual.push_back(result->id);
                        keys.push_back(cache.last_graphics_request);
                    }
                    if (!mode) { reference = actual; reference_keys = keys; }
                    assert(actual == reference);
                    assert(keys == reference_keys);
                }
                if (iteration >= 10)
                    samples.push_back(std::chrono::duration<double, std::nano>(elapsed).count() / frame.size());
            }
            std::sort(samples.begin(), samples.end());
            std::cout << "lookup run=" << run << " mode=" << mode << " ns/draw="
                      << samples[samples.size()/2] << " checksum=" << checksum << '\n';
        }
    }
    GraphicsPipelineKey key{};
    key.state = MakeFrame(1)[17].state;
    for (bool fast : {false, true}) {
        key_fast = fast;
        size_t checksum = 0;
        const GraphicsPipelineKeyHash hash;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 200000; ++i) {
            key.stage_hashes[0] = i;
            checksum ^= hash(key);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        std::cout << "hash fast=" << fast << " key_bytes=" << sizeof(key) << " ns/key="
                  << std::chrono::duration<double, std::nano>(elapsed).count()/200000
                  << " checksum=" << checksum << '\n';
    }
}
""".replace("KEY", function(header, "struct GraphicsPipelineKey {") + ";")
        .replace("HASH", function(header, "struct GraphicsPipelineKeyHash {") + ";")
        .replace("EQUALITY", function(source, "bool GraphicsPipelineKey::operator=="))
        .replace("HIT", function(current, "if (PipelineLookupFastpathEnabled()"))
        .replace("DECLARATIONS", declarations)
        .replace(
            "REMEMBER",
            function(source, "IGraphicsPipeline* PipelineCache::RememberGraphicsPipeline("),
        )
    )

    lookup = function(result, "IGraphicsPipeline* Lookup(")
    # Frozen starting-tree storage path, with the same production generation branch/hash.
    # This distinguishes newly saved copies from fast paths already present on p54.
    before = (
        lookup.replace("Lookup(", "LookupBefore(", 1)
        .replace(
            declarations,
            "    Pso::GraphicsPsoState state;\n    Pso::TopologyInfo topology{};\n    GraphicsPipelineKey request{};\n",
        )
        .replace(
            "            if (&request != &cached_graphics_request)\n                request = cached_graphics_request;",
            "            request = cached_graphics_request;",
        )
    )
    return result.replace(lookup, lookup + "\n" + before)


def recorder_source():
    fixture = (Path(__file__).with_name("d3d12_direct_replay.cpp")).read_text()
    return (
        fixture[: fixture.index("void Benchmark()")]
        + r"""
int main() {
    uint64_t reference{};
    for (bool reserve : {false, true}) {
        RecordStream<> stream{RecordStream<>::chunk_count, true, reserve};
        List list;
        Target target{&list};
        std::vector<double> samples;
        for (unsigned frame = 0; frame < 130; ++frame) {
            auto start = std::chrono::steady_clock::now();
            EncodeFrame(stream, true);
            auto elapsed = std::chrono::steady_clock::now() - start;
            for (unsigned command = 0; command < 3580 * 4; ++command)
                assert(stream.Consume(&target));
            if (frame >= 10)
                samples.push_back(std::chrono::duration<double, std::nano>(elapsed).count()/3580);
        }
        stream.Close();
        assert(!stream.Consume(&target));
        if (!reserve) reference = list.checksum;
        assert(list.checksum == reference);
        std::sort(samples.begin(), samples.end());
        std::cout << "record reserve=" << reserve << " ns/draw=" << samples[60]
                  << " checksum=" << list.checksum << '\n';
    }
}
"""
    )


class DrawCost(unittest.TestCase):
    def compile(self, directory, source, name, sanitize=False, cityhash=False):
        cpp = Path(directory) / (name + ".cpp")
        binary = Path(directory) / name
        cpp.write_text(source)
        command = [
            "clang++",
            "-std=c++20",
            "-O1" if sanitize else "-O3",
            "-pthread",
            "-I" + directory,
            "-I" + str(ROOT / "src"),
            str(cpp),
        ]
        if cityhash:
            command += [str(ROOT / "src/common/cityhash.cpp")]
        if sanitize:
            command += ["-g", "-fsanitize=address,undefined"]
        result = subprocess.run(command + ["-o", str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def run_fixture(self, binary, env=None):
        result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(result.stderr, result.stderr)
        print(result.stdout, end="")

    def test_benchmark(self):
        with tempfile.TemporaryDirectory(prefix="p54-draw-cost-") as directory:
            self.run_fixture(self.compile(directory, pipeline_source(), "pipeline", cityhash=True))
            self.run_fixture(self.compile(directory, recorder_source(), "recorder"))

    def test_exact_d3d12_calls_both_reservation_modes(self):
        # The existing facade fixture compares every replayed D3D12 argument and descriptor
        # content against a raw list, plus COM holds, failures, routes and tiny chunk splits.
        from test_d3d12_deferred_recording import source
        from test_d3d12_command_validation import FMT

        with tempfile.TemporaryDirectory(prefix="p54-call-parity-") as directory:
            (Path(directory) / "fmt").mkdir()
            (Path(directory) / "fmt/format.h").write_text(FMT)
            binary = self.compile(directory, source(), "calls", sanitize=True)
            for value in ("0", "1"):
                env = dict(os.environ, NXBOX_D3D12_RECORD_PACKET_RESERVE=value)
                self.run_fixture(binary, env)


if __name__ == "__main__":
    unittest.main()
