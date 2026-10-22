// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include "video_core/renderer_d3d12/d3d12_descriptor.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <fmt/format.h>
#include "common/alignment.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/textures/texture.h"

namespace D3D12 {
using Tegra::Texture::DepthCompareFunc;
using Tegra::Texture::TextureFilter;
using Tegra::Texture::TextureMipmapFilter;
using Tegra::Texture::WrapMode;

DXGI_FORMAT DsvFormat(DXGI_FORMAT view_format) {
    switch (view_format) {
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
        return view_format;
    }
}

namespace {

struct SrvRange {
    UINT mip, levels, first, size, plane;
};

std::optional<SrvRange> ReadSrvRange(const D3D12_SHADER_RESOURCE_VIEW_DESC& d) {
    switch (d.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        return SrvRange{d.Texture1D.MostDetailedMip, d.Texture1D.MipLevels, 0, 1, 0};
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        return SrvRange{d.Texture1DArray.MostDetailedMip, d.Texture1DArray.MipLevels,
                        d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, 0};
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        return SrvRange{d.Texture2D.MostDetailedMip, d.Texture2D.MipLevels, 0, 1,
                        d.Texture2D.PlaneSlice};
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        return SrvRange{d.Texture2DArray.MostDetailedMip, d.Texture2DArray.MipLevels,
                        d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize,
                        d.Texture2DArray.PlaneSlice};
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        return SrvRange{d.Texture3D.MostDetailedMip, d.Texture3D.MipLevels, 0, 1, 0};
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        return SrvRange{d.TextureCube.MostDetailedMip, d.TextureCube.MipLevels, 0, 6, 0};
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        return SrvRange{d.TextureCubeArray.MostDetailedMip, d.TextureCubeArray.MipLevels,
                        d.TextureCubeArray.First2DArrayFace, d.TextureCubeArray.NumCubes * 6, 0};
    default:
        return std::nullopt;
    }
}

} // namespace

// The shader's declared texture type, not the guest view type, decides the SRV
// dimension: a mismatch is undefined in D3D12. vkd3d tolerates it, but the
// Xbox driver samples zeros (SM3DW lost its ambient light that way).
std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> AdaptSrvDimension(
    const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, D3D12_SRV_DIMENSION wanted,
    const D3D12_RESOURCE_DESC& resource) {
    if (resource.SampleDesc.Count > 1) {
        const bool array = wanted == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
        if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.ViewDimension == wanted && (array || wanted == D3D12_SRV_DIMENSION_TEXTURE2DMS))
            return desc;
        return std::nullopt;
    }
    const auto range = ReadSrvRange(desc);
    if (!range)
        return std::nullopt;
    const auto [mip, levels, first, size, plane] = *range;
    const bool is_1d = resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is_2d = resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    D3D12_SHADER_RESOURCE_VIEW_DESC out{};
    out.Format = desc.Format;
    out.Shader4ComponentMapping = desc.Shader4ComponentMapping;
    out.ViewDimension = wanted;
    // A 2D TIC can expose one slice of a cached array. A shader cube declaration
    // needs complete faces from the backing resource, not that single-slice range.
    const UINT remaining_layers =
        resource.DepthOrArraySize - std::min<UINT>(first, resource.DepthOrArraySize);
    const UINT cube_layers = size < 6 ? remaining_layers : size;
    const bool cube_compatible = is_2d && plane == 0 &&
                                 first < resource.DepthOrArraySize && cube_layers >= 6 &&
                                 cube_layers <= resource.DepthOrArraySize - first;
    switch (wanted) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        if (!is_1d || first != 0)
            return std::nullopt;
        out.Texture1D = {mip, levels, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        if (!is_1d)
            return std::nullopt;
        out.Texture1DArray = {mip, levels, first, size, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        if (!is_2d || first != 0)
            return std::nullopt;
        out.Texture2D = {mip, levels, plane, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        if (!is_2d)
            return std::nullopt;
        out.Texture2DArray = {mip, levels, first, size, plane, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        if (!cube_compatible || first != 0)
            return std::nullopt;
        out.TextureCube = {mip, levels, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        if (!cube_compatible)
            return std::nullopt;
        out.TextureCubeArray = {mip, levels, first, cube_layers / 6, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        if (resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D || first != 0 || plane != 0)
            return std::nullopt;
        out.Texture3D = {mip, levels, 0.0f};
        return out;
    default:
        return std::nullopt;
    }
}

namespace {
D3D12_TEXTURE_ADDRESS_MODE AddressMode(WrapMode mode, TextureFilter mag_filter) {
    switch (mode) {
    case WrapMode::Wrap:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case WrapMode::Mirror:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case WrapMode::Border:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case WrapMode::MirrorOnceClampToEdge:
    case WrapMode::MirrorOnceBorder:
    case WrapMode::MirrorOnceClampOGL:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    case WrapMode::Clamp:
        // GL_CLAMP: Vulkan/OpenGL backends clamp to the border when magnifying linearly.
        return mag_filter == TextureFilter::Linear ? D3D12_TEXTURE_ADDRESS_MODE_BORDER
                                                   : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    default: // ClampToEdge
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}
} // namespace

namespace {
std::atomic<bool> minmax_filtering_supported{true};
std::atomic<bool> minmax_fallback_logged{false};
} // namespace

void SetMinMaxFilteringSupported(bool supported) {
    minmax_filtering_supported.store(supported);
}

D3D12_SAMPLER_DESC MakeSamplerDesc(const Tegra::Texture::TSCEntry& entry, bool shader_compares) {
    D3D12_SAMPLER_DESC desc{};
    const u32 min_linear = entry.min_filter == TextureFilter::Linear ? 1 : 0;
    const u32 mag_linear = entry.mag_filter == TextureFilter::Linear ? 1 : 0;
    const u32 mip_linear = entry.mipmap_filter == TextureMipmapFilter::Linear ? 1 : 0;
    u32 reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    // Maxwell applies the TSC compare only to depth-compare instructions; games leave it set
    // on samplers they also use for plain fetches. D3D12 leaves Sample* with a comparison
    // sampler undefined and the Xbox (AMD) driver returns zeros, which blacked out SM3DW's
    // vertex-shader ambient cube.
    const bool compare = entry.depth_compare_enabled && shader_compares;
    if (compare) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    } else if (entry.reduction_filter == Tegra::Texture::SamplerReduction::Min) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MINIMUM;
    } else if (entry.reduction_filter == Tegra::Texture::SamplerReduction::Max) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    }
    const float anisotropy = std::clamp(entry.MaxAnisotropy(), 1.0f, 16.0f);
    if ((reduction == D3D12_FILTER_REDUCTION_TYPE_MINIMUM ||
         reduction == D3D12_FILTER_REDUCTION_TYPE_MAXIMUM) &&
        !minmax_filtering_supported.load()) {
        // A point sample reads one texel, so min and max equal the standard result there.
        // Linear or anisotropic min/max has no exact substitute; standard filtering is close.
        const bool point = min_linear == 0 && mag_linear == 0 && mip_linear == 0 &&
                           anisotropy <= 1.0f;
        if (!point && !minmax_fallback_logged.exchange(true))
            Diagnostic("D3D12 min/max sampler filtering unsupported; using standard");
        reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    }
    desc.Filter = anisotropy > 1.0f
                      ? static_cast<D3D12_FILTER>(D3D12_ANISOTROPIC_FILTERING_BIT |
                                                  D3D12_ENCODE_BASIC_FILTER(1, 1, 1, reduction))
                      : static_cast<D3D12_FILTER>(D3D12_ENCODE_BASIC_FILTER(min_linear, mag_linear,
                                                                            mip_linear, reduction));
    desc.AddressU = AddressMode(entry.wrap_u, entry.mag_filter);
    desc.AddressV = AddressMode(entry.wrap_v, entry.mag_filter);
    desc.AddressW = AddressMode(entry.wrap_p, entry.mag_filter);
    desc.MipLODBias = entry.LodBias();
    desc.MaxAnisotropy = static_cast<UINT>(anisotropy);
    desc.ComparisonFunc = compare
                              ? static_cast<D3D12_COMPARISON_FUNC>(
                                    static_cast<u32>(entry.depth_compare_func.Value()) + 1)
                              : D3D12_COMPARISON_FUNC_NEVER;
    const auto border = entry.srgb_conversion ? entry.SrgbBorderColor() : entry.BorderColor();
    std::copy(border.begin(), border.end(), desc.BorderColor);
    // yuzu convention: no mip filter means "sample level 0 only". Match Vulkan and zero
    // MinLOD too; MinLOD > MaxLOD is undefined and AMD drivers sample zeros.
    const bool no_mips = entry.mipmap_filter == TextureMipmapFilter::None;
    desc.MaxLOD = no_mips ? 0.25f : entry.MaxLod();
    desc.MinLOD = no_mips ? 0.0f : std::min(entry.MinLod(), desc.MaxLOD);
    return desc;
}

D3D12DescriptorAllocator::D3D12DescriptorAllocator(Context& context, u32 view_capacity,
                                                   u32 sampler_capacity)
    : handle_table_reuse{[] {
          const char* value = std::getenv("NXBOX_D3D12_HANDLE_TABLE_CACHE");
          return !value || std::strcmp(value, "0") != 0;
      }()},
      descriptor_table_cache{BisectEnabled("NXBOX_D3D12_DESCRIPTOR_TABLE_CACHE")},
      descriptor_fastpath{[] {
          const char* value = std::getenv("NXBOX_D3D12_DESCRIPTOR_FASTPATH");
          return !value || std::strcmp(value, "0") != 0;
      }()},
      descriptor_ranges{[] {
          const char* value = std::getenv("NXBOX_D3D12_DESCRIPTOR_RANGES");
          return !value || std::strcmp(value, "0") != 0;
      }()},
      persistent_null_descriptors{[] {
          const char* value = std::getenv("NXBOX_D3D12_PERSISTENT_NULL_DESCRIPTORS");
          return !value || std::strcmp(value, "0") != 0;
      }()},
      context{context}, views{view_capacity}, samplers{sampler_capacity} {
    if (!view_capacity || view_capacity > ViewHeapSize || !sampler_capacity ||
        sampler_capacity > SamplerHeapSize)
        throw std::invalid_argument("D3D12 descriptor heap capacity outside supported limits");
    Init(views, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, view_capacity, true);
    Init(samplers, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, sampler_capacity, true);
    Init(rtvs, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RtvHeapSize, false);
    Init(dsvs, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DsvHeapSize, false);
    InitCache(view_cache, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
    InitCache(sampler_cache, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256);
    if (persistent_null_descriptors)
        InitCache(null_view_cache, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32);
    InitCache(bound_rtvs, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8);
    InitCache(bound_dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    Diagnostic(fmt::format("D3D12 handle_table_cache={}", handle_table_reuse ? 1 : 0));
    Diagnostic(fmt::format("D3D12 descriptor_fastpath={}", descriptor_fastpath ? 1 : 0));
    Diagnostic(fmt::format("D3D12 descriptor_key_patch={}",
                           previous_draw_tables[0].plan.patch_keys ? 1 : 0));
    Diagnostic(fmt::format("D3D12 descriptor_table_cache={}", descriptor_table_cache ? 1 : 0));
    Diagnostic(fmt::format("D3D12 descriptor_ranges={} persistent_null_descriptors={}",
                           descriptor_ranges, persistent_null_descriptors));
}

void D3D12DescriptorAllocator::InitCache(DescriptorCache& cache, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                         u32 capacity) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    Check(context.Device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&cache.heap)),
          "create CPU descriptor cache", context.Device());
    cache.stride = context.Device()->GetDescriptorHandleIncrementSize(type);
    cache.capacity = capacity;
    cache.cpu = cache.heap->GetCPUDescriptorHandleForHeapStart();
}
void D3D12DescriptorAllocator::RefreshCaches() {
    if (cache_tick == context.CurrentTick())
        return;
    cache_tick = context.CurrentTick();
    slot_contents.clear();
    // Cached views hold their resource, so they stay valid across ticks. Clearing them on every
    // submit (~30 per frame in BotW) recreated ~11k descriptors per frame; once per frame bounds
    // how long they keep freed resources alive. Samplers reference no resources at all.
    if (new_frame) {
        new_frame = false;
        view_cache.entries.clear();
        view_cache.cursor = 0;
        ++view_cache.generation;
    }
}
DescriptorKey D3D12DescriptorAllocator::MakeDescriptorKey(const ResourcePtr& resource, u32 kind,
                                                          const void* desc, size_t size) {
    DescriptorKey key;
    if (size > key.description.size())
        throw std::invalid_argument("D3D12 descriptor exceeds cache key size");
    key.resource = reinterpret_cast<std::uintptr_t>(resource ? resource->handle.Get() : nullptr);
    key.kind = kind;
    key.size = static_cast<u32>(size);
    std::memcpy(key.description.data(), desc, size);
    return key;
}

void D3D12DescriptorAllocator::WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                           const ResourcePtr& resource, u32 kind, const void* desc,
                                           size_t size, const DescriptorTable& table, u32 slot) {
    if (slot >= table.count)
        throw std::out_of_range("D3D12 descriptor write outside table slice");
    if (table.plan) {
        const u32 index = table.first + slot;
        auto& plan = *table.plan;
        if (size > DescriptorKey{}.description.size())
            throw std::invalid_argument("D3D12 descriptor exceeds cache key size");
        auto& key = table.plan->keys.at(index);
        const auto identity =
            reinterpret_cast<std::uintptr_t>(resource ? resource->handle.Get() : nullptr);
        if (!plan.patch_keys || key.resource != identity || key.kind != kind || key.size != size ||
            std::memcmp(key.description.data(), desc, size) != 0) {
            key = MakeDescriptorKey(resource, kind, desc, size);
        }
        if (descriptor_fastpath && plan.sources.valid) {
            RefreshCaches();
            auto& cache = type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? sampler_cache : view_cache;
            const auto& previous = previous_draw_tables[type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER];
            // Unchanged slots borrow immutable CPU sources without hashing or map access.
            plan.sources.Set(
                index, previous.plan.sources,
                index < previous.plan.keys.size() && previous.plan.keys[index] == key,
                [&] { return cache.generation; },
                [&] { return ResolveDescriptor(type, resource, key, nullptr).ptr; });
        }
        table.plan->resources.at(index) = resource;
        if (!table.plan->initialized.empty())
            table.plan->initialized.at(index) = 1;
        return;
    }
    WriteCached(type, resource, kind, desc, size, table.Slot(slot));
}

void D3D12DescriptorAllocator::ResolvePlanSources(DescriptorPlan& plan, bool sampler) {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_D3D12_SAMPLER_SOURCE_REUSE");
        return !value || std::strcmp(value, "0") != 0;
    }();
    if (!enabled || !descriptor_fastpath)
        return;
    plan.sources.Reset(plan.keys.size());
    if (!plan.sources.valid)
        return;
    RefreshCaches();
    auto& cache = sampler ? sampler_cache : view_cache;
    const auto& previous = previous_draw_tables[sampler ? 1 : 0].plan;
    const auto type =
        sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    for (u32 index = 0; index < plan.keys.size(); ++index) {
        const auto& key = plan.keys[index];
        plan.sources.Set(
            index, previous.sources, index < previous.keys.size() && previous.keys[index] == key,
            [&] { return cache.generation; },
            [&] { return ResolveDescriptor(type, plan.resources[index], key, nullptr).ptr; });
    }
}

