# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Differential sampler-plan reconstruction from production logic under ASan/UBSan."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
start = source.index("    // Cache only immutable sampler keys")
end = source.index("    DescriptorTable view_block", start)
logic = source[start:end]
start = source.index("    if (!reuse_sampler_plan || validate_sampler_plan) {", end)
end = source.index("\n    const auto predicate_trace", start)
finish = source[start:end]
start = source.index("                if (!reuse_sampler_plan || validate_sampler_plan)\n")
end = source.index("\n                // Diagnostic:", start)
write = source[start:end]

prefix = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
namespace Tegra::Texture {
enum class SamplerReduction : u32 { Standard, Min, Max };
struct TSCEntry { SamplerReduction reduction_filter{}; std::array<u32,7> words{}; };
}
using Entry = Tegra::Texture::TSCEntry;
struct DescriptorKey {
    std::array<u8,64> description{};
    uintptr_t resource{};
    u32 kind{}, size{};
    bool operator==(const DescriptorKey&) const = default;
};
struct Plan {
    std::vector<DescriptorKey> keys;
    std::vector<u8> initialized;
    struct { bool valid{true}; } sources;
};
namespace Shader {
struct Desc { u32 count{}; bool is_depth{}; };
struct Info { std::vector<Desc> texture_descriptors; };
}
struct Pipeline {
    std::array<Shader::Info,5> stages;
    const Shader::Info* StageInfo(size_t i) const {
        return stages[i].texture_descriptors.empty() ? nullptr : &stages[i];
    }
};
struct Sampler { Entry entry; const Entry& Entry() const { return entry; } };
struct TextureCache {
    std::vector<Sampler> samplers;
    const Sampler& GetSampler(u32 i) const { return samplers.at(i); }
};
bool validate{}, policy{};
bool BisectEnabled(const char*) { return validate; }
namespace fmt { template<class... T> std::string format(const char*, T...) { return {}; } }
void Diagnostic(const std::string&) {}
constexpr size_t NUM_GRAPHICS_STAGES = 5;
DescriptorKey Build(const Sampler& sampler, bool compare) {
    DescriptorKey key;
    std::memcpy(key.description.data(), &sampler.entry, sizeof(sampler.entry));
    key.description[32] = compare;
    if (sampler.entry.reduction_filter != Tegra::Texture::SamplerReduction::Standard)
        key.description[33] = policy;
    key.kind = 3; key.size = 34;
    return key;
}
struct Table { Plan* plan; u32 count; };
struct Writers {
    u32 writes{};
    void WriteSampler(Table table, u32 i, const Sampler& sampler, bool compares) {
        table.plan->keys.at(i) = Build(sampler, compares);
        table.plan->initialized.at(i) = 1;
        ++writes;
    }
    void WriteNullSampler(Table table, u32 i) { table.plan->keys.at(i) = {}; table.plan->initialized.at(i) = 1; }
} descriptors;
bool Run(Pipeline* pipeline, TextureCache& texture_cache, u32 sampler_size, bool abort = false) {
    std::vector<u32> sampler_scratch;
    for (u32 i=0; i<texture_cache.samplers.size(); ++i) sampler_scratch.push_back(i);
    struct { u32 sampler; } totals{static_cast<u32>(sampler_scratch.size())};
    Plan sampler_plan;
    sampler_plan.keys.resize(sampler_size);
    sampler_plan.initialized.resize(sampler_size);
"""
suffix = r"""
    if (abort) return reuse_sampler_plan;
    u32 smp_i = 0;
    std::vector<DescriptorKey> reference(sampler_size);
    Table sampler_table{&sampler_plan, sampler_size};
    const u32 writes_before = descriptors.writes;
    for (const auto& stage : pipeline->stages) {
        for (const auto& desc : stage.texture_descriptors) {
            for (u32 i=0; i<desc.count; ++i, ++smp_i) {
                auto key = Build(texture_cache.GetSampler(sampler_scratch[smp_i]), desc.is_depth);
                reference[smp_i] = key;
                const Sampler& sampler = texture_cache.GetSampler(sampler_scratch[smp_i]);
                WRITE_SAMPLER;
            }
        }
    }
    assert(descriptors.writes - writes_before ==
           (reuse_sampler_plan && !validate_sampler_plan ? 0 : totals.sampler));
"""
main = r"""
    assert(sampler_plan.keys == reference);
    assert(std::all_of(sampler_plan.initialized.begin(), sampler_plan.initialized.end(), [](u8 x){return x == 1;}));
    if (reuse_sampler_plan && !validate) assert(!sampler_plan.sources.valid);
    return reuse_sampler_plan;
}
int main() {
    const char* value = std::getenv("NXBOX_D3D12_SAMPLER_PLAN_REUSE");
    const bool enabled = !value || std::strcmp(value, "0") != 0;
    Pipeline pipeline;
    TextureCache textures{{Sampler{}, Sampler{}}};
    pipeline.stages[0].texture_descriptors = {{2, false}};
    assert(!Run(&pipeline, textures, 4));
    assert(Run(&pipeline, textures, 4) == enabled);
    assert(Run(&pipeline, textures, 4, true) == enabled);
    textures.samplers[0].entry.words[3]++;
    assert(!Run(&pipeline, textures, 4));
    assert(Run(&pipeline, textures, 4) == enabled);
    pipeline.stages[0].texture_descriptors[0].is_depth = true;
    assert(!Run(&pipeline, textures, 4));
    assert(Run(&pipeline, textures, 4) == enabled);
    assert(!Run(&pipeline, textures, 8));
    assert(Run(&pipeline, textures, 8) == enabled);
    textures.samplers[1].entry.words[6]++;
    assert(!Run(&pipeline, textures, 8, true));
    assert(!Run(&pipeline, textures, 8));
    assert(Run(&pipeline, textures, 8) == enabled);
    // Repartitioning stages preserves the ordered sampler plan.
    pipeline.stages[0].texture_descriptors = {{1, true}};
    pipeline.stages[4].texture_descriptors = {{1, true}};
    assert(Run(&pipeline, textures, 8) == enabled);
    for (auto reduction : {Tegra::Texture::SamplerReduction::Min, Tegra::Texture::SamplerReduction::Max}) {
        textures.samplers[0].entry.reduction_filter = reduction;
        assert(!Run(&pipeline, textures, 8));
        policy = !policy;
        assert(!Run(&pipeline, textures, 8));
    }
    textures.samplers[0].entry.reduction_filter = Tegra::Texture::SamplerReduction::Standard;
    std::mt19937 random(71);
    for (u32 i=0; i<10000; ++i) {
        validate = (i % 3 == 0);
        if (i % 4 == 0) textures.samplers[random()%2].entry.words[random()%7] ^= random();
        if (i % 7 == 0) pipeline.stages[4].texture_descriptors[0].is_depth = !pipeline.stages[4].texture_descriptors[0].is_depth;
        Run(&pipeline, textures, i%11 == 0 ? 16 : 8, i%13 == 0);
    }
    assert(!Run(&pipeline, textures, 4096));
    assert(Run(&pipeline, textures, 4096) == enabled);
    assert(!Run(&pipeline, textures, 4097));
    // Vary sampler counts, stage layouts, padding and complete entry contents.
    for (u32 draw = 0; draw < 2000; ++draw) {
        const u32 count = random() % 129;
        textures.samplers.resize(count);
        for (auto& sampler : textures.samplers) {
            for (auto& word : sampler.entry.words) word = random();
            sampler.entry.reduction_filter = Tegra::Texture::SamplerReduction::Standard;
        }
        for (auto& stage : pipeline.stages) stage.texture_descriptors.clear();
        u32 remaining = count;
        for (u32 stage = 0; stage < 5; ++stage) {
            const u32 slots = stage == 4 ? remaining : random() % (remaining + 1);
            pipeline.stages[stage].texture_descriptors = {{slots, (random() & 1) != 0}};
            remaining -= slots;
        }
        const u32 size = std::max(1u, (count + 3) & ~3u);
        validate = draw % 2 == 0;
        Run(&pipeline, textures, size);
        assert(Run(&pipeline, textures, size) == (enabled && count != 0));
    }
    std::cout << "PASS: exact input/write/layout/compare invalidation, stage changes, aborted draws, min/max policy, validation, 12000 randomized differential draws; cache=" << enabled << '\n';
}
"""


class SamplerPlanReuseTests(unittest.TestCase):
    def test_random_draws_cache_on_off(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-sampler-plan-") as directory:
            cpp = Path(directory) / "test.cpp"
            cpp.write_text(prefix + logic + suffix.replace("WRITE_SAMPLER;", write) + finish + main)
            binary = Path(directory) / "test"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            for value in (None, "1", "0"):
                env = dict(os.environ)
                env.pop("NXBOX_D3D12_SAMPLER_PLAN_REUSE", None)
                if value is not None:
                    env["NXBOX_D3D12_SAMPLER_PLAN_REUSE"] = value
                env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
                subprocess.run([str(binary)], env=env, check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
