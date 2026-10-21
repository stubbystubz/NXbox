# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Cached guest writes must reach D3D12 caches regardless of the CPU skip hack."""

import re
import unittest
from pathlib import Path

from test_d3d12_copy_write_hazards import function_body

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class CachedInvalidationTests(unittest.TestCase):
    def test_flush_caching_dispatches_to_an_unconditional_override(self):
        header = (NATIVE / "d3d12_rasterizer.h").read_text()
        self.assertRegex(
            header,
            r"void InnerInvalidation\(std::span<const std::pair<DAddr, std::size_t>> sequences\) override;",
        )
        memory = (ROOT / "src/video_core/memory_manager.cpp").read_text()
        flush = function_body(memory, "void MemoryManager::FlushCaching")
        self.assertIn("rasterizer->InnerInvalidation", flush)
        source = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        body = function_body(source, "void RasterizerD3D12::InnerInvalidation")
        self.assertNotIn("skip_cpu_inner_invalidation", body)
        self.assertNotIn("InvalidateRegion(addr, size,", body)

    def test_cached_guest_writes_match_vulkan_invalidation(self):
        native = function_body(
            (NATIVE / "d3d12_rasterizer.cpp").read_text(),
            "void RasterizerD3D12::InnerInvalidation",
        )
        vulkan = function_body(
            (ROOT / "src/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text(),
            "void RasterizerVulkan::InnerInvalidation",
        )
        # Preserve cache ordering. The texture batch coalesces the exact union;
        # test_p48_cache_cpu executes its production WriteMemory equivalence.
        calls = r"(?:texture_cache|buffer_cache|query_cache|(?:default_)?pipeline_cache)(?:\.|->)(?:WriteMemory|InvalidateRegion)\(addr, size\);"
        normalize = lambda s: s.replace("default_pipeline_cache->", "pipeline_cache.")
        self.assertEqual(
            [normalize(call) for call in re.findall(calls, native)],
            re.findall(calls, vulkan),
        )
        for cache in ("texture_cache", "buffer_cache"):
            iteration = (
                r"ForEachInvalidationRange\(sequences, coalesce, \[&\]\(DAddr addr, u64 size\) \{\s*"
                if cache == "texture_cache"
                else r"for \(const auto& \[addr, size\] : sequences\) \{\s*"
            )
            self.assertRegex(
                native,
                rf"std::scoped_lock lock\{{{cache}\.mutex\}};\s*"
                + iteration
                + rf"{cache}\.WriteMemory\(addr, size\);",
            )

    def test_buffer_write_restores_upload_eligibility_and_cancels_stale_downloads(self):
        source = (ROOT / "src/video_core/buffer_cache/buffer_cache.h").read_text()
        write = function_body(source, "void BufferCache<P>::WriteMemory")
        self.assertIn("ClearDownload(device_addr, size);", write)
        self.assertIn("gpu_modified_ranges.Subtract(device_addr, size);", write)
        self.assertIn("memory_tracker.MarkRegionAsCpuModified(device_addr, size);", write)
        sync = function_body(source, "bool BufferCache<P>::SynchronizeBuffer")
        self.assertIn("memory_tracker.ForEachUploadRange", sync)
        self.assertIn("gpu_modified_ranges.ForEachInRange", sync)
        # Draw-time invalidation must precede the cache lock and vertex upload selection.
        draw = function_body(
            (NATIVE / "d3d12_rasterizer.cpp").read_text(),
            "void RasterizerD3D12::PrepareDraw",
        )
        self.assertLess(
            draw.index("gpu_memory->FlushCaching();"),
            draw.index("std::lock(buffer_lock, texture_lock)"),
        )
        self.assertLess(
            draw.index("gpu_memory->FlushCaching();"),
            draw.index("buffer_cache.BindHostGeometryBuffers"),
        )


if __name__ == "__main__":
    unittest.main()