bool D3D12DescriptorAllocator::CanReuseDrawTable(const DescriptorPlan& plan, bool sampler) const {
    return CanReusePreviousDrawTable(plan, sampler) || FindHandleDrawTable(plan, sampler) ||
           FindCachedDrawTable(plan, sampler);
}

bool D3D12DescriptorAllocator::CanReusePreviousDrawTable(const DescriptorPlan& plan,
                                                         bool sampler) const {
    const auto& previous = previous_draw_tables[sampler ? 1 : 0];
    if (descriptor_fastpath) {
        const auto& cache = sampler ? sampler_cache : view_cache;
        if (plan.sources.Current(cache.generation)) {
            return previous.tick == context.CurrentTick() && previous.plan.sizes == plan.sizes &&
                   previous.plan.sources.Matches(plan.sources, cache.generation);
        }
    }
    return previous.tick == context.CurrentTick() && previous.plan.sizes == plan.sizes &&
           previous.plan.keys == plan.keys;
}

const DescriptorTable* D3D12DescriptorAllocator::FindHandleDrawTable(const DescriptorPlan& plan,
                                                                     bool sampler) const {
    const auto& cache = sampler ? sampler_cache : view_cache;
    if (!handle_table_reuse || !descriptor_fastpath || !plan.sources.Current(cache.generation))
        return nullptr;
    return handle_table_cache[sampler ? 1 : 0].Find(
        context.CurrentTick(), cache.generation, plan.sizes,
        {plan.sources.handles.data(), plan.sources.count});
}

