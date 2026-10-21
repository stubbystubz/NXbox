# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the adapted stream and production metadata setters without the Windows SDK."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class JadyxPorts(unittest.TestCase):
    def test_stream_and_metadata(self):
        pipeline = (BACKEND / "d3d12_graphics_pipeline.h").read_text()
        iface = (BACKEND / "d3d12_pipeline_iface.h").read_text()
        counts = iface[iface.index("struct StageTableCounts {") :]
        counts = counts[: counts.index("} // namespace")]
        buffers = (BACKEND / "d3d12_buffer_cache.cpp").read_text()
        rasterizer = (BACKEND / "d3d12_rasterizer.cpp").read_text()
        stage_gate = rasterizer.split("    static const bool stage_metadata = [] {", 1)[1].split(
            "    }();", 1
        )[0]
        stage_toggle = (
            "bool StageMetadataEnabled() { static const bool stage_metadata = [] {"
            + stage_gate
            + "}(); return stage_metadata; }"
        )
        harness = r"""
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_uniform_stream.h"
using u32 = uint32_t;
namespace Shader {
struct Descriptor { u32 count; };
struct Info {
    std::vector<Descriptor> constant_buffer_descriptors, texture_descriptors,
        texture_buffer_descriptors, storage_buffers_descriptors, image_buffer_descriptors,
        image_descriptors;
};
u32 NumDescriptors(const std::vector<Descriptor>& entries) {
    u32 result{};
    for (const auto& entry : entries) result += entry.count;
    return result;
}
}
TOGGLE
STAGE_TOGGLE
COUNTS
struct Pipeline {
    std::array<std::optional<Shader::Info>, 5> stage_infos{};
    std::array<StageTableCounts, 5> stage_counts{};
    SETTER
    GETTER
};
struct Slice {
    std::shared_ptr<std::vector<uint8_t>> buffer;
    size_t offset{};
    std::span<uint8_t> mapped_span;
};
int main() {
    D3D12::UniformStream<Slice> stream;
    uint64_t tick = 1;
    size_t allocations = 0, allocated_bytes = 0;
    bool advance = false, reclaim = false;
    auto allocate = [&](size_t size) {
        assert(size);
        if (reclaim) { stream.Clear(); reclaim = false; }
        ++allocations;
        allocated_bytes += size;
        if (advance) { ++tick; advance = false; }
        auto buffer = std::make_shared<std::vector<uint8_t>>(size + 512, 0xa5);
        return Slice{buffer, 512, std::span{*buffer}.subspan(512, size)};
    };
    auto request = [&](size_t size) {
        return stream.Request(size, allocate, [&] { return tick; });
    };
    auto first = request(256);
    std::fill(first.mapped_span.begin(), first.mapped_span.end(), 13);
    for (size_t i = 1; i < 16; ++i) {
        auto part = request(256);
        assert(part.buffer == first.buffer && part.offset == 512 + i * 256);
        std::fill(part.mapped_span.begin(), part.mapped_span.end(), uint8_t(i));
    }
    assert(allocations == 1 && allocated_bytes == 4096);
    auto overflow = request(1);
    assert(overflow.buffer != first.buffer);
    assert(first.mapped_span.front() == 13 && first.mapped_span.back() == 13);
    ++tick;
    auto next = request(255);
    assert(next.buffer != overflow.buffer);
    advance = true;
    ++tick;
    auto submitted = request(256);
    const size_t before = allocations;
    assert(request(256).buffer == submitted.buffer && allocations == before);
    auto large = request(65536);
    assert(large.mapped_span.size() == 65536);
    stream.Clear();
    std::weak_ptr<std::vector<uint8_t>> lease;
    { auto only = request(256); lease = only.buffer; }
    assert(!lease.expired());
    stream.Clear();
    assert(lease.expired());
    // Large/other uploads may invoke the ring's reclamation callback too.
    { auto only = request(256); lease = only.buffer; }
    reclaim = true;
    request(8192);
    assert(lease.expired());
    assert(request(256).mapped_span.size() == 256);
    // Differential bytes for mixed sizes/ticks, with all old GPU leases still live.
    std::mt19937 random(61);
    std::vector<Slice> live;
    std::vector<uint8_t> expected;
    for (int i = 0; i < 4000; ++i) {
        if (i % 11 == 0) ++tick;
        size_t size = 1 + random() % 65536;
        if (i % 2 == 0) size = 1 + random() % 4096;
        auto part = request(size);
        assert((part.offset & 255) == 0 && part.mapped_span.size() == size);
        uint8_t byte = random();
        std::fill(part.mapped_span.begin(), part.mapped_span.end(), byte);
        live.push_back(part);
        expected.push_back(byte);
    }
    for (size_t i = 0; i < live.size(); ++i)
        for (auto byte : live[i].mapped_span) assert(byte == expected[i]);
    const char* stage_env = std::getenv("NXBOX_D3D12_STAGE_METADATA");
    assert(StageMetadataEnabled() == (!stage_env || std::strcmp(stage_env, "0") != 0));
    Pipeline pipeline;
    for (size_t stage = 0; stage < 5; ++stage) {
        auto empty = pipeline.StageCounts(stage);
        assert(empty.cbv == 0 && empty.srv == 0 && empty.uav == 0 && empty.sampler == 0);
    }
    for (int i = 0; i < 2000; ++i) {
        Shader::Info info;
        for (auto* entries : {&info.constant_buffer_descriptors, &info.texture_descriptors,
                             &info.texture_buffer_descriptors, &info.storage_buffers_descriptors,
                             &info.image_buffer_descriptors, &info.image_descriptors})
            for (size_t j = random() % 16; j; --j) entries->push_back({random() % 32});
        const size_t stage = random() % 5;
        pipeline.SetStageInfo(stage, &info);
        auto reference = CountStageTables(info);
        // Mutate the caller: cached counts must follow the owned copy.
        info = {};
        auto cached = StageMetadataEnabled() ? pipeline.StageCounts(stage)
                                             : CountStageTables(*pipeline.stage_infos[stage]);
        assert(std::memcmp(&reference, &cached, sizeof(cached)) == 0);
        assert(CountStageTables(*pipeline.stage_infos[stage]).srv == cached.srv);
    }
    // Run the production default-on environment gate in fresh processes.
    const char* env = std::getenv("NXBOX_D3D12_UNIFORM_STREAM");
    const bool expected_enabled = !env || std::strcmp(env, "0") != 0;
    assert(UniformStreamEnabled() == expected_enabled);
    stream.Clear(); ++tick; allocations = 0;
    for (int i = 0; i < 32; ++i) {
        auto part = UniformStreamEnabled() ? request(256) : allocate(256);
        assert(part.mapped_span.size() == 256);
    }
    assert(allocations == (expected_enabled ? 2 : 32));
    // Structural work benchmark; no assertion on noisy elapsed times.
    stream.Clear(); ++tick; allocations = 0; allocated_bytes = 0;
    for (int i = 0; i < 100000; ++i) request(256);
    std::cout << "100000 x 256-byte uniforms: ring requests 100000 -> " << allocations
              << "; ring bytes (512 alignment) 51200000 -> " << allocated_bytes << '\n';
    assert(allocations == 6250 && allocated_bytes == 25600000);
    volatile uint64_t sink = 0;
    constexpr int iterations = 1000000;
    auto bench = [&](bool cached) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            auto count = cached ? pipeline.StageCounts(i % 5)
                                : CountStageTables(*pipeline.stage_infos[i % 5]);
            sink = sink + count.cbv + count.srv + count.uav + count.sampler;
        }
        return std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - start).count() / iterations;
    };
    std::cout << "stage counts ns/stage: scan=" << bench(false) << " cached=" << bench(true)
              << "; 4000 stream / 2000 metadata differential cases passed\n";
}
"""
        harness = (
            harness.replace("STAGE_TOGGLE", stage_toggle)
            .replace("TOGGLE", function(buffers, "bool UniformStreamEnabled()"))
            .replace("COUNTS", counts)
            .replace("SETTER", function(pipeline, "void SetStageInfo("))
            .replace("GETTER", function(pipeline, "StageTableCounts StageCounts("))
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-jadyx-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(harness)
            for flags in (("-O1", "-fsanitize=address,undefined"), ("-O2",)):
                subprocess.run(
                    [
                        "clang++",
                        "-std=c++20",
                        *flags,
                        "-I",
                        str(ROOT / "src"),
                        str(cpp),
                        "-o",
                        str(binary),
                    ],
                    check=True,
                )
                for value, stage_value in ((None, None), ("0", "1"), ("1", "0"), ("00", "00")):
                    env = dict(os.environ)
                    env.pop("NXBOX_D3D12_UNIFORM_STREAM", None)
                    if value is not None:
                        env["NXBOX_D3D12_UNIFORM_STREAM"] = value
                    env.pop("NXBOX_D3D12_STAGE_METADATA", None)
                    if stage_value is not None:
                        env["NXBOX_D3D12_STAGE_METADATA"] = stage_value
                    subprocess.run([str(binary)], check=True, timeout=60, env=env)

    def test_integration_and_opt_outs(self):
        buffers = (BACKEND / "d3d12_buffer_cache.cpp").read_text()
        mapped = function(buffers, "std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer")
        self.assertIn("UniformStreamEnabled()", mapped)
        self.assertIn("uniform_stream.Request(", mapped)
        self.assertIn(": upload.Request(padded)", mapped)
        self.assertIn("context.CurrentTick()", mapped)
        self.assertIn("begin() + size", mapped)  # Zero padding remains owned.
        toggle = function(buffers, "bool UniformStreamEnabled()")
        self.assertIn('"NXBOX_D3D12_UNIFORM_STREAM"', toggle)
        self.assertIn('std::strcmp(value, "0") != 0', toggle)
        header = (BACKEND / "d3d12_buffer_cache.h").read_text()
        self.assertIn("D3D12_HEAP_TYPE_UPLOAD, [this] { uniform_stream.Clear(); }", header)
        rasterizer = (BACKEND / "d3d12_rasterizer.cpp").read_text()
        self.assertIn('"NXBOX_D3D12_STAGE_METADATA"', rasterizer)
        self.assertIn('std::strcmp(value, "0") != 0', rasterizer)
        self.assertIn("metadata_pipeline ? metadata_pipeline->StageCounts(stage)", rasterizer)
        self.assertIn(": CountStageTables(*info)", rasterizer)


if __name__ == "__main__":
    unittest.main()
