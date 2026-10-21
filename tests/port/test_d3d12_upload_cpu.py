# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate upload accounting and index coherence without the Windows SDK."""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


def test_upload_category_counts_include_legacy_callers():
    context = (NATIVE / "d3d12_context.cpp").read_text()
    method = (
        "void FrameProfile::CountEnabled("
        + context.split("void FrameProfile::CountEnabled(", 1)[1].split(
            "void FrameProfile::NoteTransition", 1
        )[0]
    )
    driver = r"""
#include <cassert>
#include <cstdint>
#include "video_core/renderer_d3d12/d3d12_context.h"
using namespace D3D12;
using u64 = std::uint64_t;
struct FrameProfile {
    bool owner = true;
    std::array<u64, static_cast<size_t>(FrameCount::Count)> counts{};
    bool OnOwner() const { return owner; }
    void CountEnabled(FrameCount counter, u64 amount = 1);
    void Count(FrameCount counter, u64 amount = 1) { CountEnabled(counter, amount); }
};
__METHOD__
int main() {
    ::FrameProfile profile;
    const FrameCount bytes[]{FrameCount::VertexUploadBytes, FrameCount::IndexUploadBytes,
        FrameCount::UniformUploadBytes, FrameCount::StorageUploadBytes,
        FrameCount::TexelUploadBytes, FrameCount::OtherUploadBytes};
    const FrameCount events[]{FrameCount::VertexUploadCount, FrameCount::IndexUploadCount,
        FrameCount::UniformUploadCount, FrameCount::StorageUploadCount,
        FrameCount::TexelUploadCount, FrameCount::OtherUploadCount};
    for (size_t i = 0; i < 6; ++i) {
        profile.Count(bytes[i], 256);
        profile.Count(bytes[i], 512);
        profile.Count(bytes[i], 0);
        profile.Count(FrameCount::BufferUploadBytes, 768);
        assert(profile.counts[static_cast<size_t>(bytes[i])] == 768);
        assert(profile.counts[static_cast<size_t>(events[i])] == 2);
    }
    assert(profile.counts[static_cast<size_t>(FrameCount::BufferUploadBytes)] == 4608);
    profile.owner = false;
    profile.Count(FrameCount::IndexUploadBytes, 99);
    assert(profile.counts[static_cast<size_t>(FrameCount::IndexUploadCount)] == 2);
}
""".replace("__METHOD__", method)
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "accounting.cpp"
        binary = Path(directory) / "accounting"
        source.write_text(driver)
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "src"),
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run([str(binary)], check=True)


def test_validated_hit_precedes_read_hash_restart_scan_and_download():
    rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
    prepare = rasterizer.split("bool RasterizerD3D12::PrepareDrawIndices", 1)[1].split(
        "void RasterizerD3D12::Draw(", 1
    )[0]
    assert "gpu_memory->FlushCaching()" not in prepare
    assert "IsRegionCpuModified(*cpu, input_size)" in prepare
    assert prepare.index("external_write = MustFlushRegion") < prepare.index(
        "index_converter.FindValidated"
    )
    assert "FlushRegion(*cpu, input_size, VideoCommon::CacheType::All)" in prepare
    assert "buffer_cache.WriteMemory(*cpu, input_size)" in prepare
    assert "validation_key.device_address = cpu.value_or(0)" in prepare
    assert "FindValidated(validation_key, source)" in prepare
    # The existing diagnostic toggle also gates validated hits.
    assert 'found && !BisectEnabled("NXBOX_D3D12_NO_INDEX_VALIDATED") &&' in prepare
    assert "(!found->native || allow_native)" in prepare
    assert "info->storage_buffers_descriptors.empty()" in prepare
    assert "info->image_buffer_descriptors.empty()" in prepare
    assert "regs.transform_feedback_enabled == 0" in prepare
    assert prepare.index("ObtainBufferSynchronize::FullSynchronize") < prepare.index(
        "index_converter.FindValidated"
    )
    miss = prepare.split("if (!validated_hit)", 1)[1]
    assert "gpu_memory->ReadBlock" in miss and "buffer_cache.DownloadMemory" in miss
    assert "IndexConversion::ContainsRestart" in miss
    assert "index_read_scratch.resize" in miss
    assert "IndexConversion::NeedsConversion(topology, format, {}, provoking)" in miss
    assert "index_converter.RememberValidated" in miss
    assert "converted_indices.reset(); // Bind through the normal index buffer cache" in miss
    converter = (NATIVE / "d3d12_index_converter.cpp").read_text()
    hit = converter.split("std::optional<ConvertedIndexBuffer> IndexConverter::FindValidated", 1)[
        1
    ].split("void IndexConverter::RememberValidated", 1)[0]
    assert "found->Matches(source)" in hit and "context.Retain" in hit
    assert "CityHash64" not in hit and "CreateBuffer" not in hit