const D3D12DescriptorAllocator::CachedDrawTable* D3D12DescriptorAllocator::FindCachedDrawTable(
    const DescriptorPlan& plan, bool sampler) const {
    if (!descriptor_table_cache)
        return nullptr;
    return draw_table_cache[sampler ? 1 : 0].Find(context.CurrentTick(), plan.sizes, plan.keys);
}

void D3D12DescriptorAllocator::RememberDrawTable(const DescriptorPlan& plan, DescriptorTable table,
                                                 bool sampler) {
    const auto& cache = sampler ? sampler_cache : view_cache;
    if (handle_table_reuse && descriptor_fastpath && plan.sources.Current(cache.generation)) {
        // Resource transitions retain every backing resource until tick completion;
        // cached GPU slots are immutable and never reused across that tick.
        handle_table_cache[sampler ? 1 : 0].Remember(
            context.CurrentTick(), cache.generation, plan.sizes,
            {plan.sources.handles.data(), plan.sources.count}, table);
    }
    if (descriptor_table_cache)
        draw_table_cache[sampler ? 1 : 0].Remember(context.CurrentTick(), plan.sizes, plan.keys,
                                                   {table, plan.resources});
}

DescriptorTable D3D12DescriptorAllocator::FinishDrawTable(DescriptorPlan& plan, bool sampler) {
    if (!plan.initialized.empty()) {
        for (u32 slot = 0; slot < plan.keys.size(); ++slot) {
            if (!plan.initialized[slot]) {
                const auto message =
                    fmt::format("D3D12_VALIDATION invalid tick={} call=FinishDrawTable sampler={} "
                                "unwritten_slot={} count={}",
                                context.CurrentTick(), sampler, slot, plan.keys.size());
                Diagnostic(message);
                throw std::invalid_argument(message);
            }
        }
    }
    auto& previous = previous_draw_tables[sampler ? 1 : 0];
    if (CanReusePreviousDrawTable(plan, sampler)) {
        context.Profile().Count(FrameCount::DescriptorTableReuses);
        return previous.table;
    }
    // Recheck after Reserve: a submission retires the entire cache's GPU slots.
    if (const auto* cached = FindHandleDrawTable(plan, sampler)) {
        context.Profile().Count(FrameCount::DescriptorTableReuses);
        previous.tick = context.CurrentTick();
        std::swap(previous.plan, plan);
        previous.table = *cached;
        return *cached;
    }
    if (const auto* cached = FindCachedDrawTable(plan, sampler)) {
        context.Profile().Count(FrameCount::DescriptorTableReuses);
        previous.tick = context.CurrentTick();
        std::swap(previous.plan, plan);
        previous.table = cached->table;
        return cached->table;
    }
    const u32 count = static_cast<u32>(plan.keys.size());
    const auto table = sampler ? AllocateSamplers(count) : AllocateViews(count);
    const auto type =
        sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    const FrameProfile::Scope profile_write{context.Profile(), FramePhase::DescriptorWrites};
    RefreshCaches(); // Reserve may have submitted and invalidated CPU sources.
    const auto& cache = sampler ? sampler_cache : view_cache;
    if (descriptor_fastpath && plan.sources.Current(cache.generation)) {
        // The destination is contiguous. Merge adjacent CPU sources, but keep
        // repeated null handles separate so every GPU slot gets initialized.
        if (descriptor_ranges) {
            DescriptorCopyRanges<D3D12_CPU_DESCRIPTOR_HANDLE, 4096> sources;
            for (u32 slot = 0; slot < count; ++slot) {
                const D3D12_CPU_DESCRIPTOR_HANDLE handle{plan.sources.handles[slot]};
                sources.Add(handle, cache.stride, IsNullDescriptorHandle(handle));
            }
            context.Device()->CopyDescriptors(1, &table.cpu, &count,
                                              static_cast<UINT>(sources.count),
                                              sources.starts.data(), sources.sizes.data(), type);
        } else {
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4096> sources;
            for (u32 slot = 0; slot < count; ++slot)
                sources[slot].ptr = plan.sources.handles[slot];
            context.Device()->CopyDescriptors(1, &table.cpu, &count, count, sources.data(), nullptr,
                                              type);
        }
        TraceApiCall(context.Device(), "CopyDescriptors", nullptr, &type, sizeof(type));
        context.Profile().Count(FrameCount::DescriptorCopyCalls);
        context.Profile().Count(FrameCount::DescriptorCopies, count);
        RememberDrawTable(plan, table, sampler);
        previous.tick = context.CurrentTick();
        std::swap(previous.plan, plan);
        previous.table = table;
        return table;
    }
    static const bool batch_copies = [] {
        const char* value = std::getenv("NXBOX_D3D12_BATCH_DESCRIPTOR_COPIES");
        return !value || std::strcmp(value, "0") != 0;
    }();
    std::optional<CopyBatch> batch;
    if (batch_copies)
        batch.emplace();
    try {
        for (u32 slot = 0; slot < count; ++slot) {
            const auto& key = plan.keys[slot];
            WriteCached(type, plan.resources[slot], key.kind, key.description.data(), key.size,
                        table.Slot(slot), batch ? &*batch : nullptr);
        }
    } catch (...) {
        // slot_contents already describes queued copies. Complete them even if a later
        // cache allocation fails, so the next draw cannot mistake an unwritten slot for a hit.
        if (batch)
            FlushCopies(type, *batch);
        throw;
    }
    if (batch)
        FlushCopies(type, *batch);
    RememberDrawTable(plan, table, sampler);
    previous.tick = context.CurrentTick();
    // Retain the new keys/resources and return the old storage to the draw scratch.
    // Move assignment would discard that storage and allocate again on the next draw.
    std::swap(previous.plan, plan);
    previous.table = table;
    return table;
}

