// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <optional>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_build_stats.h"
#include "video_core/renderer_d3d12/d3d12_compile_policy.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_disk_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_dxc.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"

namespace D3D12 {

const wchar_t* StageTarget(Shader::Stage stage, bool native64) {
    switch (stage) {
    case Shader::Stage::VertexA:
    case Shader::Stage::VertexB:
        return native64 ? L"vs_6_6" : L"vs_6_0";
    case Shader::Stage::TessellationControl:
        return native64 ? L"hs_6_6" : L"hs_6_0";
    case Shader::Stage::TessellationEval:
        return native64 ? L"ds_6_6" : L"ds_6_0";
    case Shader::Stage::Geometry:
        return native64 ? L"gs_6_6" : L"gs_6_0";
    case Shader::Stage::Fragment:
        return native64 ? L"ps_6_6" : L"ps_6_0";
    case Shader::Stage::Compute:
    default:
        return native64 ? L"cs_6_6" : L"cs_6_0";
    }
}

DxilBlob DxcCache::Compile(std::string_view hlsl, Shader::Stage stage, u64 pipeline_hash,
                           DiskPipelineCache* disk) {
    auto* const stats = active_build_stats;
    // The emitter only writes this intrinsic after checking SM 6.6 + OPTIONS11.
    const bool native64 = hlsl.find(".InterlockedCompareExchange64(") != std::string_view::npos;
    const wchar_t* target = StageTarget(stage, native64);
    u64 hash = Pso::HashBytes(hlsl.data(), hlsl.size());
    if (native64) {
        constexpr u64 sm66_salt = 0x4458435F534D3636ULL;
        hash = Pso::HashBytes(&sm66_salt, sizeof(sm66_salt), hash);
    }
    const auto stage_value = static_cast<u32>(stage);
    hash = Pso::HashBytes(&stage_value, sizeof(stage_value), hash);
    if (DxcStrictEnabled()) {
        // Separate strict IEEE DXIL in both the memory and disk caches.
        constexpr u64 strict_salt = 0x4458435F47495331ULL;
        hash = Pso::HashBytes(&strict_salt, sizeof(strict_salt), hash);
    }
    // Preserve existing O3 disk hits; separate every non-default optimization level.
    const auto optimization = DxcOptimization();
    if (optimization != 3) {
        constexpr u64 optimization_salt = 0x4458435F4F505431ULL;
        hash = Pso::HashBytes(&optimization_salt, sizeof(optimization_salt), hash);
        hash = Pso::HashBytes(&optimization, sizeof(optimization), hash);
    }
    const auto [begin, end] = entries.equal_range(hash);
    for (auto it = begin; it != end; ++it) {
        if (it->second.target == target && it->second.source == hlsl) {
            if (it->second.failure) {
                if (profile)
                    profile->Count(FrameCount::NegativeCacheHits);
                std::rethrow_exception(it->second.failure);
            }
            if (stats)
                ++stats->memory_hits;
            return it->second.dxil;
        }
    }
    if (disk) {
        if (auto cached = disk->FindDxil(hash)) {
            if (stats)
                ++stats->disk_hits;
            auto dxil = std::make_shared<const std::vector<u8>>(std::move(*cached));
            entries.emplace(hash, Entry{std::string{hlsl}, target, dxil, {}});
            return dxil;
        }
    }
    if (stats)
        ++stats->disk_misses;
    std::optional<FrameProfile::Scope> scope;
    if (profile) {
        scope.emplace(*profile, FramePhase::DxcCompile);
        profile->Count(FrameCount::DxcCacheMisses);
        profile->Count(FrameCount::DxcCompiles);
    }
    try {
        auto dxil = [&] {
            const PsoBuildTimer timer{&PsoBuildStats::dxc_us};
            return std::make_shared<const std::vector<u8>>(
                compiler.Compile(hlsl, L"main", target, pipeline_hash));
        }();
        if (disk)
            disk->StoreDxil(hash, *dxil);
        entries.emplace(hash, Entry{std::string{hlsl}, target, dxil, {}});
        return dxil;
    } catch (...) {
        entries.emplace(hash, Entry{std::string{hlsl}, target, {}, std::current_exception()});
        throw;
    }
}

} // namespace D3D12