def test_profile_subphases_and_category_averages_are_wired():
    context = (NATIVE / "d3d12_context.cpp").read_text()
    for phase in (
        "index_read",
        "index_hash",
        "index_flush_caching",
        "index_coherence",
        "index_restart_scan",
        "upload_allocation",
        "upload_transitions",
        "upload_recording",
        "staging_collect",
    ):
        assert f'"{phase}"' in context
    assert '" {}_avg={:.2f}"' in context
    assert "StagingFastRequests" in context and "StagingCollections" in context
    runtime = (NATIVE / "d3d12_buffer_cache.cpp").read_text()
    assert "FrameCount::UploadCopyCommands" in runtime
    assert "synchronizing_indices ? FrameCount::IndexUploadBytes : counter" in runtime


def test_validated_result_uses_weak_allocation_and_retains_only_a_live_hit():
    converter = (NATIVE / "d3d12_index_converter.cpp").read_text()
    methods = converter.split(
        "std::optional<ConvertedIndexBuffer> IndexConverter::FindValidated", 1
    )[1].split("ConvertedIndexBuffer IndexConverter::Generate", 1)[0]
    driver = r"""
#include <cassert>
#include <memory>
#include <optional>
#include "video_core/renderer_d3d12/d3d12_index_cache.h"
namespace D3D12 {
struct Resource { std::uint64_t content_version{}; };
using ResourcePtr = std::shared_ptr<Resource>;
struct ConvertedIndexBuffer { unsigned count{}; bool native{}; ResourcePtr resource; };
struct Context {
    ResourcePtr retained;
    void Retain(const ResourcePtr& resource) { retained = resource; }
};
struct IndexConverter {
    struct ValidatedIndexBuffer {
        ConvertedIndexBuffer buffer;
        std::weak_ptr<Resource> allocation;
    };
    Context context;
    IndexConversion::ConversionCache<IndexConversion::ValidatedSnapshot<Resource, ValidatedIndexBuffer>> validated;
    std::optional<ConvertedIndexBuffer> FindValidated(const IndexConversion::CacheKey&, const ResourcePtr&);
    void RememberValidated(const IndexConversion::CacheKey&, const ResourcePtr&, const ConvertedIndexBuffer&);
};
std::optional<ConvertedIndexBuffer> IndexConverter::FindValidated__METHODS__
}
int main() {
    using namespace D3D12;
    IndexConverter cache;
    IndexConversion::CacheKey key{};
    auto source = std::make_shared<Resource>();
    assert(!cache.FindValidated(key, source));
    cache.RememberValidated(key, source, {0, true, {}});
    auto native = cache.FindValidated(key, source);
    assert(native && native->native && !cache.context.retained);
    ++source->content_version;
    assert(!cache.FindValidated(key, source));
    auto allocation = std::make_shared<Resource>();
    cache.RememberValidated(key, source, {6, false, allocation});
    auto hit = cache.FindValidated(key, source);
    assert(hit && hit->count == 6 && hit->resource == allocation);
    assert(cache.context.retained == allocation);
    std::weak_ptr<Resource> weak = allocation;
    allocation.reset();
    assert(!weak.expired()); // Recorded draw lease survives eviction.
    cache.context.retained.reset();
    hit.reset();
    assert(weak.expired()); // Validation metadata owns no converted allocation.
    assert(!cache.FindValidated(key, source));
    auto replacement = std::make_shared<Resource>();
    replacement->content_version = source->content_version;
    assert(!cache.FindValidated(key, replacement));
    std::weak_ptr<Resource> source_weak = source;
    source.reset();
    assert(source_weak.expired());
}
""".replace("__METHODS__", methods)
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "validated.cpp"
        binary = Path(directory) / "validated"
        source.write_text(driver)
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
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run([str(binary)], check=True)