void D3D12DescriptorAllocator::WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                           const ResourcePtr& resource, u32 kind, const void* desc,
                                           size_t size, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                           CopyBatch* batch) {
    RefreshCaches();
    const auto key = MakeDescriptorKey(resource, kind, desc, size);
    const auto old_contents = slot_contents.find(destination.ptr);
    if (old_contents != slot_contents.end() && old_contents->second == key)
        return; // The slot has not changed; never overwrite an in-flight table.
    const auto source = ResolveDescriptor(type, resource, key, batch);
    if (batch) {
        batch->Add(destination, source,
                   [&](size_t count, const auto* destinations, const auto* sources) {
                       CopyDescriptors(type, count, destinations, sources);
                   });
    } else {
        context.Device()->CopyDescriptorsSimple(1, destination, source, type);
        TraceApiCall(context.Device(), "CopyDescriptorsSimple", nullptr, &type, sizeof(type));
        context.Profile().Count(FrameCount::DescriptorCopyCalls);
    }
    context.Profile().Count(FrameCount::DescriptorCopies);
    slot_contents[destination.ptr] = std::move(key);
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::ResolveDescriptor(
    D3D12_DESCRIPTOR_HEAP_TYPE type, const ResourcePtr& resource, const DescriptorKey& key,
    CopyBatch* batch) {
    const auto kind = key.kind;
    const void* desc = key.description.data();
    // Null views own no resources and never change. Keep their CPU slots out
    // of the frame/rollover cache; a full permanent heap falls back without
    // recycling any handles borrowed by an unfinished descriptor plan.
    const bool null_view = persistent_null_descriptors && !resource && kind < 3;
    if (null_view) {
        if (const auto found = null_view_cache.entries.find(key);
            found != null_view_cache.entries.end())
            return found->second.handle;
    }
    auto& cache = null_view && null_view_cache.cursor < null_view_cache.capacity ? null_view_cache
                  : type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER                   ? sampler_cache
                                                                                 : view_cache;
    auto found = cache.entries.find(key);
    if (found == cache.entries.end()) {
        if (cache.cursor == cache.capacity) {
            // Pending CPU copies must consume these handles before they are recycled.
            if (batch)
                FlushCopies(type, *batch);
            // Copies are synchronous; no GPU fence is needed to recycle CPU descriptors.
            cache.entries.clear();
            slot_contents.clear();
            cache.cursor = 0;
            ++cache.generation;
        }
        auto handle = cache.cpu;
        handle.ptr += static_cast<SIZE_T>(cache.cursor++) * cache.stride;
        switch (kind) {
        case 0: {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
            std::memcpy(&cbv, desc, sizeof(cbv));
            context.Device()->CreateConstantBufferView(cbv.BufferLocation ? &cbv : nullptr, handle);
            TraceApiCall(context.Device(), "CreateConstantBufferView", nullptr, &cbv, sizeof(cbv));
            break;
        }
        case 1: {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            std::memcpy(&srv, desc, sizeof(srv));
            if (BisectEnabled("NXBOX_D3D12_VALIDATE_COMMANDS") && !resource) {
                std::string words;
                for (size_t offset = 0; offset < sizeof(srv); offset += sizeof(u32)) {
                    u32 word{};
                    std::memcpy(&word, reinterpret_cast<const u8*>(&srv) + offset,
                                std::min(sizeof(word), sizeof(srv) - offset));
                    words += fmt::format("{}{:08x}", offset ? " " : "", word);
                }
                Diagnostic(fmt::format(
                    "D3D12_VALIDATION null_srv tick={} dimension={} format={} desc=[{}]",
                    context.CurrentTick(), static_cast<UINT>(srv.ViewDimension),
                    static_cast<UINT>(srv.Format), words));
            }
            context.ValidateViewFormat(resource, srv.Format, "CreateShaderResourceView");
            context.Device()->CreateShaderResourceView(resource ? resource->handle.Get() : nullptr,
                                                       &srv, handle);
            TraceApiCall(context.Device(), "CreateShaderResourceView",
                         resource ? resource->handle.Get() : nullptr, &srv, sizeof(srv));
            break;
        }
        case 2: {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            std::memcpy(&uav, desc, sizeof(uav));
            context.ValidateViewFormat(resource, uav.Format, "CreateUnorderedAccessView");
            context.Device()->CreateUnorderedAccessView(resource ? resource->handle.Get() : nullptr,
                                                        nullptr, &uav, handle);
            TraceApiCall(context.Device(), "CreateUnorderedAccessView",
                         resource ? resource->handle.Get() : nullptr, &uav, sizeof(uav));
            break;
        }
        case 3: {
            D3D12_SAMPLER_DESC sampler{};
            std::memcpy(&sampler, desc, sizeof(sampler));
            context.Device()->CreateSampler(&sampler, handle);
            TraceApiCall(context.Device(), "CreateSampler", nullptr, &sampler, sizeof(sampler));
            break;
        }
        default:
            throw std::invalid_argument("D3D12 unknown cached descriptor kind");
        }
        context.Profile().Count(FrameCount::DescriptorWrites);
        constexpr std::array by_kind{FrameCount::DescriptorWritesCbv, FrameCount::DescriptorWritesSrv,
                                     FrameCount::DescriptorWritesUav,
                                     FrameCount::DescriptorWritesSampler};
        context.Profile().Count(by_kind[kind]);
        found = cache.entries.emplace(key, CachedDescriptor{handle, resource}).first;
    }
    return found->second.handle;
}

void D3D12DescriptorAllocator::CopyDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE type, size_t count,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE* sources) {
    if (descriptor_ranges) {
        DescriptorCopyRanges<D3D12_CPU_DESCRIPTOR_HANDLE, 32> destination_ranges, source_ranges;
        const auto stride =
            type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? sampler_cache.stride : view_cache.stride;
        for (size_t index = 0; index < count; ++index) {
            destination_ranges.Add(destinations[index], stride);
            source_ranges.Add(sources[index], stride, IsNullDescriptorHandle(sources[index]));
        }
        context.Device()->CopyDescriptors(
            static_cast<UINT>(destination_ranges.count), destination_ranges.starts.data(),
            destination_ranges.sizes.data(), static_cast<UINT>(source_ranges.count),
            source_ranges.starts.data(), source_ranges.sizes.data(), type);
    } else {
        // Null range-size arrays mean one descriptor per range, including noncontiguous slots.
        context.Device()->CopyDescriptors(static_cast<UINT>(count), destinations, nullptr,
                                          static_cast<UINT>(count), sources, nullptr, type);
    }
    TraceApiCall(context.Device(), "CopyDescriptors", nullptr, &type, sizeof(type));
    context.Profile().Count(FrameCount::DescriptorCopyCalls);
}

