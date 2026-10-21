# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production generation-hit branch without the Windows SDK."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class PipelineGenerationLookup(unittest.TestCase):
    def test_production_hit_branch_and_lifetime(self):
        source = (BACKEND / "d3d12_pipeline_cache.cpp").read_text()
        current = function(source, "IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline")
        branch = function(current, "if (PipelineLookupFastpathEnabled()")
        invalidate = function(source, "void PipelineCache::InvalidateLastGraphicsPipeline()")
        remember = function(source, "IGraphicsPipeline* PipelineCache::RememberGraphicsPipeline(")
        # Run the actual hit branch and pointer installation/invalidation methods. The large
        # fake key counts equality operations, which must stay at zero on generation hits.
        harness = (
            r"""
#include <cassert>
#include <cstdint>
#include <iostream>
#include "video_core/renderer_d3d12/d3d12_state_cache.h"
using namespace D3D12;
struct IGraphicsPipeline {};
struct GraphicsPipelineKey { int id; };
enum class FrameCount { PrewarmedPsoHits };
struct ProfileData {
    bool enabled{};
    int hits{};
    bool Enabled() const { return enabled; }
    void Count(FrameCount) { ++hits; }
};
struct Context {
    ProfileData data;
    ProfileData& Profile() { return data; }
};
struct Prewarmed {
    int queries{};
    bool contains(const GraphicsPipelineKey& key) { ++queries; return key.id == 7; }
};
struct PipelineCache {
    Context context;
    Prewarmed prewarmed_graphics;
    StateInputCache<GraphicsStateInputs> graphics_state_inputs;
    GraphicsPipelineKey last_graphics_request{};
    IGraphicsPipeline* last_graphics_pipeline{};
    bool last_graphics_request_current{};
    bool last_graphics_prewarmed{};
    bool enabled{true}, verify{};
    int slow{};
    bool PipelineLookupFastpathEnabled() { return enabled; }
    bool StateVerifyEnabled() { return verify; }
    void InvalidateLastGraphicsPipeline();
    IGraphicsPipeline* RememberGraphicsPipeline(const GraphicsPipelineKey&, IGraphicsPipeline*);
    IGraphicsPipeline* Lookup(const GraphicsStateInputs& inputs) {
        const bool reuse = graphics_state_inputs.Matches(inputs);
        HIT_BRANCH
        // Mirror the production boundary to exercise a new input set followed by a pending
        // or unsupported lookup. Structural assertions below tie this to compute_request.
        if (!reuse || StateVerifyEnabled()) {
            last_graphics_request_current = false;
            graphics_state_inputs.Store(inputs);
        }
        ++slow;
        return nullptr;
    }
};
INVALIDATE
REMEMBER
int main() {
    PipelineCache cache;
    GraphicsStateInputs inputs{};
    IGraphicsPipeline first, second;
    assert(!cache.Lookup(inputs));
    cache.RememberGraphicsPipeline({7}, &first);
    const int queries = cache.prewarmed_graphics.queries;
    for (int i = 0; i < 10000; ++i) assert(cache.Lookup(inputs) == &first);
    assert(cache.slow == 1 && cache.prewarmed_graphics.queries == queries);
    assert(queries == 0);
    cache.context.data.enabled = true;
    cache.RememberGraphicsPipeline({7}, &first);
    assert(cache.Lookup(inputs) == &first);
    assert(cache.context.data.hits == 1);
    // Every non-register dependency as well as the dirty generation must miss.
    for (int field = 0; field < 10; ++field) {
        auto changed = inputs;
        switch (field) {
        case 0: ++changed.generation; break;
        case 1: ++changed.hashes[0]; break;
        case 2: ++changed.formats.colors[0]; break;
        case 3: ++changed.formats.depth; break;
        case 4: ++changed.formats.sample_count; break;
        case 5: ++changed.topology; break;
        case 6: ++changed.zeta_enable; break;
        case 7: changed.indexed = true; break;
        case 8: ++changed.tessellation_enables[0]; break;
        case 9: ++changed.tessellation_enables[1]; break;
        }
        assert(!cache.Lookup(changed));
        // Repeated pending requests must not return the old pipeline.
        assert(!cache.Lookup(changed));
        cache.RememberGraphicsPipeline({8}, &second);
        assert(cache.Lookup(changed) == &second);
        assert(!cache.Lookup(inputs));
        cache.RememberGraphicsPipeline({7}, &first);
    }
    cache.InvalidateLastGraphicsPipeline();
    assert(!cache.Lookup(inputs));
    cache.RememberGraphicsPipeline({8}, nullptr);
    assert(!cache.Lookup(inputs));
    cache.RememberGraphicsPipeline({7}, &first);
    cache.enabled = false;
    assert(!cache.Lookup(inputs));
    cache.enabled = true;
    cache.verify = true;
    assert(!cache.Lookup(inputs));
    std::cout << "10000 unchanged draws: no key/hash lookup; dependency, pending, lifecycle, "
                 "profiling, opt-out and verify cases passed\n";
}
""".replace("HIT_BRANCH", branch)
            .replace("INVALIDATE", invalidate)
            .replace("REMEMBER", remember)
        )
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "lookup.cpp"
            binary = Path(directory) / "lookup"
            cpp.write_text(harness)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_generation_branch_precedes_key_work_and_retains_safety_checks(self):
        source = (BACKEND / "d3d12_pipeline_cache.cpp").read_text()
        current = function(source, "IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline")
        hit = current.index("if (PipelineLookupFastpathEnabled()")
        for prerequisite in (
            "SyncGuestHost();",
            "RefreshStages(guest_hashes)",
            "context.GraphicsStateTracker().Update(*maxwell3d)",
            "graphics_state_inputs.Matches(inputs)",
        ):
            self.assertLess(current.index(prerequisite), hit)
        for slow_work in (
            "Pso::GraphicsPsoState state;",
            "GraphicsPipelineKey rebuilt_request;",
            "FillGraphicsState(*maxwell3d, state)",
            "request = cached_graphics_request;",
            "prewarmed_graphics.contains(request)",
            "guest_graphics.find(request)",
        ):
            self.assertLess(hit, current.index(slow_work))
        compute = current.split("if (!reuse || StateVerifyEnabled())", 1)[1]
        self.assertLess(
            compute.index("last_graphics_request_current = false;"),
            compute.index("compute_request();"),
        )
        self.assertIn('"NXBOX_D3D12_PIPELINE_LOOKUP_FASTPATH"', source)
        toggle = function(source, "bool PipelineLookupFastpathEnabled()")
        self.assertIn("static const bool enabled", toggle)
        self.assertIn("value[0] == '0'", toggle)
        header = (BACKEND / "d3d12_pipeline_cache.h").read_text()
        self.assertIn("std::has_unique_object_representations_v<GraphicsPipelineKey>", header)
        self.assertIn("Common::CityHash64", header)
        equality = function(source, "bool GraphicsPipelineKey::operator==")
        self.assertIn("std::memcmp", equality)


if __name__ == "__main__":
    unittest.main()