void D3D12DescriptorAllocator::FlushCopies(D3D12_DESCRIPTOR_HEAP_TYPE type, CopyBatch& batch) {
    batch.Flush([&](size_t count, const auto* destinations, const auto* sources) {
        CopyDescriptors(type, count, destinations, sources);
    });
}

std::optional<DescriptorTable> D3D12DescriptorAllocator::FindTable(std::string_view key,
                                                                   bool sampler) {
    RefreshCaches();
    return table_cache[sampler ? 1 : 0].Find(context.CurrentTick(), key);
}
void D3D12DescriptorAllocator::RememberTable(std::string key, DescriptorTable table, bool sampler) {
    RefreshCaches();
    table_cache[sampler ? 1 : 0].Remember(context.CurrentTick(), std::move(key), table);
}
void D3D12DescriptorAllocator::WriteUavDescriptor(const DescriptorTable& table, u32 slot,
                                                  const ResourcePtr& resource,
                                                  const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc) {
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, resource, 2, &desc, sizeof(desc), table,
                slot);
}
void D3D12DescriptorAllocator::Init(Ring& ring, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity,
                                    bool visible) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags =
        visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Diagnostic("D3D12 descriptor heap type=" + std::to_string(static_cast<u32>(type)) +
               " capacity=" + std::to_string(capacity) + " visible=" + std::to_string(visible));
    Check(context.Device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&ring.heap)),
          "CreateDescriptorHeap", context.Device());
    ring.stride = context.Device()->GetDescriptorHandleIncrementSize(type);
    ring.cpu = ring.heap->GetCPUDescriptorHandleForHeapStart();
    ring.shader_visible = visible;
    if (visible)
        ring.gpu = ring.heap->GetGPUDescriptorHandleForHeapStart();
}

void D3D12DescriptorAllocator::Reserve(u32 view_count, u32 sampler_count, u32 rtv_count,
                                       u32 dsv_count) {
    const std::array rings{&views, &samplers, &rtvs, &dsvs};
    const std::array counts{view_count, sampler_count, rtv_count, dsv_count};
    for (size_t i = 0; i < rings.size(); ++i) {
        if (counts[i] > rings[i]->math.Capacity())
            throw std::runtime_error("D3D12 descriptor batch larger than its heap");
    }
    for (;;) {
        // Retire callbacks before the preflight, then keep this recording open.
        // Every subsequent allocation in this batch must use the same fence tick.
        context.Commands();
        bool ready = true;
        for (size_t i = 0; i < rings.size(); ++i) {
            auto& ring = *rings[i];
            while (const auto oldest = ring.math.OldestTick()) {
                if (!context.IsFree(*oldest))
                    break;
                ring.math.Retire(*oldest);
            }
            ready &= counts[i] == 0 || ring.math.CanAllocate(counts[i]);
        }
        if (ready)
            return;
        // Submit the producer if needed, then retire only the oldest blocking
        // allocation rather than draining every in-flight command allocator.
        u64 oldest_blocked = context.CurrentTick();
        for (size_t i = 0; i < rings.size(); ++i) {
            if (counts[i] && !rings[i]->math.CanAllocate(counts[i]))
                oldest_blocked = std::min(oldest_blocked, *rings[i]->math.OldestTick());
        }
        if (oldest_blocked == context.CurrentTick())
            context.Submit(FrameCount::ForcedSubmitDescriptorExhaustion);
        context.Wait(oldest_blocked);
    }
}

void D3D12DescriptorAllocator::SetHeaps(CommandFacade* commands, bool cache_draw_state) {
    const std::array heaps{views.heap.Get(), samplers.heap.Get()};
    if (cache_draw_state) {
        auto& state = context.DrawState();
        if (!context.RecordState(state.heaps.Update(heaps)))
            return;
        // Changing descriptor heaps invalidates descriptor tables. Clearing all
        // root arguments is conservative and keeps their tracking simple.
        state.ClearRootArguments();
    } else {
        context.InvalidateRecordedState();
    }
    commands->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
}

DescriptorTable D3D12DescriptorAllocator::Allocate(Ring& ring, u32 count) {
    if (!count || count > ring.math.Capacity())
        throw std::invalid_argument("D3D12 invalid descriptor allocation size");
    for (;;) {
        context.Commands();
        while (const auto oldest = ring.math.OldestTick()) {
            if (!context.IsFree(*oldest))
                break;
            ring.math.Retire(*oldest);
        }
        if (const auto offset = ring.math.Allocate(count, context.CurrentTick())) {
            DescriptorTable table;
            table.count = count;
            table.stride = ring.stride;
            table.cpu = {ring.cpu.ptr + static_cast<SIZE_T>(*offset) * ring.stride};
            if (ring.shader_visible)
                table.gpu = {ring.gpu.ptr + static_cast<UINT64>(*offset) * ring.stride};
            return table;
        }
        const auto oldest = ring.math.OldestTick();
        if (!oldest)
            throw std::runtime_error("D3D12 descriptor table larger than its heap");
        // Everything live belongs to the open command list: close it so it can retire.
        if (*oldest >= context.CurrentTick())
            context.Submit(FrameCount::ForcedSubmitDescriptorExhaustion);
        else
            context.Wait(*oldest, "descriptor");
    }
}

DescriptorTable D3D12DescriptorAllocator::AllocateViews(u32 count) {
    return Allocate(views, count);
}
DescriptorTable D3D12DescriptorAllocator::AllocateSamplers(u32 count) {
    return Allocate(samplers, count);
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::AllocateRtv(u32 count) {
    return Allocate(rtvs, count).cpu;
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::AllocateDsv() {
    return Allocate(dsvs, 1).cpu;
}

void D3D12DescriptorAllocator::WriteNullCbv(const DescriptorTable& table, u32 slot) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {}, 0, &desc, sizeof(desc), table, slot);
}
void D3D12DescriptorAllocator::WriteNullSrv(const DescriptorTable& table, u32 slot,
                                            D3D12_SRV_DIMENSION dimension) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = dimension;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (dimension) {
    case D3D12_SRV_DIMENSION_BUFFER:
        desc.Buffer.NumElements = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        desc.Texture1D.MipLevels = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        desc.Texture1DArray.MipLevels = desc.Texture1DArray.ArraySize = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        desc.Texture2DArray.MipLevels = desc.Texture2DArray.ArraySize = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
        desc.Texture2DMSArray.ArraySize = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        desc.Texture3D.MipLevels = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        desc.TextureCube.MipLevels = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        desc.TextureCubeArray.MipLevels = desc.TextureCubeArray.NumCubes = 1;
        break;
    default:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;
        break;
    }
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {}, 1, &desc, sizeof(desc), table, slot);
}
void D3D12DescriptorAllocator::WriteNullUav(const DescriptorTable& table, u32 slot) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    WriteUavDescriptor(table, slot, {}, desc);
}
void D3D12DescriptorAllocator::WriteNullSampler(const DescriptorTable& table, u32 slot) {
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, {}, 3, &desc, sizeof(desc), table, slot);
}

void D3D12DescriptorAllocator::WriteCbv(const DescriptorTable& table, u32 slot,
                                        const BoundBuffer& buffer) {
    if (!buffer.resource || !buffer.size) {
        WriteNullCbv(table, slot);
        return;
    }
    const u64 width = buffer.resource->handle->GetDesc().Width;
    u64 size = Common::AlignUp<u64>(buffer.size, 256);
    // Diagnostic: view the whole 64 KiB the HLSL cbuffer declares, so reads past the guest
    // size see memory instead of the zeros AMD returns for out-of-bounds CBV reads.
    static const bool full_cbv = BisectEnabled("NXBOX_D3D12_CBV_FULL");
    if (full_cbv)
        size = 65536;
    if (buffer.offset >= width) {
        size = 0;
    } else if (buffer.offset + size > width) {
        // CBV ranges must be 256-byte multiples: drop the (sub-256) tail past the allocation.
        size = (width - buffer.offset) & ~u64{255};
    }
    if (!size) {
        WriteNullCbv(table, slot);
        return;
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    desc.BufferLocation = buffer.resource->handle->GetGPUVirtualAddress() + buffer.offset;
    desc.SizeInBytes = static_cast<UINT>(size);
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, buffer.resource, 0, &desc, sizeof(desc),
                table, slot);
}

void D3D12DescriptorAllocator::WriteStorageUav(const DescriptorTable& table, u32 slot,
                                               const BoundBuffer& buffer) {
    const u32 size = buffer.size & ~3u;
    if (!buffer.resource || !size) {
        WriteNullUav(table, slot);
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = buffer.offset / 4;
    desc.Buffer.NumElements = size / 4;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    WriteUavDescriptor(table, slot, buffer.resource, desc);
}

void D3D12DescriptorAllocator::WriteTexelSrv(const DescriptorTable& table, u32 slot,
                                             const BoundBuffer& buffer) {
    const auto format =
        MaxwellFormat(static_cast<VideoCore::Surface::PixelFormat>(buffer.pixel_format));
    if (!buffer.resource || !format.Supported() || !format.bytes_per_block ||
        buffer.size < format.bytes_per_block) {
        WriteNullSrv(table, slot, D3D12_SRV_DIMENSION_BUFFER);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(format.view_format);
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = buffer.offset / format.bytes_per_block;
    desc.Buffer.NumElements = buffer.size / format.bytes_per_block;
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, buffer.resource, 1, &desc, sizeof(desc),
                table, slot);
}

void D3D12DescriptorAllocator::WriteTexelUav(const DescriptorTable& table, u32 slot,
                                             const BoundBuffer& buffer) {
    const auto format =
        MaxwellFormat(static_cast<VideoCore::Surface::PixelFormat>(buffer.pixel_format));
    if (!buffer.resource || !format.Supported() || !format.bytes_per_block ||
        buffer.size < format.bytes_per_block) {
        WriteNullUav(table, slot);
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(format.view_format);
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = buffer.offset / format.bytes_per_block;
    desc.Buffer.NumElements = buffer.size / format.bytes_per_block;
    WriteUavDescriptor(table, slot, buffer.resource, desc);
}

bool D3D12DescriptorAllocator::WriteImageSrv(const DescriptorTable& table, u32 slot,
                                             const ImageView& view,
                                             D3D12_SRV_DIMENSION fallback_dimension,
                                             const ResourcePtr& replacement,
                                             D3D12_SHADER_RESOURCE_VIEW_DESC* resolved) {
    // Diagnostic output only. A false return denotes a null binding; do not report the
    // original guest descriptor as though it were the adapted descriptor we actually bind.
    if (resolved) {
        *resolved = {};
        resolved->ViewDimension = fallback_dimension;
    }
    const auto resource = replacement ? replacement : view.ImageHandle();
    if (!resource) {
        WriteNullSrv(table, slot, fallback_dimension);
        return false;
    }
    auto desc = view.Description();
    if (desc.ViewDimension != fallback_dimension) {
        const auto backing_desc = resource->handle->GetDesc();
        if (const auto adapted = AdaptSrvDimension(desc, fallback_dimension, backing_desc)) {
            desc = *adapted;
        } else {
            // A 2D texture with fewer than 6 layers has no faces for a cube slot. Mario Party
            // Superstars binds an 8x8 one: the Xbox driver removed the device
            // (DXGI_ERROR_INVALID_CALL) half a second later, so that slot reads zeros like an
            // unbound one. Other mismatches keep the unadapted view, as before: MK8D binds later
            // cubes of a cube array to cube slots, and a null SRV in their place has not been
            // checked on the console.
            // A 3D slot needs a 3D resource and the reverse: no view can bridge resource
            // dimensions (Mario Party binds the same 8x8 2D texture to a 3D slot, and the
            // device was removed with DXGI_ERROR_INVALID_CALL right after).
            const bool wrong_resource =
                (fallback_dimension == D3D12_SRV_DIMENSION_TEXTURE3D) !=
                (backing_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
            const bool no_faces = wrong_resource ||
                                  ((fallback_dimension == D3D12_SRV_DIMENSION_TEXTURECUBE ||
                                    fallback_dimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY) &&
                                   backing_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                                   backing_desc.SampleDesc.Count == 1 &&
                                   backing_desc.DepthOrArraySize < 6);
            if (logged_srv_mismatches.insert(desc.ViewDimension * 16 + fallback_dimension)
                    .second) {
                Diagnostic(fmt::format(
                    "D3D12 SRV dimension {} cannot become shader dimension {} "
                    "(resource={} {}x{} depth/layers={} samples={} view_type={}){}",
                    static_cast<int>(desc.ViewDimension), static_cast<int>(fallback_dimension),
                    static_cast<int>(backing_desc.Dimension), backing_desc.Width,
                    backing_desc.Height, backing_desc.DepthOrArraySize,
                    backing_desc.SampleDesc.Count, static_cast<int>(view.type),
                    no_faces ? ", bound null" : ""));
            }
            if (no_faces) {
                WriteNullSrv(table, slot, fallback_dimension);
                return false;
            }
        }
    }
    // NXBOX_D3D12_ARRAY_TRACE=1: once per address, report array-sampling shaders whose view
    // exposes fewer slices than the resource holds (the sampled layer clamps to the last one).
    static const bool array_trace = BisectEnabled("NXBOX_D3D12_ARRAY_TRACE");
    if (array_trace && fallback_dimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
        const auto resource_desc = resource->handle->GetDesc();
        const UINT slices = desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                ? desc.Texture2DArray.ArraySize
                                : 1;
        static std::unordered_set<u64> traced;
        if (traced.insert(view.gpu_addr).second) {
            Diagnostic(fmt::format(
                "D3D12_ARRAY_TRACE gpu_addr={:#x} view_type={} view_dim={} first={} slices={} "
                "resource_layers={} width={} height={} format={}",
                view.gpu_addr, static_cast<int>(view.type), static_cast<int>(desc.ViewDimension),
                desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                    ? desc.Texture2DArray.FirstArraySlice
                    : 0,
                slices, resource_desc.DepthOrArraySize, resource_desc.Width, resource_desc.Height,
                static_cast<int>(resource_desc.Format)));
        }
    }
    if (resolved)
        *resolved = desc;
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, resource, 1, &desc, sizeof(desc), table,
                slot);
    return true;
}

void D3D12DescriptorAllocator::WriteSampler(const DescriptorTable& table, u32 slot,
                                            const Sampler& sampler, bool shader_compares) {
    const auto desc = MakeSamplerDesc(sampler.Entry(), shader_compares);
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, {}, 3, &desc, sizeof(desc), table, slot);
}

void D3D12DescriptorAllocator::CreateRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                         const ImageView* view_ptr, const ResourcePtr& replacement,
                                         u32 layer_count) {
    const FrameProfile::Scope profile_write{context.Profile(), FramePhase::DescriptorWrites};
    context.Profile().Count(FrameCount::DescriptorWrites);
    const auto resource = replacement ? replacement : view_ptr ? view_ptr->ImageHandle() : nullptr;
    if (!resource) {
        // Null render target: a valid RTV that discards writes.
        D3D12_RENDER_TARGET_VIEW_DESC null_desc{};
        null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        null_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        context.Device()->CreateRenderTargetView(nullptr, &null_desc, handle);
        return;
    }
    const ImageView& view = *view_ptr;
    const auto desc = resource->handle->GetDesc();
    const UINT level = replacement ? 0 : static_cast<UINT>(view.range.base.level);
    const UINT layer = replacement ? 0 : static_cast<UINT>(view.range.base.layer);
    const UINT layers = layer_count ? layer_count : static_cast<UINT>(view.range.extent.layers);
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = view.Description().Format;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        rtv.Texture3D = {level, 0, static_cast<UINT>(-1)};
    } else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
        rtv.Texture1DArray = {level, layer, layers};
    } else if (desc.SampleDesc.Count > 1) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        rtv.Texture2DMSArray = {layer, layers};
    } else {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv.Texture2DArray = {level, layer, layers, 0};
    }
    context.ValidateViewFormat(resource, rtv.Format, "CreateRenderTargetView");
    context.Device()->CreateRenderTargetView(resource->handle.Get(), &rtv, handle);
    TraceApiCall(context.Device(), "CreateRenderTargetView", resource->handle.Get(), &rtv, sizeof(rtv));
}

void D3D12DescriptorAllocator::CreateDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const ImageView& view,
                                         D3D12_DSV_FLAGS flags, u32 layer_count) {
    const FrameProfile::Scope profile_write{context.Profile(), FramePhase::DescriptorWrites};
    context.Profile().Count(FrameCount::DescriptorWrites);
    const auto resource = view.ImageHandle();
    if (!resource)
        throw std::runtime_error("D3D12 depth view without image");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DsvFormat(view.Description().Format);
    dsv.Flags = flags;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    if (resource->handle->GetDesc().SampleDesc.Count > 1) {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        dsv.Texture2DMSArray = {static_cast<UINT>(view.range.base.layer),
                                layer_count ? layer_count
                                            : static_cast<UINT>(view.range.extent.layers)};
    } else {
        dsv.Texture2DArray = {
            static_cast<UINT>(view.range.base.level), static_cast<UINT>(view.range.base.layer),
            layer_count ? layer_count : static_cast<UINT>(view.range.extent.layers)};
    }
    context.ValidateViewFormat(resource, dsv.Format, "CreateDepthStencilView");
    context.Device()->CreateDepthStencilView(resource->handle.Get(), &dsv, handle);
    TraceApiCall(context.Device(), "CreateDepthStencilView", resource->handle.Get(), &dsv, sizeof(dsv));
}
bool D3D12DescriptorAllocator::WriteImageUav(const DescriptorTable& table, u32 slot,
                                             const ImageView& view, DXGI_FORMAT format) {
    if (!view.ImageHandle()) {
        WriteNullUav(table, slot);
        return false;
    }
    if (!(view.ImageHandle()->handle->GetDesc().Flags &
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        view.LogUnsupported(
            "storage image lacks ALLOW_UNORDERED_ACCESS (MSAA UAVs are unsupported)");
        WriteNullUav(table, slot);
        return false;
    }
    const auto& srv = view.Description();
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = format;
    switch (srv.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
        desc.Texture1D.MipSlice = srv.Texture1D.MostDetailedMip;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        desc.Texture1DArray = {srv.Texture1DArray.MostDetailedMip,
                               srv.Texture1DArray.FirstArraySlice, srv.Texture1DArray.ArraySize};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Texture2D = {srv.Texture2D.MostDetailedMip, 0};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {srv.Texture2DArray.MostDetailedMip,
                               srv.Texture2DArray.FirstArraySlice, srv.Texture2DArray.ArraySize, 0};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {srv.Texture3D.MostDetailedMip, 0, static_cast<UINT>(-1)};
        break;
    default:
        throw std::runtime_error("D3D12 unsupported storage image dimension");
    }
    WriteUavDescriptor(table, slot, view.ImageHandle(), desc);
    return true;
}
} // namespace D3D12
