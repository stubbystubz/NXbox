// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "common/alignment.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "core/memory.h"
#include "shader_recompiler/backend/hlsl/hlsl_draw_texture.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_buffer_view.h"
#include "video_core/renderer_d3d12/d3d12_feedback.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_render_area.h"
#include "video_core/renderer_d3d12/d3d12_resolution.h"
#include "video_core/shader_discovery.h"
#include "video_core/submission_trace.h"
#include "video_core/textures/texture.h"

namespace D3D12 {

void RasterizerD3D12::PrepareZeroUniformBuffer() {
    if (!zero_uniform_buffer) {
        // Root CBVs cannot be null; cover the full 64 KiB guest cbuffer declaration.
        zero_uniform_buffer = context.CreateBuffer(65536, D3D12_HEAP_TYPE_UPLOAD);
        std::fill(zero_uniform_buffer->mapped.begin(), zero_uniform_buffer->mapped.end(), u8{0});
    }
}

AccelerateDMA::AccelerateDMA() = default;

bool AccelerateDMA::BufferCopy(GPUVAddr start_address, GPUVAddr end_address, u64 amount) {
    return false;
}
bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    return false;
}

RasterizerD3D12::RasterizerD3D12(Tegra::GPU& gpu, Tegra::MaxwellDeviceMemoryManager& memory,
                                 Context& context)
    : context{context}, device_memory{memory}, descriptors{context},
      buffer_runtime{context, bindings}, texture_runtime{context},
      buffer_cache{memory, buffer_runtime}, texture_cache{texture_runtime, memory},
      index_converter{context}, clear_renderer{context}, compute_dispatch{context},
      transform_feedback{context}, query_runtime{context, memory, buffer_cache, transform_feedback},
      query_cache{gpu, *this, memory, query_runtime},
      fence_manager{*this, gpu, texture_cache, buffer_cache, query_cache, context}, m_gpu{gpu} {
    // Root signatures are bounded by 64 parameters; never grow draw scratch.
    draw_root_cbv_addresses.reserve(64);
    if (BisectEnabled("NXBOX_GPU_PIPELINE")) {
        gpu_pipeline = std::make_unique<VideoCommon::GpuDrawPipeline>();
        gpu_pipeline->Invoke([&gpu] {
            gpu.RegisterHostThread();
            Common::SetCurrentThreadName("GPU draw builder");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
            Common::SetCurrentThreadToPerformanceCores();
        });
    }
    VideoCommon::SubmissionTrace::Reset();
    VideoCommon::ShaderDiscovery::Reset();
    default_pipeline_cache = std::make_unique<PipelineCache>(context, memory);
    pipeline_cache = default_pipeline_cache.get();
    context.Profile().guest_visibility = [this] {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        return query_runtime.VisibilityDiagnostics() +
               " pending_guest_fences=" + std::to_string(fence_manager.PendingGuestFences()) +
               " pending_guest_callbacks=" + std::to_string(fence_manager.PendingGuestCallbacks()) +
               " uncommitted_buffer_writes=" +
               std::to_string(buffer_cache.HasUncommittedFlushes()) +
               " uncommitted_texture_writes=" +
               std::to_string(texture_cache.HasUncommittedFlushes()) +
               default_pipeline_cache->ProfileSummary() +
               " attachment_pool_entries=" + std::to_string(attachment_pool.size()) +
               " feedback_pool_entries=" + std::to_string(feedback_pool.size()) +
               " native_memory_bytes=" + std::to_string(this->context.MemoryUsage());
    };
}
RasterizerD3D12::~RasterizerD3D12() {
    // Stop publication and join while snapshots, channels and caches are still alive.
    if (gpu_pipeline) {
        gpu_pipeline->Close();
        gpu_pipeline.reset();
    }
    VideoCommon::ShaderDiscovery::Shutdown();
    context.Profile().guest_visibility = {};
    // Query callbacks capture the cache. Drain while every cache/runtime is alive.
    try {
        fence_manager.SignalReference();
        fence_manager.WaitPendingFences(true);
        context.Finish(FrameCount::ForcedSubmitLifecycle);
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12 rasterizer teardown: {}", error.what());
    }
}

namespace {
// Format support of this device, asked once per format (clears and storage image binds ask
// per call).
D3D12_FEATURE_DATA_FORMAT_SUPPORT FormatSupport(ID3D12Device* device, DXGI_FORMAT format) {
    static std::mutex mutex;
    static std::unordered_map<DXGI_FORMAT, D3D12_FEATURE_DATA_FORMAT_SUPPORT> known;
    std::scoped_lock lock{mutex};
    const auto [entry, fresh] = known.try_emplace(format, D3D12_FEATURE_DATA_FORMAT_SUPPORT{format});
    if (fresh && FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &entry->second,
                                                    sizeof(entry->second))))
        entry->second = {format};
    return entry->second;
}
bool SupportsFormat(ID3D12Device* device, DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 required) {
    const auto support = static_cast<UINT>(FormatSupport(device, format).Support1);
    return (support & static_cast<UINT>(required)) == static_cast<UINT>(required);
}
using Maxwell = Tegra::Engines::Maxwell3D::Regs;
// CPU expansion preserves the guest multi-draw ID independently of base IDs.
// Scoped to the GPU thread and restored after each indirect batch.
thread_local u32 indirect_draw_id{};

DrawParams MakeDrawParams(const Tegra::Engines::Maxwell3D::DrawManager::State& draw_state,
                          u32 num_instances, bool is_indexed) {
    DrawParams params{
        .base_instance = draw_state.base_instance,
        .num_instances = num_instances,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .is_indexed = is_indexed,
    };
    // Quads are drawn as 6 indexed triangle vertices per quad; the generated indices already
    // include the first vertex (see BufferCacheRuntime::BindQuadIndexBuffer).
    if (draw_state.topology == Maxwell::PrimitiveTopology::Quads) {
        params.num_vertices = (params.num_vertices / 4) * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    } else if (draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
        params.num_vertices = params.num_vertices < 4 ? 0 : (params.num_vertices - 2) / 2 * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    }
    return params;
}

// Keep the submitted draw parameters available when sizing vertex bindings.
struct DrawCommand {
    DrawParams params;
    void operator()(CommandFacade* commands) const {
        if (!params.num_vertices || !params.num_instances)
            return;
        if (params.is_indexed)
            commands->DrawIndexedInstanced(params.num_vertices, params.num_instances,
                                           params.first_index, static_cast<INT>(params.base_vertex),
                                           params.base_instance);
        else
            commands->DrawInstanced(params.num_vertices, params.num_instances, params.base_vertex,
                                    params.base_instance);
    }
};

// Maxwell maps ndc to window space as y_win = translate_y + ndc_y * scale_y. D3D12 viewports
// cannot have a negative height, so a positive Maxwell height (the Vulkan convention) needs the
// vertex shader to negate Y (y_flip), while a negative one maps directly.
struct ViewportResult {
    D3D12_VIEWPORT viewport;
    bool y_flip;
};
ViewportResult MakeViewport(const Maxwell& regs, size_t index) {
    const auto& src = regs.viewport_transform[index];
    float x = src.translate_x - src.scale_x;
    float width = src.scale_x * 2.0f;
    if (width < 0.0f) {
        x += width;
        width = -width;
    }
    float y = src.translate_y - src.scale_y;
    float height = src.scale_y * 2.0f;
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
        y += static_cast<f32>(regs.surface_clip.height);
        height = -height;
    }
    if (src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY) {
        y += height;
        height = -height;
    }
    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
    if (width == 0.0f)
        width = 1.0f;
    if (height == 0.0f)
        height = 1.0f;
    const bool y_flip = height > 0.0f;
    if (!y_flip) {
        y += height;
        height = -height;
    }
    return {{x, y, std::abs(width), height,
             std::clamp(src.translate_z - src.scale_z * reduce_z, 0.0f, 1.0f),
             std::clamp(src.translate_z + src.scale_z, 0.0f, 1.0f)},
            y_flip};
}

ViewportResult MakeFullViewport(const Maxwell& regs) {
    float x = static_cast<float>(regs.surface_clip.x);
    float y = static_cast<float>(regs.surface_clip.y);
    float width = (std::max)(1.0f, static_cast<float>(regs.surface_clip.width));
    float height = (std::max)(1.0f, static_cast<float>(regs.surface_clip.height));
    // Mirrors the Vulkan backend: lower-left origin means a negative height (no shader flip).
    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    return {{x, y, width, height, 0.0f, 1.0f}, !lower_left};
}

D3D12_RECT MakeScissor(const Maxwell& regs, size_t index) {
    const auto& src = regs.scissor_test[index];
    ClearOperations::Rect clip{0, 0, 16384, 16384};
    if (!regs.viewport_scale_offset_enabled) {
        clip = {static_cast<s32>(regs.surface_clip.x), static_cast<s32>(regs.surface_clip.y),
                static_cast<s32>(regs.surface_clip.x + regs.surface_clip.width),
                static_cast<s32>(regs.surface_clip.y + regs.surface_clip.height)};
    }
    // The untransformed path uses the surface clip as its scissor, like Vulkan.
    // Scissor registers can still contain the preceding transformed pass's bounds.
    if (regs.viewport_scale_offset_enabled && src.enable) {
        clip = ClearOperations::Intersect(
            clip, {static_cast<s32>(src.min_x), static_cast<s32>(src.min_y),
                   static_cast<s32>(src.max_x), static_cast<s32>(src.max_y)});
    }
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
        const auto top = clip.top;
        clip.top = static_cast<s32>(regs.surface_clip.height) - clip.bottom;
        clip.bottom = static_cast<s32>(regs.surface_clip.height) - top;
    }
    clip.left = (std::max)(clip.left, 0);
    clip.top = (std::max)(clip.top, 0);
    return {clip.left, clip.top, (std::max)(clip.left, clip.right),
            (std::max)(clip.top, clip.bottom)};
}

std::optional<VideoCore::Surface::PixelFormat> PixelFormatFromImageFormat(
    Shader::ImageFormat format) {
    using VideoCore::Surface::PixelFormat;
    switch (format) {
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8_UINT;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8_SINT;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::R32G32B32A32_UINT;
    default:
        return std::nullopt;
    }
}

// Null when the storage image can be bound, otherwise why its draw or dispatch is skipped.
const char* StorageImageProblem(Context& context, VideoCore::Surface::PixelFormat format,
                                Shader::ImageFormat image_format, bool is_integer, bool is_read,
                                bool is_written) {
    const bool typeless = image_format == Shader::ImageFormat::Typeless;
    if (!typeless && image_format != Shader::ImageFormat::R8_SINT &&
        image_format != Shader::ImageFormat::R16_SINT)
        return nullptr;
    if (typeless && is_read && !context.TypedUavLoadAdditionalFormats()) {
        // The shader substitutes zero for these reads, as in the SPIR-V backend.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            Diagnostic(
                "D3D12 additional typed UAV loads unsupported; typeless image reads return zero");
        is_read = false;
    }
    // A typeless image is declared float4 or uint4 from the format the shader was built against;
    // a view of the other kind would be reinterpreted.
    if (typeless && VideoCore::Surface::IsPixelFormatInteger(format) != is_integer)
        return "typeless storage image bound with a different numeric kind";
    if (is_read && !context.TypedUavLoadAdditionalFormats())
        return "signed storage image read needs additional typed UAV loads";
    const auto support = FormatSupport(
        context.Device(), static_cast<DXGI_FORMAT>(MaxwellFormat(format).view_format));
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW))
        return "storage image format has no typed UAV";
    if (is_read && !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
        return "storage image format has no typed UAV load";
    if (is_written && !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return "storage image format has no typed UAV store";
    return nullptr;
}

D3D12_SRV_DIMENSION SrvDimension(Shader::TextureType type, bool multisample = false) {
    if (multisample)
        return type == Shader::TextureType::ColorArray2D ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY
                                                         : D3D12_SRV_DIMENSION_TEXTURE2DMS;
    switch (type) {
    case Shader::TextureType::Color1D:
        return D3D12_SRV_DIMENSION_TEXTURE1D;
    case Shader::TextureType::ColorArray1D:
        return D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
    case Shader::TextureType::ColorArray2D:
        return D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    case Shader::TextureType::Color3D:
        return D3D12_SRV_DIMENSION_TEXTURE3D;
    case Shader::TextureType::ColorCube:
        return D3D12_SRV_DIMENSION_TEXTURECUBE;
    case Shader::TextureType::ColorArrayCube:
        return D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    case Shader::TextureType::Buffer:
        return D3D12_SRV_DIMENSION_BUFFER;
    default:
        return D3D12_SRV_DIMENSION_TEXTURE2D;
    }
}

constexpr D3D12_RESOURCE_STATES SrvState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES BufferReadState =
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
bool IsStorageBuffer(const BindingState& bindings, const ResourcePtr& resource) {
    for (const auto& stage : bindings.stages) {
        for (const auto& storage : stage.storage)
            if (storage.resource == resource)
                return true;
        // DBSZ aliases constants with image buffers. Those are UAVs even for read-only
        // shader accesses; keeping the CBV on this resource drops the entire dispatch.
        for (const auto& texel : stage.texel)
            if (texel.image && texel.resource == resource)
                return true;
    }
    return false;
}

} // namespace

void RasterizerD3D12::SkipDraw(const char* reason) {
    VideoCommon::SubmissionTrace::Log("host_skip reason={}", reason);
    // Descriptor slots and recorded copies belong to CurrentTick even if no draw
    // reaches OMSetRenderTargets. Submit that producer so they can retire.
    // Transitions are immediate; this backend never opens a native render pass.
    for (auto& stage : bindings.stages)
        stage.Clear();
    state_scratch.clear();
    converted_indices.reset();
    try {
        query_runtime.CancelDraw();
        context.Submit(FrameCount::ForcedSubmitSkippedDraw);
    } catch (const std::exception& error) {
        context.ReportFailure(error);
    }
    ++skipped_draws;
    context.Profile().Count(FrameCount::SkippedDraws);
    static std::mutex warning_mutex;
    static std::set<std::string> warnings;
    {
        std::scoped_lock lock{warning_mutex};
        if (warnings.emplace(reason).second) {
            Diagnostic(std::string{"D3D12_DRAW_SKIP reason="} + reason);
            LOG_WARNING(Render, "D3D12 draw fallback: {}", reason);
        }
    }
}

ResourcePtr RasterizerD3D12::CopyFeedback(const ResourcePtr& source,
                                          const VideoCommon::SubresourceRange* range) {
    const FrameProfile::Scope profile_feedback{context.Profile(), FramePhase::FeedbackCopies};
    auto desc = source->handle->GetDesc();
    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    FeedbackTexture* selected{};
    const bool partial = range && desc.SampleDesc.Count == 1 &&
                         !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const u32 array_size =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize;
    const u32 subresource_count = desc.MipLevels * array_size;
    // Copies the missing subresources of the view (or everything) into entry.
    const auto copy_into = [&](FeedbackTexture& entry) {
        if (entry.copied.size() != subresource_count)
            entry.copied.assign(subresource_count, false);
        std::vector<u32> missing;
        if (!partial) {
            for (u32 index = 0; index < subresource_count; ++index)
                if (!entry.copied[index])
                    missing.push_back(index);
        } else {
            const u32 first_layer = array_size == 1 ? 0u : static_cast<u32>(range->base.layer);
            const u32 layers = array_size == 1 ? 1u : static_cast<u32>(range->extent.layers);
            for (u32 layer = first_layer; layer < std::min(first_layer + layers, array_size);
                 ++layer) {
                for (u32 mip = static_cast<u32>(range->base.level);
                     mip < std::min<u32>(range->base.level + range->extent.levels, desc.MipLevels);
                     ++mip) {
                    if (!entry.copied[mip + layer * desc.MipLevels])
                        missing.push_back(mip + layer * desc.MipLevels);
                }
            }
        }
        if (missing.empty())
            return;
        context.Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
        context.Transition(entry.resource, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!partial) {
            context.Commands()->CopyResource(entry.resource->handle.Get(), source->handle.Get());
            entry.copied.assign(subresource_count, true);
            context.Profile().Count(FrameCount::FeedbackCopyBytes, source->allocation_size);
            return;
        }
        for (const u32 index : missing) {
            D3D12_TEXTURE_COPY_LOCATION dst{entry.resource->handle.Get(),
                                            D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = index;
            D3D12_TEXTURE_COPY_LOCATION src{source->handle.Get(),
                                            D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = index;
            context.Commands()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            entry.copied[index] = true;
        }
        context.Profile().Count(FrameCount::FeedbackCopyBytes,
                                source->allocation_size * missing.size() / subresource_count);
    };
    static const bool reuse = !BisectEnabled("NXBOX_D3D12_FEEDBACK_COPY_PER_DRAW");
    for (auto& entry : feedback_pool) {
        if (entry.draw == feedback_draw && entry.source == source.get()) {
            copy_into(entry);
            return entry.resource;
        }
        // Maxwell only orders texture reads after earlier draws' attachment writes across
        // a texture barrier, so a copy stays valid until the next coherence point.
        if (reuse && entry.source == source.get() && entry.frame == feedback_frame &&
            entry.epoch == feedback_epoch && entry.source_version == source->content_version) {
            entry.draw = feedback_draw;
            copy_into(entry);
            context.Profile().Count(FrameCount::FeedbackReused);
            return entry.resource;
        }
        const auto other = entry.resource->handle->GetDesc();
        if (entry.draw != feedback_draw && desc.Dimension == other.Dimension &&
            desc.Alignment == other.Alignment && desc.Width == other.Width &&
            desc.Height == other.Height && desc.DepthOrArraySize == other.DepthOrArraySize &&
            desc.MipLevels == other.MipLevels && desc.Format == other.Format &&
            desc.SampleDesc.Count == other.SampleDesc.Count &&
            desc.SampleDesc.Quality == other.SampleDesc.Quality && desc.Layout == other.Layout &&
            desc.Flags == other.Flags) {
            selected = &entry;
        }
    }
    if (!selected) {
        u64 pool_bytes{};
        for (const auto& entry : feedback_pool)
            pool_bytes += entry.resource->allocation_size;
        // A cache target, not a draw limit. Evicted resources remain owned by
        // Context's submission batches until their fences complete. Copies reused
        // on the same queue are ordered after earlier sampling by Transition.
        constexpr u64 PoolTarget = 64ULL << 20;
        std::erase_if(feedback_pool, [this, pool_bytes](const auto& entry) {
            return entry.draw != feedback_draw &&
                   (entry.frame != feedback_frame || pool_bytes >= PoolTarget);
        });
        pool_bytes = 0;
        for (const auto& entry : feedback_pool)
            pool_bytes += entry.resource->allocation_size;
        const auto allocation = context.Device()->GetResourceAllocationInfo(0, 1, &desc);
        try {
            feedback_pool.push_back({context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT,
                                                            D3D12_RESOURCE_STATE_COPY_DEST)});
        } catch (const std::exception& error) {
            if (!feedback_failure_logged) {
                feedback_failure_logged = true;
                Diagnostic(fmt::format("D3D12_FEEDBACK_ALLOCATION_FAILED width={} height={} "
                                       "layers={} mips={} bytes={} pool_bytes={} memory_bytes={} "
                                       "reason={}",
                                       desc.Width, desc.Height, desc.DepthOrArraySize,
                                       desc.MipLevels, allocation.SizeInBytes, pool_bytes,
                                       context.MemoryUsage(), error.what()));
            }
            return {};
        }
        selected = &feedback_pool.back();
    }
    selected->source = source.get();
    selected->draw = feedback_draw;
    selected->frame = feedback_frame;
    selected->copied.assign(subresource_count, false);
    selected->epoch = feedback_epoch;
    selected->source_version = source->content_version;
    // Without a range, preserve every mip, layer and depth/stencil plane. With one,
    // the SRV reads only those subresources, so copy just them.
    copy_into(*selected);
    context.MarkWork("attachment feedback copies");
    ++feedback_copies;
    context.Profile().Count(FrameCount::FeedbackCopies);
    return selected->resource;
}

// Original dynamic-state calculations, shared by cache misses and verification.
RasterizerD3D12::DynamicState RasterizerD3D12::ComputeDynamicState(
    const Maxwell& regs, const Pso::GraphicsPsoState& attachment_state,
    const Pso::FramebufferFormats& formats, u32 width, u32 height,
    const IGraphicsPipeline& pipeline, bool rescaled) {
    const auto& resolution = Settings::values.resolution_info;
    const ResolutionScale scale =
        rescaled ? ResolutionScale{resolution.up_scale, resolution.down_shift} : ResolutionScale{};
    DynamicState result;
    auto& viewports = result.viewports;
    auto& scissors = result.scissors;
    auto& y_flip = result.y_flip;
    for (size_t i = 0; i < Maxwell::NumViewports; ++i) {
        const auto viewport_result =
            regs.viewport_scale_offset_enabled ? MakeViewport(regs, i) : MakeFullViewport(regs);
        viewports[i] = viewport_result.viewport;
        viewports[i].TopLeftX = scale.Coordinate(viewports[i].TopLeftX);
        viewports[i].TopLeftY = scale.Coordinate(viewports[i].TopLeftY);
        viewports[i].Width = scale.Coordinate(viewports[i].Width);
        viewports[i].Height = scale.Coordinate(viewports[i].Height);
        if (viewports[i].MinDepth > viewports[i].MaxDepth) {
            static bool logged_inverted_depth = false;
            if (!logged_inverted_depth) {
                logged_inverted_depth = true;
                Diagnostic(fmt::format("D3D12 viewport {} MinDepth={} > MaxDepth={}", i,
                                       viewports[i].MinDepth, viewports[i].MaxDepth));
            }
        }
        static const bool viewport_z01 = BisectEnabled("NXBOX_D3D12_VIEWPORT_Z01");
        if (viewport_z01) {
            viewports[i].MinDepth = 0.0f;
            viewports[i].MaxDepth = 1.0f;
        }
        scissors[i] = MakeScissor(regs, i);
        const auto scaled_scissor =
            scale.Rect(scissors[i].left, scissors[i].top, scissors[i].right, scissors[i].bottom);
        scissors[i] = {scaled_scissor.left, scaled_scissor.top, scaled_scissor.right,
                       scaled_scissor.bottom};
        scissors[i].right = std::min(scissors[i].right, static_cast<LONG>(width));
        scissors[i].bottom = std::min(scissors[i].bottom, static_cast<LONG>(height));
        scissors[i].left = std::min(scissors[i].left, scissors[i].right);
        scissors[i].top = std::min(scissors[i].top, scissors[i].bottom);
        if (i == 0)
            y_flip = viewport_result.y_flip;
    }
    const bool back_only = regs.stencil_two_side_enable && regs.gl_cull_test_enabled &&
                           static_cast<u32>(regs.gl_cull_face) == 0x404;
    const auto cull =
        Pso::CullMode(regs.gl_cull_test_enabled != 0, static_cast<u32>(regs.gl_cull_face));
    if (attachment_state.stencil_enable && Pso::DsvHasStencil(formats.depth) && regs.zeta_enable &&
        regs.stencil_enable &&
        Pso::IndependentStencil(regs.stencil_two_side_enable != 0, cull.mode,
                                regs.stencil_front_ref, regs.stencil_back_ref,
                                regs.stencil_front_func_mask, regs.stencil_back_func_mask,
                                regs.stencil_front_mask, regs.stencil_back_mask)) {
        result.stencil = {1, regs.stencil_front_ref, regs.stencil_back_ref};
    } else {
        const UINT stencil_ref = back_only ? regs.stencil_back_ref : regs.stencil_front_ref;
        result.stencil = {0, stencil_ref, stencil_ref};
    }
    const bool replicate_alpha = Pso::ConstantBlendFactors(attachment_state) == 2;
    result.blend = {replicate_alpha ? regs.blend_color.a : regs.blend_color.r,
                    replicate_alpha ? regs.blend_color.a : regs.blend_color.g,
                    replicate_alpha ? regs.blend_color.a : regs.blend_color.b, regs.blend_color.a};
    if (attachment_state.depth_bounds) {
        result.depth_bounds = {std::clamp(regs.depth_bounds[0], 0.0f, 1.0f),
                               std::clamp(regs.depth_bounds[1], 0.0f, 1.0f)};
    }
    // PositionPass converts guest window coordinates to NDC using guest dimensions.
    result.words = pipeline.RootConstants(
        {rescaled ? static_cast<u32>(regs.surface_clip.width) : width,
         rescaled ? static_cast<u32>(regs.surface_clip.height) : height,
         regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft});
    return result;
}

// Binds everything a draw needs. Descriptor allocations (which can submit the open
// command list) happen before recording state.
template <typename Func>
void RasterizerD3D12::PrepareDraw(bool is_indexed, Func&& draw) {
    // A guest fence cuts the list between draws; so does this. Nothing of this draw has been
    // resolved or recorded yet, and the new list rebinds all state (InvalidateRecordedState).
    if (context.FlushDrawsIfDue())
        fence_manager.NotifyDeferredSubmit();
    bool trace_submitted = false;
    VideoCommon::SubmissionTrace::Log("host_draw_begin indexed={}", is_indexed);
    SCOPE_EXIT {
        VideoCommon::SubmissionTrace::Log("host_draw_end submitted={}", trace_submitted);
    };
    const FrameProfile::Scope profile_setup{context.Profile(), FramePhase::DrawSetup};
    // Re-emplacing ends the previous RAII scope before starting the next one. Keep
    // these boundaries outside descriptor/stage/attribute loops; nested runtime
    // scopes still subtract uploads, barriers and submissions exclusively.
    std::optional<FrameProfile::Scope> draw_phase;
    draw_phase.emplace(context.Profile(), FramePhase::DrawMemory);
    SCOPE_EXIT {
        const FrameProfile::Scope profile_cleanup{context.Profile(), FramePhase::DrawCleanup};
        for (auto& stage : bindings.stages)
            stage.Clear();
        state_scratch.clear();
        // Scratch keeps capacity, but only the allocator's remembered plans own
        // resources between draws (including failed/skipped draws).
        draw_view_plan.resources.clear();
        draw_sampler_plan.resources.clear();
    };
    {
        const FrameProfile::Scope profile_flush{context.Profile(), FramePhase::IndexFlushCaching};
        gpu_memory->FlushCaching();
    }
    if (!pipeline_cache) {
        if (!draw_logged) {
            LOG_WARNING(Render, "D3D12 P3: no pipeline cache registered, draws are skipped");
            draw_logged = true;
        }
        return;
    }
    std::unique_lock buffer_lock{buffer_cache.mutex, std::defer_lock};
    std::unique_lock texture_lock{texture_cache.mutex, std::defer_lock};
    std::lock(buffer_lock, texture_lock);
    const auto& regs = maxwell3d->regs;
    draw_phase.emplace(context.Profile(), FramePhase::GraphicsDescriptors);
    texture_cache.SynchronizeDescriptors(false);
    draw_phase.emplace(context.Profile(), FramePhase::RenderTargets);
    texture_cache.UpdateRenderTargets(false);
    draw_phase.emplace(context.Profile(), FramePhase::AttachmentSetup);
    auto* const cached_framebuffer = texture_cache.GetFramebuffer();
    if (!cached_framebuffer)
        return;
    const auto framebuffer_snapshot = cached_framebuffer->Snapshot();
    auto* const framebuffer = framebuffer_snapshot.get();
    static const bool cache_draw_metadata = !BisectEnabled("NXBOX_D3D12_NO_DRAW_FASTPATHS");
    const u32 guest_rts = std::min<u32>(regs.rt_control.count, VideoCommon::NUM_RT);
    ImageView* const depth_view = framebuffer->Depth();
    const bool has_depth = depth_view && depth_view->BorrowImageHandle();
    const bool reuse_draw_attachments = cache_draw_metadata &&
                                        draw_attachments.snapshot == framebuffer_snapshot &&
                                        draw_attachments.guest_rts == guest_rts;
    Pso::FramebufferFormats formats = draw_attachments.formats;
    std::array<ImageView*, VideoCommon::NUM_RT> color_views = draw_attachments.colors;
    if (!reuse_draw_attachments) {
        formats = {};
        color_views = {};
        u32 attachment_samples = 0;
        const auto accept_attachment = [&](const ImageView& view, bool depth) {
            const auto& resource = view.BorrowImageHandle();
            if (!resource)
                return false;
            const auto& desc = view.ResourceDescription();
            const auto required = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                        : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (!(desc.Flags & required)) {
                view.LogUnsupported(depth ? "depth attachment lacks ALLOW_DEPTH_STENCIL"
                                          : "color attachment lacks ALLOW_RENDER_TARGET");
                return false;
            }
            if (desc.SampleDesc.Quality != 0 ||
                (attachment_samples && attachment_samples != desc.SampleDesc.Count)) {
                view.LogUnsupported(fmt::format(
                    "attachment sample mismatch: bound={} count={} quality={}", attachment_samples,
                    desc.SampleDesc.Count, desc.SampleDesc.Quality));
                return false;
            }
            attachment_samples = desc.SampleDesc.Count;
            formats.sample_count = static_cast<u8>(attachment_samples);
            return true;
        };
        for (u32 slot = 0; slot < guest_rts; ++slot) {
            auto* view = framebuffer->Color(framebuffer->DrawBuffer(slot));
            if (view && !accept_attachment(*view, false)) {
                SkipDraw("unsupported attachment or mismatched sample counts");
                return;
            }
            if (view && view->BorrowImageHandle()) {
                // Only overlapping RTV subresources conflict. Distinct mips/layers
                // of one resource remain legal independent color attachments.
                bool duplicate = false;
                const auto& resource_desc = view->ResourceDescription();
                for (u32 previous = 0; previous < slot; ++previous) {
                    const auto* prior = color_views[previous];
                    if (!prior || prior->BorrowImageHandle() != view->BorrowImageHandle())
                        continue;
                    const bool same_mip = resource_desc.SampleDesc.Count > 1 ||
                                          prior->range.base.level == view->range.base.level;
                    duplicate |= same_mip &&
                                 (resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
                                  Pso::RenderTargetLayersOverlap(
                                      static_cast<u32>(prior->range.base.layer),
                                      static_cast<u32>(prior->range.extent.layers),
                                      static_cast<u32>(view->range.base.layer),
                                      static_cast<u32>(view->range.extent.layers)));
                }
                if (!duplicate) {
                    color_views[slot] = view;
                    formats.colors[slot] = static_cast<u32>(view->Description().Format);
                }
            }
        }
        if (depth_view && !accept_attachment(*depth_view, true)) {
            SkipDraw("unsupported depth attachment or mismatched sample counts");
            return;
        }
        if (has_depth)
            formats.depth = static_cast<u32>(DsvFormat(depth_view->Description().Format));
        draw_attachments.snapshot = framebuffer_snapshot;
        draw_attachments.guest_rts = guest_rts;
        draw_attachments.formats = formats;
        draw_attachments.colors = color_views;
        draw_attachments.area_valid = false;
    }
    Pso::GraphicsPsoState attachment_state{};
    if (reuse_draw_attachments)
        context.Profile().Count(FrameCount::AttachmentMetadataReuses);
    Pso::ApplyFramebufferFormats(attachment_state, formats);
    const u32 num_rts = attachment_state.num_render_targets;
    {
        const FrameProfile::Scope profile_query{context.Profile(), FramePhase::QueryState};
        query_runtime.PrepareDraw();
    }
    IGraphicsPipeline* const pipeline =
        pipeline_cache->CurrentGraphicsPipeline(formats, is_indexed, texture_cache.CaptureFrame());
    if (!pipeline) {
        VideoCommon::SubmissionTrace::Log("host_draw_drop reason=pending_or_failed_pso");
        return;
    }
    ID3D12PipelineState* const pso = pipeline->PipelineState();
    ID3D12RootSignature* const root_signature = pipeline->RootSignature();
    if (!pso || !root_signature)
        return;
    if constexpr (std::is_same_v<std::remove_cvref_t<Func>, DrawCommand>) {
        if (!PrepareDrawIndices(is_indexed, draw.params, *pipeline))
            return;
    }
    draw_phase.emplace(context.Profile(), FramePhase::GraphicsState);
    const GraphicsStateInputs current_attachment{
        context.GraphicsStateTracker().Update(*maxwell3d),
        {},
        formats,
        static_cast<u32>(maxwell3d->draw_manager.draw_state.topology),
        regs.zeta_enable,
        is_indexed,
        {regs.pipelines[2].enable != 0, regs.pipelines[3].enable != 0}};
    const bool reuse_attachment =
        StateFastpathEnabled() && attachment_inputs.Matches(current_attachment);
    if (!reuse_attachment || StateVerifyEnabled()) {
        FillGraphicsState(*maxwell3d, attachment_state);
        Pso::ApplyFramebufferFormats(attachment_state, formats);
        if (StateVerifyEnabled() && reuse_attachment &&
            std::memcmp(&cached_attachment_state, &attachment_state, sizeof(attachment_state)) != 0)
            StateVerifyMismatch("pipeline", &cached_attachment_state, &attachment_state,
                                sizeof(attachment_state));
        cached_attachment_state = attachment_state;
        attachment_inputs.Store(current_attachment);
    } else {
        attachment_state = cached_attachment_state;
    }
    if (attachment_diagnostics < 8) {
        ++attachment_diagnostics;
        std::string message = "D3D12_ATTACHMENTS count=" + std::to_string(guest_rts);
        const auto* fragment = pipeline->StageInfo(4);
        for (u32 slot = 0; slot < VideoCommon::NUM_RT; ++slot) {
            message += " slot" + std::to_string(slot) + "=" + std::to_string(formats.colors[slot]);
            if (fragment && fragment->stores_frag_color[slot] && !color_views[slot])
                message += "(unbound_shader_output)";
        }
        message += " depth=" + std::to_string(formats.depth);
        Diagnostic(message);
    }

    draw_phase.emplace(context.Profile(), FramePhase::AttachmentSetup);
    ++feedback_draw;
    const auto extent = [](const ImageView& view) {
        const auto& desc = view.ResourceDescription();
        const u32 mip = desc.SampleDesc.Count > 1 ? 0 : static_cast<u32>(view.range.base.level);
        return AttachmentExtent{static_cast<u32>(std::max<u64>(1, desc.Width >> mip)),
                                std::max(1u, desc.Height >> mip),
                                static_cast<u32>(view.range.extent.layers)};
    };
    AttachmentExtent render_area = draw_attachments.area;
    AttachmentExtent matching_extent = draw_attachments.matching_extent;
    if (!cache_draw_metadata || !draw_attachments.area_valid) {
        render_area = {16384, 16384, 2048};
        bool has_attachment = false;
        for (const auto* color : color_views) {
            if (color) {
                render_area = IntersectAttachments(render_area, extent(*color));
                has_attachment = true;
            }
        }
        if (has_depth) {
            render_area = IntersectAttachments(render_area, extent(*depth_view));
            has_attachment = true;
        }
        if (!has_attachment) {
            const auto size = framebuffer->Size();
            render_area = {size.width, size.height, 1};
        }
        if (!render_area.width || !render_area.height || !render_area.layers)
            return;
        D3D12_RESOURCE_DIMENSION attachment_dimension =
            has_depth ? depth_view->ResourceDescription().Dimension
                      : D3D12_RESOURCE_DIMENSION_UNKNOWN;
        for (const auto* color : color_views) {
            if (!color)
                continue;
            const auto dimension = color->ResourceDescription().Dimension;
            if (attachment_dimension != D3D12_RESOURCE_DIMENSION_UNKNOWN &&
                dimension != attachment_dimension) {
                SkipDraw("mismatched attachment resource types");
                return;
            }
            attachment_dimension = dimension;
        }
        matching_extent = MatchingAttachmentExtent(
            render_area, has_depth ? extent(*depth_view) : render_area, has_depth);
        draw_attachments.area = render_area;
        draw_attachments.matching_extent = matching_extent;
        draw_attachments.area_valid = true;
    }
    std::array<ResourcePtr, VideoCommon::NUM_RT> color_resources{};
    struct ColorCopy {
        BlitSurface original;
        BlitSurface temporary;
        VideoCommon::ImageCopy region;
    };
    std::array<ColorCopy, VideoCommon::NUM_RT> color_copy_storage{};
    std::span<ColorCopy> color_copies{color_copy_storage.data(), size_t{0}};
    for (u32 slot = 0; slot < num_rts; ++slot) {
        const auto* color = color_views[slot];
        if (!color)
            continue;
        color_resources[slot] = color->BorrowImageHandle();
        const FrameProfile::Scope profile_extent{context.Profile(), FramePhase::ExtentCopies};
        const auto original_extent = extent(*color);
        // Bind mismatched extents directly; hardware renders the intersection. The copy-in/out
        // path (NXBOX_D3D12_EXTENT_COPY=1) cost ~460 copies per frame in BotW for no visible gain.
        static const bool no_extent_copy = !BisectEnabled("NXBOX_D3D12_EXTENT_COPY");
        if (no_extent_copy || (original_extent.width == matching_extent.width &&
                               original_extent.height == matching_extent.height))
            continue;
        auto desc = color->ResourceDescription();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            SkipDraw("mismatched non-2D attachment extents");
            return;
        }
        desc.Width = matching_extent.width;
        desc.Height = matching_extent.height;
        desc.DepthOrArraySize = static_cast<UINT16>(matching_extent.layers);
        desc.MipLevels = 1;
        desc.Alignment = 0;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        ResourcePtr temporary;
        for (auto& entry : attachment_pool) {
            const auto& cached = entry.resource->Description();
            if (entry.draw != feedback_draw && cached.Width == desc.Width &&
                cached.Height == desc.Height && cached.DepthOrArraySize == desc.DepthOrArraySize &&
                cached.Format == desc.Format && cached.Flags == desc.Flags &&
                cached.SampleDesc.Count == desc.SampleDesc.Count &&
                cached.SampleDesc.Quality == desc.SampleDesc.Quality) {
                entry.draw = feedback_draw;
                entry.frame = feedback_frame;
                temporary = entry.resource;
                break;
            }
        }
        if (!temporary) {
            temporary = context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_STATE_COPY_DEST);
            temporary->Description();
            attachment_pool.push_back({temporary, nullptr, feedback_draw, feedback_frame});
        }
        color_resources[slot] = temporary;
        const VideoCommon::ImageCopy copy{
            .src_subresource = {color->range.base.level, color->range.base.layer,
                                static_cast<s32>(render_area.layers)},
            .dst_subresource = {0, 0, static_cast<s32>(render_area.layers)},
            .src_offset = {0, 0, 0},
            .dst_offset = {0, 0, 0},
            .extent = {render_area.width, render_area.height, 1},
        };
        const BlitSurface original{color->BorrowImageHandle(), color->HostFormat()};
        const BlitSurface target{temporary, color->HostFormat()};
        // CopyImage uses a bitwise region copy for single-sample color and a
        // sample-preserving shader copy for MSAA (never an averaging resolve).
        texture_runtime.BlitHelpers().CopyImage(target, original, std::span{&copy, 1});
        context.Profile().Count(FrameCount::ExtentCopies, render_area.layers);
        color_copy_storage[color_copies.size()] = {original, target, copy};
        color_copies = {color_copy_storage.data(), color_copies.size() + 1};
    }
    if (!color_copies.empty() && attachment_extent_diagnostics++ < 16) {
        Diagnostic(fmt::format("D3D12_ATTACHMENT_EXTENTS area={}x{} layers={} matched={}x{} "
                               "color_temporaries={}",
                               render_area.width, render_area.height, render_area.layers,
                               matching_extent.width, matching_extent.height, color_copies.size()));
    }

    draw_phase.emplace(context.Profile(), FramePhase::ResourceBindings);
    // ---- 1. Gather views/samplers and tell the buffer cache what the shaders use ----
    StageTableCounts totals{};
    bool has_images = false;
    for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
        uniform_masks[stage] = 0;
        uniform_sizes[stage].fill(0);
        if (const Shader::Info* info = pipeline->StageInfo(stage)) {
            uniform_masks[stage] = info->constant_buffer_mask;
            // The HLSL byte-addressed cbuffer can read dynamic offsets. Bind the
            // full guest range (the shared cache clamps this to the guest size).
            uniform_sizes[stage].fill(65536);
            const auto counts = CountStageTables(*info);
            totals.cbv += counts.cbv;
            totals.srv += counts.srv;
            totals.uav += counts.uav;
            totals.sampler += counts.sampler;
            has_images |= !info->image_descriptors.empty();
        }
    }
    buffer_cache.SetUniformBuffersState(uniform_masks, &uniform_sizes);

    view_scratch.clear();
    sampler_scratch.clear();
    const bool via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
        const Shader::Info* info = pipeline->StageInfo(stage);
        if (!info)
            continue;
        buffer_cache.UnbindGraphicsStorageBuffers(stage);
        size_t ssbo_index = 0;
        for (const auto& desc : info->storage_buffers_descriptors) {
            buffer_cache.BindGraphicsStorageBuffer(stage, ssbo_index++, desc.cbuf_index,
                                                   desc.cbuf_offset, desc.is_written);
        }
        const auto& cbufs = maxwell3d->state.shader_stages[stage].const_buffers;
        const auto read_handle = [&](const auto& desc, u32 index) {
            const u32 index_offset = index << desc.size_shift;
            const GPUVAddr addr = cbufs[desc.cbuf_index].address + desc.cbuf_offset + index_offset;
            if constexpr (requires { desc.has_secondary; }) {
                if (desc.has_secondary) {
                    const GPUVAddr separate = cbufs[desc.secondary_cbuf_index].address +
                                              desc.secondary_cbuf_offset + index_offset;
                    const u32 lhs = gpu_memory->Read<u32>(addr) << desc.shift_left;
                    const u32 rhs = gpu_memory->Read<u32>(separate) << desc.secondary_shift_left;
                    return Tegra::Texture::TexturePair(lhs | rhs, via_header_index);
                }
            }
            return Tegra::Texture::TexturePair(gpu_memory->Read<u32>(addr), via_header_index);
        };
        const auto add_images = [&](const auto& descs, bool blacklist) {
            for (const auto& desc : descs) {
                for (u32 i = 0; i < desc.count; ++i) {
                    view_scratch.push_back(
                        {.index = read_handle(desc, i).first, .blacklist = blacklist, .id = {}});
                }
            }
        };
        // Same order as the Vulkan backend: texel buffers, image buffers, textures, images.
        add_images(info->texture_buffer_descriptors, false);
        add_images(info->image_buffer_descriptors, false);
        for (const auto& desc : info->texture_descriptors) {
            for (u32 i = 0; i < desc.count; ++i) {
                const auto handle = read_handle(desc, i);
                view_scratch.push_back({.index = handle.first, .blacklist = false, .id = {}});
                sampler_scratch.push_back(texture_cache.GetSamplerId(handle.second, false));
            }
        }
        for (const auto& desc : info->image_descriptors) {
            for (u32 i = 0; i < desc.count; ++i)
                view_scratch.push_back(
                    {.index = read_handle(desc, i).first,
                     .blacklist = desc.is_written,
                     .id = {},
                     .format_override = PixelFormatFromImageFormat(desc.format)});
        }
    }
    draw_phase.emplace(context.Profile(), FramePhase::ImageViews);
    texture_cache.FillImageViews(std::span(view_scratch.data(), view_scratch.size()), false,
                                 has_images);
    if (has_images)
        texture_cache.BeginPassDumpImageWrite();
    // Mark the render targets modified again, after the sampled textures synchronized their
    // aliases. Vulkan prepares textures first; here the targets were marked earlier, so a
    // texture aliasing a target (SM3DW's bloom mip chain) copied the target's pre-draw
    // contents and took its modification tick, and never picked up what this draw writes.
    draw_phase.emplace(context.Profile(), FramePhase::RenderTargets);
    texture_cache.UpdateRenderTargets(false);

    draw_phase.emplace(context.Profile(), FramePhase::ResourceBindings);
    std::vector<VideoCommon::ImageViewId> written_images;
    // Texel/image buffers are bound through the buffer cache from the resolved views.
    {
        const VideoCommon::ImageViewInOut* it = view_scratch.data();
        for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
            const Shader::Info* info = pipeline->StageInfo(stage);
            if (!info)
                continue;
            buffer_cache.UnbindGraphicsTextureBuffers(stage);
            size_t index = 0;
            const auto add_buffers = [&](const auto& descs, bool is_image) {
                for (const auto& desc : descs) {
                    for (u32 i = 0; i < desc.count; ++i, ++it, ++index) {
                        ImageView& view = texture_cache.GetImageView(it->id);
                        auto format = view.format;
                        bool is_written = false;
                        if constexpr (requires { desc.is_written; }) {
                            is_written = desc.is_written;
                            if (const auto explicit_format =
                                    PixelFormatFromImageFormat(desc.format))
                                format = *explicit_format;
                        }
                        if constexpr (requires { desc.format; }) {
                            if (const auto problem =
                                    StorageImageProblem(context, format, desc.format,
                                                        desc.is_integer, desc.is_read,
                                                        desc.is_written))
                                return problem;
                        }
                        buffer_cache.BindGraphicsTextureBuffer(stage, index, view.GpuAddr(),
                                                               view.BufferSize(), format,
                                                               is_written, is_image);
                    }
                }
                return static_cast<const char*>(nullptr);
            };
            if (const auto problem = add_buffers(info->texture_buffer_descriptors, false)) {
                SkipDraw(problem);
                return;
            }
            if (const auto problem = add_buffers(info->image_buffer_descriptors, true)) {
                SkipDraw(problem);
                return;
            }
            it += Shader::NumDescriptors(info->texture_descriptors) +
                  Shader::NumDescriptors(info->image_descriptors);
        }
    }

    // Diagnostic: re-dirty small vertex streams so the cache re-reads guest memory every draw.
    static const bool vertex_resync = BisectEnabled("NXBOX_D3D12_VB_RESYNC");
    if (vertex_resync) {
        for (u32 index = 0; index < Maxwell::NumVertexArrays; ++index) {
            const auto& stream = regs.vertex_streams[index];
            if (!stream.enable)
                continue;
            const GPUVAddr begin = stream.Address();
            const GPUVAddr end = regs.vertex_stream_limits[index].Address() + 1;
            if (end <= begin || end - begin > 4096)
                continue;
            if (const auto device_address = gpu_memory->GpuToCpuAddress(begin))
                buffer_cache.WriteMemory(*device_address, end - begin);
        }
    }
    draw_phase.emplace(context.Profile(), FramePhase::GraphicsBuffers);
    buffer_cache.UpdateGraphicsBuffers(is_indexed);
    draw_phase.emplace(context.Profile(), FramePhase::GeometryBuffers);
    buffer_cache.BindHostGeometryBuffers(is_indexed, !converted_indices.has_value());
    draw_phase.emplace(context.Profile(), FramePhase::StageBuffers);
    for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
        bindings.stages[stage].Clear();
        if (!pipeline->StageInfo(stage))
            continue;
        bindings.current_stage = stage;
        buffer_cache.BindHostStageBuffers(stage);
    }
    if (buffer_cache.any_buffer_uploaded) {
        buffer_runtime.PostCopyBarrier();
        buffer_cache.any_buffer_uploaded = false;
    }

    draw_phase.emplace(context.Profile(), FramePhase::BufferValidation);
    // Guest mappings can shrink between cache bindings. Do not hand invalid
    // buffer ranges to the descriptor API or poison the whole GPU thread.
    for (const auto& stage : bindings.stages) {
        const auto valid = [this](const auto& buffer, u64 alignment) {
            return !buffer.resource || !buffer.size ||
                   (buffer.offset % alignment == 0 &&
                    IsValidBufferView(buffer.resource->handle->GetGPUVirtualAddress(),
                                      (buffer_runtime.BufferFastpathEnabled()
                                           ? buffer.resource->Description().Width
                                           : buffer.resource->handle->GetDesc().Width),
                                      buffer.offset, buffer.size));
        };
        for (const auto& buffer : stage.uniform) {
            if (!valid(buffer, 256)) {
                SkipDraw("invalid constant buffer view");
                return;
            }
        }
        for (const auto& buffer : stage.storage) {
            if (!valid(buffer, 4)) {
                SkipDraw("invalid storage buffer view");
                return;
            }
        }
        for (const auto& buffer : stage.texel) {
            const auto format =
                MaxwellFormat(static_cast<VideoCore::Surface::PixelFormat>(buffer.pixel_format));
            if (format.bytes_per_block && !valid(buffer, format.bytes_per_block)) {
                SkipDraw("invalid texel buffer view");
                return;
            }
        }
    }

    static const bool subresource_feedback = BisectEnabled("NXBOX_D3D12_SUBRESOURCE_FEEDBACK");
    const auto color_disjoint = [&](const ImageView& view) {
        const auto sampled = view.ImageHandle();
        const auto& range = view.range;
        const auto overlaps = [&](const auto& other) {
            return range.base.level < other.base.level + other.extent.levels &&
                   other.base.level < range.base.level + range.extent.levels &&
                   range.base.layer < other.base.layer + other.extent.layers &&
                   other.base.layer < range.base.layer + range.extent.layers;
        };
        for (const auto* color : color_views) {
            if (color && color->ImageHandle() == sampled && overlaps(color->range))
                return false;
        }
        return true;
    };
    const auto can_split_feedback = [&](const ImageView& view) {
        if (!subresource_feedback || !color_disjoint(view))
            return false;
        const auto sampled = view.ImageHandle();
        const auto desc = sampled->handle->GetDesc();
        if ((desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                           D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) ||
            (has_depth && depth_view->ImageHandle() == sampled))
            return false;
        // Depth slices of a 3D mip share one subresource, even if view layers differ.
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            for (const auto* color : color_views) {
                if (color && color->ImageHandle() == sampled &&
                    view.range.base.level < color->range.base.level + color->range.extent.levels &&
                    color->range.base.level < view.range.base.level + view.range.extent.levels)
                    return false;
            }
        }
        return true;
    };
    {
        // A different attachment set is a coherence point for feedback copies.
        std::array<const Resource*, VideoCommon::NUM_RT + 1> targets{};
        for (u32 slot = 0; slot < VideoCommon::NUM_RT; ++slot)
            targets[slot] = color_views[slot] ? color_views[slot]->ImageHandle().get() : nullptr;
        targets[VideoCommon::NUM_RT] = has_depth ? depth_view->ImageHandle().get() : nullptr;
        if (targets != feedback_targets) {
            feedback_targets = targets;
            ++feedback_epoch;
        }
    }
    draw_phase.emplace(context.Profile(), FramePhase::FeedbackSetup);
    // Resource creation can drain the queue under memory pressure. Do it before
    // reserving descriptor slots, whose lifetime is tied to the recording tick.
    for (const auto& item : view_scratch) {
        const auto sampled = texture_cache.GetImageView(item.id).ImageHandle();
        if (!sampled)
            continue;
        bool feedback = false;
        for (const auto& color : color_resources)
            feedback |= color == sampled;
        if (feedback && context.Profile().Enabled() &&
            color_disjoint(texture_cache.GetImageView(item.id)))
            context.Profile().Count(FrameCount::FeedbackDisjoint);
        if (feedback && can_split_feedback(texture_cache.GetImageView(item.id)))
            feedback = false;
        const bool depth_feedback = has_depth && depth_view->ImageHandle() == sampled;
        const bool stencil_writes =
            regs.stencil_enable &&
            ((regs.stencil_front_mask & 0xff) ||
             (regs.stencil_two_side_enable && (regs.stencil_back_mask & 0xff)));
        feedback |= depth_feedback && SelectFeedbackMode(true, regs.depth_write_enabled != 0,
                                                         stencil_writes) == FeedbackMode::Copy;
        if (feedback && !CopyFeedback(sampled, &texture_cache.GetImageView(item.id).range)) {
            SkipDraw("feedback allocation failed");
            return;
        }
    }

    draw_phase.emplace(context.Profile(), FramePhase::GeometrySetup);
    bool invalid_states = false;
    bool readonly_depth = false;
    state_scratch.clear();
    static const bool resource_state_index = [] {
        const char* value = std::getenv("NXBOX_D3D12_RESOURCE_STATE_INDEX");
        return !value || std::strcmp(value, "0") != 0;
    }();
    if (resource_state_index)
        draw_resource_index.Reset();
    std::vector<std::pair<ResourcePtr, std::vector<u32>>> sampled_subresources;
    const auto need = [this, &invalid_states](const ResourcePtr& resource,
                                              D3D12_RESOURCE_STATES state) {
        if (!resource)
            return;
        const auto index = resource_state_index
                               ? draw_resource_index.FindOrInsert(resource.get(), state_scratch)
                               : DrawResourceIndex<>::FindLinear(resource.get(), state_scratch);
        if (index != state_scratch.size()) {
            auto& known_state = state_scratch[index].second;
            constexpr u32 WriteStates =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET |
                D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT;
            if (!CompatibleDrawStates(known_state, state, WriteStates)) {
                if (MergesIntoBufferUav(known_state, state) &&
                    resource->handle->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                    known_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    return;
                }
                // Name the first conflicts so the skipped draw can be traced to its bindings.
                static std::atomic<u32> logged{};
                if (!invalid_states && logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                    const auto desc = resource->handle->GetDesc();
                    Diagnostic(fmt::format(
                        "D3D12_STATE_CONFLICT have={:#x} want={:#x} dim={} size={}x{}x{} mips={} "
                        "format={} flags={:#x}",
                        static_cast<u32>(known_state), static_cast<u32>(state),
                        static_cast<int>(desc.Dimension), desc.Width, desc.Height,
                        desc.DepthOrArraySize, desc.MipLevels, static_cast<int>(desc.Format),
                        static_cast<u32>(desc.Flags)));
                }
                invalid_states = true;
            }
            known_state |= state;
            return;
        }
        state_scratch.emplace_back(resource, state);
    };
    // Geometry buffers.
    auto& geometry = bindings.geometry;
    std::array<D3D12_VERTEX_BUFFER_VIEW, GeometryBindings::MaxVertexBuffers> vertex_views{};
    if (!zero_vertex_buffer) {
        context.Profile().Count(FrameCount::BufferUploadBytes, 65536);
        context.Profile().Count(FrameCount::VertexUploadBytes, 65536);
        zero_vertex_buffer = context.CreateBuffer(65536, D3D12_HEAP_TYPE_UPLOAD);
        std::fill(zero_vertex_buffer->mapped.begin(), zero_vertex_buffer->mapped.end(), u8{0});
    }
    context.Retain(zero_vertex_buffer);
    const GraphicsPipeline* const native_pipeline = pipeline->AsNative();
    if (!native_pipeline) {
        SkipDraw("vertex layout unavailable for native pipeline");
        return;
    }
    const auto& submitted = draw.params;
    if (!submitted.num_vertices || !submitted.num_instances)
        return;
    static const u64 draw_trace_frame = [] {
        u64 selected_frame = 0;
        if (const char* value = std::getenv("NXBOX_D3D12_DRAW_TRACE_FRAME")) {
            const std::string_view text{value};
            const auto parsed =
                std::from_chars(text.data(), text.data() + text.size(), selected_frame);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return u64{0};
        }
        return selected_frame;
    }();
    static const bool draw_trace_enabled = draw_trace_frame != 0;
    const bool trace_vertex_bytes = draw_trace_enabled &&
                                    texture_cache.CaptureFrame() == draw_trace_frame &&
                                    draw_trace_lines < 3000 && vb_trace_draws < 64 &&
                                    !submitted.is_indexed && submitted.num_vertices <= 64;
    // Diagnostic (NXBOX_D3D12_GLYPH_TRACE=<first frame>): for the text-quad vertex shader, log
    // the instance records the guest wrote next to what the host storage buffer holds, and the
    // texture bound to each array-sampled slot.
    static const u64 glyph_trace_frame = [] {
        const char* value = std::getenv("NXBOX_D3D12_GLYPH_TRACE");
        return value ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    constexpr u64 GlyphTraceHash = 0xeec542b30fd5cb27ULL;
    constexpr u32 GlyphTraceBytes = 512;
    const bool glyph_trace = glyph_trace_frame != 0 &&
                             texture_cache.CaptureFrame() >= glyph_trace_frame &&
                             texture_cache.CaptureFrame() < glyph_trace_frame + 3 &&
                             glyph_trace_draws < 60 &&
                             native_pipeline->VertexGuestHash() == GlyphTraceHash;
    ResourcePtr glyph_readback;
    if (glyph_trace)
        glyph_readback = context.CreateBuffer(GlyphTraceBytes * 2, D3D12_HEAP_TYPE_READBACK);
    // Keep the actual per-attribute IA resources, including temporary copies.
    std::array<ResourcePtr, Pso::kNumVertexAttributes> trace_vertex_resources{};
    constexpr size_t VertexTraceBytes = 48;
    constexpr u32 MaxTraceStreams = 2;
    ResourcePtr vertex_trace_readback;
    if (trace_vertex_bytes) {
        // CreateBuffer may submit under pressure. Allocate before descriptor reservation
        // and state recording, never between the copy and its draw.
        vertex_trace_readback =
            context.CreateBuffer(VertexTraceBytes * MaxTraceStreams, D3D12_HEAP_TYPE_READBACK);
    }
    const auto& guest_draw = maxwell3d->draw_manager.draw_state;
    // Index expansion increases submitted index count, not the maximum guest
    // vertex. Use the original vertex range for generated fans/strips/quads.
    u64 last_vertex =
        Pso::VertexLastNonIndexed(submitted.base_vertex, guest_draw.vertex_buffer.count);
    // Indexed draws can reference any vertex in the bound stream. Reading and scanning the
    // guest index buffer on the CPU per draw stalled the GPU thread for seconds (it forces
    // GPU memory reads), so bind the whole stream range the guest declared instead, like the
    // Vulkan backend does.
    // Diagnostic: also bind the whole stream for generated quad indices.
    static const bool quad_whole_stream = BisectEnabled("NXBOX_D3D12_QUAD_WHOLE_STREAM");
    const bool quad_draw = guest_draw.topology == Maxwell::PrimitiveTopology::Quads;
    const bool whole_stream = is_indexed || (quad_whole_stream && quad_draw);
    static const bool vertex_binding_prefix =
        !BisectEnabled("NXBOX_D3D12_NO_VERTEX_BINDING_PREFIX");
    // Slots above the input layout are unread. A later, wider layout rebuilds
    // its entire prefix; RecordedArray also compares the count when rebinding.
    // Keep the complete array for diagnostics that inspect unused stream zero.
    const u32 num_vertex_views = vertex_binding_prefix && !draw_trace_enabled
                                     ? Pso::VertexBufferBindingCount(native_pipeline->State())
                                     : static_cast<u32>(vertex_views.size());
    const auto zero_vertex_address = zero_vertex_buffer->handle->GetGPUVirtualAddress();
    for (u32 i = 0; i < num_vertex_views; ++i) {
        const auto& attribute = native_pipeline->State().attributes[i];
        vertex_views[i] = {zero_vertex_address, 65536, 0};
        if (trace_vertex_bytes)
            trace_vertex_resources[i] = zero_vertex_buffer;
        if (!attribute.enabled || attribute.enabled == 2)
            continue;
        const auto& slot = geometry.vertex[attribute.slot];
        if (!regs.vertex_streams[attribute.slot].enable || !slot.resource || !slot.size)
            continue;
        const auto& input = regs.vertex_attrib_format[i];
        const u32 vertex_size = static_cast<u32>(input.size.Value());
        const u32 bytes = Pso::VertexByteSize(vertex_size);
        const u32 storage_bytes = Pso::VertexStorageByteSize(vertex_size, attribute.swap_rb != 0);
        const u32 alignment = Pso::VertexAlignment(static_cast<u32>(input.size.Value()));
        u64 last = attribute.instanced
                             ? (attribute.divisor == 0 ? 0
                                                       : u64{submitted.base_instance} +
                                                             (submitted.num_instances - 1) /
                                                                 std::max(attribute.divisor, 1u))
                             : last_vertex;
        const u64 stream_available =
            slot.size > attribute.offset ? slot.size - attribute.offset : 0;
        if (whole_stream && !attribute.instanced)
            last = slot.stride && stream_available >= bytes
                       ? (stream_available - bytes) / slot.stride
                       : 0;
        const u64 required = Pso::VertexRequiredBytes(last, slot.stride, bytes);
        constexpr u64 MaxVertexUpload = 64 * 1024 * 1024;
        const u64 available = slot.size > attribute.offset ? slot.size - attribute.offset : 0;
        const u64 source_offset = u64{slot.offset} + attribute.offset;
        const bool realign = (source_offset % alignment) || (slot.stride % alignment) ||
                             bytes != storage_bytes || attribute.swap_rb;
        if (!realign && required <= available) {
            vertex_views[i] = {slot.resource->handle->GetGPUVirtualAddress() + source_offset,
                               static_cast<UINT>(available), slot.stride};
            if (trace_vertex_bytes)
                trace_vertex_resources[i] = slot.resource;
            need(slot.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            continue;
        }
        if (required > MaxVertexUpload) {
            SkipDraw("vertex binding exceeds 64 MiB draw budget");
            return;
        }
        // Only instrument the slow copy/repack path, never each native attribute binding.
        const FrameProfile::Scope profile_vertex_copies{context.Profile(),
                                                        FramePhase::VertexCopies};
        if (!realign) {
            // Preserve hardware zero reads beyond the inclusive Maxwell limit.
            context.Profile().Count(FrameCount::BufferUploadBytes, required);
            context.Profile().Count(FrameCount::VertexUploadBytes, required);
            context.Profile().Count(FrameCount::VertexPaddingBytes, required);
            context.Profile().Count(FrameCount::StagingCopyBytes, required);
            context.Profile().Count(FrameCount::GpuBufferCopyBytes, std::min(available, required));
            // The data and zero-tail copies must not overlap: D3D12 does not order two copies
            // into the same range without a barrier, and AMD let the zero fill land last.
            const u64 copied = std::min(available, required);
            if (trace_vertex_bytes) {
                // Diagnostic: the cached source bytes and extent behind a padded binding.
                const u64 width = slot.resource->handle->GetDesc().Width;
                const u64 probe = source_offset < width
                                      ? std::min<u64>(VertexTraceBytes, width - source_offset)
                                      : 0;
                const GPUVAddr guest_address =
                    regs.vertex_streams[attribute.slot].Address() + attribute.offset;
                const auto device_address = gpu_memory->GpuToCpuAddress(guest_address);
                const int gpu_modified =
                    device_address ? buffer_cache.IsRegionGpuModified(*device_address, copied) : -1;
                const int cpu_modified =
                    device_address ? buffer_cache.IsRegionCpuModified(*device_address, copied) : -1;
                // Snapshot at draw time, before the deferred readback callback. Translate each
                // endpoint independently: adjacent GPU pages need not be adjacent device pages.
                const auto trace_page = [&](std::optional<DAddr> address) -> std::string {
                    if (!address) {
                        return "unavailable";
                    }
                    const auto info = device_memory.GetPageCacheInfo(*address);
                    if (!info) {
                        return "unavailable";
                    }
                    const auto entry = info->memory
                                           ? info->memory->GetPageEntryInfo(info->cpu_vaddr)
                                           : std::nullopt;
                    return fmt::format(
                        "daddr={:#x},asid={},cpu_vaddr={:#x},cached_count={},entry_valid={},"
                        "raw={:#x},storage_raw={:#x},type={},marked={},pointer_null={},is_current={},"
                        "aliases={}",
                        *address & ~Core::DEVICE_PAGEMASK, info->asid.id, info->cpu_vaddr,
                        info->cached_count, entry.has_value(), entry ? entry->raw : 0,
                        entry ? entry->storage_raw : 0,
                        entry ? static_cast<int>(entry->type) : -1,
                        entry ? static_cast<int>(entry->marked) : -1,
                        entry ? static_cast<int>(entry->pointer_null) : -1,
                        entry ? static_cast<int>(entry->is_current) : -1,
                        info->memory ? info->memory->DescribePhysicalAliases(
                                           device_memory.GetPointer<u8>(*address &
                                                                        ~Core::DEVICE_PAGEMASK))
                                     : std::string{"unavailable"});
                };
                const auto first_page = trace_page(copied ? device_address : std::nullopt);
                const auto last_page =
                    trace_page(copied ? gpu_memory->GpuToCpuAddress(guest_address + copied - 1)
                                      : std::nullopt);
                auto source_readback =
                    context.CreateBuffer(VertexTraceBytes, D3D12_HEAP_TYPE_READBACK);
                if (probe) {
                    context.Transition(source_readback, D3D12_RESOURCE_STATE_COPY_DEST);
                    context.Transition(slot.resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    context.BeforeCopyWrite(source_readback, 0, probe);
                    context.Commands()->CopyBufferRegion(source_readback->handle.Get(), 0,
                                                         slot.resource->handle.Get(),
                                                         source_offset, probe);
                }
                const u64 resource_va = slot.resource->handle->GetGPUVirtualAddress();
                const int resource_heap = static_cast<int>(slot.resource->heap);
                const bool is_zero_buffer = slot.resource == zero_vertex_buffer;
                context.AfterCompletion([source_readback, probe, i, size = slot.size,
                                         resource_va, resource_heap, is_zero_buffer,
                                         offset = slot.offset, attr_offset = attribute.offset,
                                         width, required, available, copied, gpu_modified,
                                         cpu_modified, first_page, last_page] {
                    std::string hex;
                    for (const u8 byte : source_readback->mapped.first(static_cast<size_t>(probe)))
                        hex += fmt::format("{:02x}", byte);
                    Diagnostic(fmt::format(
                        "D3D12_VB_SOURCE resource_va={:#x} heap={} zero_buffer={} "
                        "attr={} slot_size={} slot_offset={} attr_offset={} "
                        "width={} required={} available={} copied={} gpu_modified={} cpu_modified={} "
                        "cache={} first_page=[{}] last_page=[{}]",
                        resource_va, resource_heap, is_zero_buffer, i, size, offset, attr_offset,
                        width, required, available, copied, gpu_modified, cpu_modified, hex,
                        first_page, last_page));
                });
            }
            // Creating two buffers per padded binding cost SM3DW ~0.6 ms a frame for ~540 bytes:
            // reuse the padded buffer per source range (barriers order the re-copy) and take the
            // zero tail from the shared zero buffer when it fits.
            // ponytail: unbounded until 256 entries, then cleared; LRU if games churn ranges.
            const bool shared_zero = required - copied <= 65536;
            ResourcePtr zero = zero_vertex_buffer;
            if (!shared_zero) {
                zero = context.CreateBuffer(required, D3D12_HEAP_TYPE_UPLOAD);
                std::fill(zero->mapped.begin(), zero->mapped.end(), u8{0});
            }
            if (padded_vertex_buffers.size() > 256)
                padded_vertex_buffers.clear();
            auto& cached = padded_vertex_buffers[{slot.resource.get(), source_offset, required}];
            if (cached.source != slot.resource || !cached.padded) {
                cached.source = slot.resource;
                cached.padded = context.CreateBuffer(required, D3D12_HEAP_TYPE_DEFAULT);
            }
            auto padded = cached.padded;
            context.MarkWork("vertex binding padding");
            context.Transition(padded, D3D12_RESOURCE_STATE_COPY_DEST);
            // Diagnostic: source the bytes from guest memory instead of the buffer cache.
            static const bool guest_upload = BisectEnabled("NXBOX_D3D12_VB_GUEST_UPLOAD");
            if (guest_upload && copied) {
                gpu_memory->ReadBlockUnsafe(regs.vertex_streams[attribute.slot].Address() +
                                                attribute.offset,
                                            zero->mapped.data(), copied);
                context.BeforeCopyWrite(padded, 0, copied);
                context.Commands()->CopyBufferRegion(padded->handle.Get(), 0, zero->handle.Get(),
                                                     0, copied);
            } else if (copied) {
                context.Transition(slot.resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
                context.BeforeCopyWrite(padded, 0, copied);
                context.Commands()->CopyBufferRegion(padded->handle.Get(), 0,
                                                     slot.resource->handle.Get(), source_offset,
                                                     copied);
            }
            if (required > copied) {
                context.BeforeCopyWrite(padded, copied, required - copied);
                context.Commands()->CopyBufferRegion(padded->handle.Get(), copied,
                                                     zero->handle.Get(), shared_zero ? 0 : copied,
                                                     required - copied);
            }
            context.Retain(zero);
            context.Retain(padded);
            vertex_views[i] = {padded->handle->GetGPUVirtualAddress(), static_cast<UINT>(required),
                               slot.stride};
            if (trace_vertex_bytes)
                trace_vertex_resources[i] = padded;
            need(padded, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            continue;
        }
        // Repack only misaligned streams. Each attribute owns one IA slot with
        // offset zero, so shared streams cannot impose conflicting alignments.
        const u64 elements = slot.stride ? last + 1 : 1;
        if (elements * storage_bytes > MaxVertexUpload) {
            SkipDraw("realigned vertex binding exceeds 64 MiB draw budget");
            return;
        }
        context.Profile().Count(FrameCount::BufferUploadBytes, elements * storage_bytes);
        context.Profile().Count(FrameCount::VertexUploadBytes, elements * storage_bytes);
        context.Profile().Count(FrameCount::VertexRepackBytes, elements * storage_bytes);
        // A ring slice rather than a placed resource (and aliasing barrier) per draw: creation
        // cost Odyssey ~1.9 ms a frame for ~25 repacked streams. Tracing keeps a whole buffer.
        const auto packed_bytes = static_cast<size_t>(elements * storage_bytes);
        const StagingBufferRef packed =
            trace_vertex_bytes ? [&] {
                auto buffer = context.CreateBuffer(packed_bytes, D3D12_HEAP_TYPE_UPLOAD);
                return StagingBufferRef{buffer, 0, buffer->mapped.first(packed_bytes)};
            }()
                               : vertex_repack.Request(packed_bytes);
        std::fill(packed.mapped_span.begin(), packed.mapped_span.end(), u8{0});
        const u64 read_bytes = std::min(available, required);
        std::vector<u8> source(static_cast<size_t>(read_bytes));
        const auto guest_address = regs.vertex_streams[attribute.slot].Address() + attribute.offset;
        if (const auto cpu = gpu_memory->GpuToCpuAddress(guest_address))
            buffer_cache.DownloadMemory(*cpu, read_bytes);
        gpu_memory->ReadBlock(guest_address, source.data(), source.size());
        Pso::RepackVertexFormat(packed.mapped_span, source, slot.stride, vertex_size,
                                static_cast<u32>(input.type.Value()), attribute.swap_rb != 0);
        context.Retain(packed.buffer);
        if (trace_vertex_bytes)
            trace_vertex_resources[i] = packed.buffer;
        vertex_views[i] = {packed.buffer->handle->GetGPUVirtualAddress() + packed.offset,
                           static_cast<UINT>(elements * storage_bytes),
                           slot.stride ? storage_bytes : 0};
    }
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const bool quads = draw_state.topology == Maxwell::PrimitiveTopology::Quads ||
                       draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip;
    const bool needs_index = is_indexed || quads || converted_indices.has_value();
    D3D12_INDEX_BUFFER_VIEW index_view{};
    if (converted_indices) {
        index_view = converted_indices->view;
        if (converted_indices->resource)
            need(converted_indices->resource, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    } else if (needs_index) {
        if (!geometry.index || !geometry.index_size || geometry.index_unsupported) {
            if (geometry.index_unsupported && !quad_logged) {
                LOG_WARNING(Render, "D3D12 P3: 8-bit/indexed-quad index conversion not "
                                    "implemented, draw skipped");
                quad_logged = true;
            }
            return;
        }
        if (!IsValidBufferView(geometry.index->handle->GetGPUVirtualAddress(),
                               geometry.index->handle->GetDesc().Width, geometry.index_offset,
                               geometry.index_size)) {
            SkipDraw("invalid index buffer view");
            return;
        }
        index_view = {geometry.index->handle->GetGPUVirtualAddress() + geometry.index_offset,
                      static_cast<UINT>(geometry.index_size),
                      geometry.index_is_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
        need(geometry.index, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
    const auto* feedback_info = native_pipeline->TransformFeedbackInfo();
    if (feedback_info) {
        transform_feedback.Configure(*feedback_info);
        for (u32 slot = 0; slot < regs.transform_feedback.buffers.size(); ++slot) {
            const auto& guest = regs.transform_feedback.buffers[slot];
            if (!guest.enable || !regs.transform_feedback.controls[slot].varying_count) {
                transform_feedback.BindBuffer(slot, nullptr, 0, 0);
                continue;
            }
            if (guest.size <= 0 || guest.start_offset < 0) {
                SkipDraw("invalid guest stream-output range");
                return;
            }
            const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                guest.Address() + static_cast<u32>(guest.start_offset),
                static_cast<u32>(guest.size), VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                VideoCommon::ObtainBufferOperation::MarkAsWritten);
            auto resource = buffer->Handle();
            if (!resource) {
                SkipDraw("unmapped guest stream-output buffer");
                return;
            }
            transform_feedback.BindBuffer(slot, resource, offset, static_cast<u32>(guest.size));
            need(resource, D3D12_RESOURCE_STATE_STREAM_OUT);
        }
    }
    draw_phase.emplace(context.Profile(), FramePhase::DescriptorSetup);
    // The buffer cache is done for this draw; guest writes contend on it the most.
    if (!draw_trace_enabled)
        buffer_lock.unlock();
    // ---- 2. Plan every descriptor this draw needs ----
    const bool root_cbvs = pipeline->UsesRootCbvs();
    if (root_cbvs)
        PrepareZeroUniformBuffer();
    auto& root_cbv_addresses = draw_root_cbv_addresses;
    root_cbv_addresses.clear();
    const u32 cbv_size = root_cbvs ? 0u : TableSize(totals.cbv);
    const u32 srv_size = TableSize(totals.srv);
    const u32 uav_size = TableSize(totals.uav);
    const u32 sampler_size = TableSize(totals.sampler);
    auto& view_plan = draw_view_plan;
    auto& sampler_plan = draw_sampler_plan;
    view_plan.Reset({cbv_size, srv_size, uav_size});
    sampler_plan.Reset({sampler_size, 0, 0});
    // Cache only immutable sampler keys, never heap handles, view IDs or resources.
    // GetSamplerId above still reads the guest TSC every draw. Full entry bytes and
    // comparison mode therefore cover CPU/DMA/GPU writes and channel/table changes.
    // Image resolution cannot use this proof: it also needs lifetime/alias generations.
    struct ResolvedSamplerPlan {
        struct Input {
            std::array<u8, sizeof(Tegra::Texture::TSCEntry)> entry{};
            bool compares{};
        };
        std::vector<Input> inputs;
        std::vector<DescriptorKey> keys;
        u64 draws{}, hits{};
    };
    static thread_local ResolvedSamplerPlan resolved_sampler_plan;
    static const bool resolved_samplers_enabled = [] {
        const char* value = std::getenv("NXBOX_D3D12_SAMPLER_PLAN_REUSE");
        return !value || std::strcmp(value, "0") != 0;
    }();
    // Keep this memo bounded, including for providers with unusual layouts.
    const bool cache_sampler_plan =
        resolved_samplers_enabled && totals.sampler != 0 && sampler_size <= 4096;
    bool reuse_sampler_plan = false;
    if (cache_sampler_plan) {
        auto& cached = resolved_sampler_plan;
        reuse_sampler_plan =
            cached.inputs.size() == totals.sampler && cached.keys.size() == sampler_size;
        cached.inputs.resize(totals.sampler);
        u32 slot = 0;
        for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
            const Shader::Info* info = pipeline->StageInfo(stage);
            if (!info)
                continue;
            for (const auto& desc : info->texture_descriptors) {
                for (u32 i = 0; i < desc.count; ++i, ++slot) {
                    const auto& entry = texture_cache.GetSampler(sampler_scratch[slot]).Entry();
                    auto& input = cached.inputs[slot];
                    reuse_sampler_plan &=
                        input.compares == desc.is_depth &&
                        std::memcmp(input.entry.data(), &entry, sizeof(entry)) == 0;
                    // Min/max translation also depends on a mutable device policy in
                    // MakeSamplerDesc; rebuild those plans through the normal writers.
                    reuse_sampler_plan &=
                        entry.reduction_filter != Tegra::Texture::SamplerReduction::Min &&
                        entry.reduction_filter != Tegra::Texture::SamplerReduction::Max;
                    std::memcpy(input.entry.data(), &entry, sizeof(entry));
                    input.compares = desc.is_depth;
                }
            }
        }
        if (reuse_sampler_plan) {
            sampler_plan.keys = cached.keys;
            std::fill(sampler_plan.initialized.begin(), sampler_plan.initialized.end(), u8{1});
            sampler_plan.sources.valid = false;
            // FinishDrawTable will reuse the current
            // recording's table or resolve keys against the current heap generation.
            ++cached.hits;
        } else {
            // A failed/skipped draw must not pair new inputs with old keys.
            cached.keys.clear();
        }
        if (++cached.draws % 65536 == 0) {
            Diagnostic(fmt::format("D3D12_RESOLVED_SAMPLER_PLAN draws={} hits={} misses={}",
                                   cached.draws, cached.hits, cached.draws - cached.hits));
        }
    }
    const bool validate_sampler_plan =
        reuse_sampler_plan && BisectEnabled("NXBOX_D3D12_VALIDATE_COMMANDS");
    DescriptorTable view_block{{}, {}, cbv_size + srv_size + uav_size, 0, &view_plan};
    auto cbv_table = view_block.Slice(0, cbv_size);
    auto srv_table = view_block.Slice(cbv_size, srv_size);
    auto uav_table = view_block.Slice(cbv_size + srv_size, uav_size);
    DescriptorTable sampler_table{{}, {}, sampler_size, 0, &sampler_plan};
    const auto rtv_base = descriptors.BoundRtvs();
    const auto dsv_handle = has_depth ? descriptors.BoundDsv() : D3D12_CPU_DESCRIPTOR_HANDLE{};

    // ---- 3. Build full descriptor keys and collect resource states for every draw ----
    u32 cbv_i = 0, srv_i = 0, uav_i = 0, smp_i = 0;
    std::array<u32, NUM_GRAPHICS_STAGES> texture_scaling_masks{};
    u32 image_scaling_mask = 0;
    const VideoCommon::ImageViewInOut* view_it = view_scratch.data();
    const VideoCommon::SamplerId* sampler_it = sampler_scratch.data();
    for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
        const Shader::Info* info = pipeline->StageInfo(stage);
        if (!info)
            continue;
        const auto& bound = bindings.stages[stage];
        size_t uniform_it = 0, storage_it = 0, texel_it = 0;
        // Constant buffers, in the order of constant_buffer_descriptors.
        for (const auto& desc : info->constant_buffer_descriptors) {
            for (u32 i = 0; i < desc.count; ++i, ++cbv_i) {
                BoundBuffer buffer;
                if (uniform_it < bound.uniform.size())
                    buffer = bound.uniform[uniform_it++];
                // Storage aliases need a copy; CopyUniform also aligns the GPU address.
                if (buffer.resource && IsStorageBuffer(bindings, buffer.resource))
                    buffer = buffer_runtime.CopyUniform(buffer);
                TraceUniformWord(stage, desc.index, buffer);
                if (root_cbvs) {
                    if (!buffer.resource || !buffer.size)
                        buffer = {zero_uniform_buffer, 0, 65536};
                    root_cbv_addresses.push_back(buffer.resource->handle->GetGPUVirtualAddress() +
                                                 buffer.offset);
                } else {
                    descriptors.WriteCbv(cbv_table, cbv_i, buffer);
                }
                need(buffer.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            }
        }
        // Textures (+ samplers), then texel buffers.
        const u32 texel_count = Shader::NumDescriptors(info->texture_buffer_descriptors);
        const u32 image_buffer_count = Shader::NumDescriptors(info->image_buffer_descriptors);
        const VideoCommon::ImageViewInOut* texture_views =
            view_it + texel_count + image_buffer_count;
        u32 texture_descriptor_index = 0;
        for (const auto& desc : info->texture_descriptors) {
            const u32 scaling_index = texture_descriptor_index++;
            for (u32 i = 0; i < desc.count; ++i, ++srv_i, ++smp_i) {
                ImageView& view = texture_cache.GetImageView((texture_views++)->id);
                if (scaling_index < 32 && view.IsRescaled())
                    texture_scaling_masks[stage] |= 1u << scaling_index;
                auto sampled = view.ImageHandle();
                bool color_feedback = false;
                for (const auto& color : color_resources)
                    color_feedback |= sampled && color == sampled;
                const bool split_feedback = color_feedback && can_split_feedback(view);
                const bool depth_feedback = has_depth && sampled == depth_view->ImageHandle();
                if ((color_feedback || depth_feedback) && !split_feedback) {
                    const bool stencil_writes =
                        regs.stencil_enable != 0 &&
                        ((regs.stencil_front_mask & 0xff) != 0 ||
                         (regs.stencil_two_side_enable && (regs.stencil_back_mask & 0xff) != 0));
                    if (!color_feedback &&
                        SelectFeedbackMode(depth_feedback, regs.depth_write_enabled != 0,
                                           stencil_writes) == FeedbackMode::ReadOnlyDepth) {
                        readonly_depth = true;
                    } else {
                        sampled = CopyFeedback(sampled, &view.range);
                        if (!sampled) {
                            SkipDraw("feedback memory budget");
                            return;
                        }
                    }
                }
                // Diagnostic: NXBOX_D3D12_SAMPLE_COPY=1 samples array textures through a fresh
                // copy of the cached resource. If glyphs change, the original resource's GPU
                // state (upload ordering, metadata, barriers) is at fault, not its guest data.
                static const bool sample_copy = BisectEnabled("NXBOX_D3D12_SAMPLE_COPY");
                if (sample_copy && sampled && sampled == view.ImageHandle() &&
                    desc.type == Shader::TextureType::ColorArray2D) {
                    if (const auto copy = CopyFeedback(sampled, &view.range)) {
                        static bool reported = false;
                        if (!reported) {
                            reported = true;
                            Diagnostic(fmt::format("D3D12_SAMPLE_COPY substituted gpu={:x}",
                                                   view.GpuAddr()));
                        }
                        sampled = copy;
                    }
                }
                if (descriptors.WriteImageSrv(srv_table, srv_i, view,
                                              SrvDimension(desc.type, desc.is_multisample),
                                              sampled)) {
                    if (split_feedback) {
                        const auto resource_desc = sampled->handle->GetDesc();
                        const bool volume =
                            resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                        const u32 first_layer = volume ? 0u : view.range.base.layer;
                        const u32 layers = volume ? 1u : view.range.extent.layers;
                        auto& subresources =
                            sampled_subresources.emplace_back(sampled, std::vector<u32>{}).second;
                        for (u32 layer = first_layer; layer < first_layer + layers; ++layer) {
                            for (s32 mip = view.range.base.level;
                                 mip < view.range.base.level + view.range.extent.levels; ++mip) {
                                // D3D12CalcSubresource(mip, layer, 0, mips, layers).
                                subresources.push_back(mip + layer * resource_desc.MipLevels);
                            }
                        }
                        context.Profile().Count(FrameCount::FeedbackAvoided);
                    } else {
                        need(sampled, SrvState);
                    }
                }
                if (glyph_trace && sampled && stage != 0) {
                    const auto res = sampled->handle->GetDesc();
                    const auto& srv = view.Description();
                    Diagnostic(fmt::format(
                        "D3D12_GLYPH_TEX frame={} stage={} gpu={:x} res_fmt={} res={}x{}x{} "
                        "mips={} srv_dim={} first_layer={} layers={} desc_type={}",
                        texture_cache.CaptureFrame(), stage, view.GpuAddr(),
                        static_cast<int>(res.Format), res.Width, res.Height, res.DepthOrArraySize,
                        res.MipLevels, static_cast<int>(srv.ViewDimension),
                        view.range.base.layer, view.range.extent.layers,
                        static_cast<int>(desc.type)));
                }
                const Sampler& sampler = texture_cache.GetSampler(*sampler_it++);
                if (!reuse_sampler_plan || validate_sampler_plan)
                    descriptors.WriteSampler(sampler_table, smp_i, sampler, desc.is_depth);
                // Diagnostic: describe each distinct vertex-stage cube binding once.
                static const bool vs_texture_trace = BisectEnabled("NXBOX_D3D12_VS_TEXTURE_TRACE");
                if (vs_texture_trace && stage == 0 && !sampled &&
                    (desc.type == Shader::TextureType::ColorCube ||
                     desc.type == Shader::TextureType::ColorArrayCube)) {
                    static std::set<u32> traced_null;
                    const u32 tic = (texture_views - 1)->index;
                    if (traced_null.size() < 64 && traced_null.insert(tic).second)
                        Diagnostic(fmt::format(
                            "D3D12_VS_TEXTURE_NULL tic={} view_id={} gpu={:x} view_type={} "
                            "cbuf={} offset={:x} cbuf_addr={:x} via_header={}",
                            tic, (texture_views - 1)->id.index, view.GpuAddr(),
                            static_cast<int>(view.type), desc.cbuf_index, desc.cbuf_offset,
                            maxwell3d->state.shader_stages[stage].const_buffers[desc.cbuf_index]
                                .address,
                            regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding));
                }
                if (vs_texture_trace && stage == 0 && sampled &&
                    (desc.type == Shader::TextureType::ColorCube ||
                     desc.type == Shader::TextureType::ColorArrayCube)) {
                    static std::set<std::pair<GPUVAddr, u64>> traced;
                    const auto smp = MakeSamplerDesc(sampler.Entry(), desc.is_depth);
                    u64 smp_hash{};
                    std::memcpy(&smp_hash, &smp, sizeof(smp_hash));
                    smp_hash ^= static_cast<u64>(smp.AddressU) << 32 ^
                                std::bit_cast<u32>(smp.MaxLOD) ^ std::bit_cast<u32>(smp.MinLOD);
                    if (traced.size() < 64 && traced.emplace(view.GpuAddr(), smp_hash).second) {
                        const auto& srv = view.Description();
                        const auto res = sampled->handle->GetDesc();
                        Diagnostic(fmt::format(
                            "D3D12_VS_TEXTURE gpu={:x} res_fmt={} res={}x{}x{} mips={} srv_fmt={} "
                            "srv_dim={} srv_mip={} srv_levels={} filter={:x} addr={},{},{} "
                            "bias={} minlod={} maxlod={} aniso={} cmp={} tsc_cmp={} is_depth={}",
                            view.GpuAddr(), static_cast<int>(res.Format), res.Width, res.Height,
                            res.DepthOrArraySize, res.MipLevels, static_cast<int>(srv.Format),
                            static_cast<int>(srv.ViewDimension), srv.TextureCube.MostDetailedMip,
                            srv.TextureCube.MipLevels, static_cast<u32>(smp.Filter),
                            static_cast<int>(smp.AddressU), static_cast<int>(smp.AddressV),
                            static_cast<int>(smp.AddressW), smp.MipLODBias, smp.MinLOD,
                            smp.MaxLOD, smp.MaxAnisotropy, static_cast<int>(smp.ComparisonFunc),
                            sampler.Entry().depth_compare_enabled.Value(), desc.is_depth));
                    }
                }
            }
        }
        for (u32 i = 0; i < texel_count; ++i, ++srv_i) {
            if (texel_it < bound.texel.size()) {
                const auto& buffer = bound.texel[texel_it++];
                descriptors.WriteTexelSrv(srv_table, srv_i, buffer);
                need(buffer.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            } else {
                descriptors.WriteNullSrv(srv_table, srv_i, D3D12_SRV_DIMENSION_BUFFER);
            }
        }
        // Storage buffers, image buffers, images.
        for (const auto& desc : info->storage_buffers_descriptors) {
            for (u32 i = 0; i < desc.count; ++i, ++uav_i) {
                if (storage_it < bound.storage.size()) {
                    const auto& buffer = bound.storage[storage_it++];
                    descriptors.WriteStorageUav(uav_table, uav_i, buffer);
                    need(buffer.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                } else {
                    descriptors.WriteNullUav(uav_table, uav_i);
                }
            }
        }
        const u32 image_start = uav_i;
        uav_i += Shader::NumDescriptors(info->image_descriptors);
        for (u32 i = 0; i < image_buffer_count; ++i, ++uav_i) {
            if (texel_it < bound.texel.size()) {
                const auto& buffer = bound.texel[texel_it++];
                descriptors.WriteTexelUav(uav_table, uav_i, buffer);
                need(buffer.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            } else {
                descriptors.WriteNullUav(uav_table, uav_i);
            }
        }
        const u32 stage_uav_end = uav_i;
        uav_i = image_start;
        for (const auto& desc : info->image_descriptors) {
            const auto format = PixelFormatFromImageFormat(desc.format);
            if (!format && desc.format != Shader::ImageFormat::Typeless) {
                SkipDraw("unsupported graphics storage image format");
                return;
            }
            for (u32 i = 0; i < desc.count; ++i, ++uav_i) {
                const auto view_id = (texture_views++)->id;
                auto& view = texture_cache.GetImageView(view_id);
                if (desc.is_written && view_id != VideoCommon::NULL_IMAGE_VIEW_ID)
                    written_images.push_back(view_id);
                if (uav_i < 32 && view.IsRescaled())
                    image_scaling_mask |= 1u << uav_i;
                const auto guest_format = format.value_or(view.format);
                if (const auto problem =
                        StorageImageProblem(context, guest_format, desc.format, desc.is_integer,
                                            desc.is_read, desc.is_written)) {
                    SkipDraw(problem);
                    return;
                }
                if (const auto resource = view.ImageHandle()) {
                    const auto dimension = view.Description().ViewDimension;
                    if (!(resource->handle->GetDesc().Flags &
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
                        (dimension != D3D12_SRV_DIMENSION_TEXTURE1D &&
                         dimension != D3D12_SRV_DIMENSION_TEXTURE1DARRAY &&
                         dimension != D3D12_SRV_DIMENSION_TEXTURE2D &&
                         dimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY &&
                         dimension != D3D12_SRV_DIMENSION_TEXTURE3D)) {
                        SkipDraw("unsupported graphics storage image view");
                        return;
                    }
                }
                if (!descriptors.WriteImageUav(
                        uav_table, uav_i, view,
                        static_cast<DXGI_FORMAT>(MaxwellFormat(guest_format).view_format))) {
                    SkipDraw("unsupported graphics storage image view");
                    return;
                }
                need(view.ImageHandle(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
        uav_i = stage_uav_end;
        view_it += texel_count + image_buffer_count +
                   Shader::NumDescriptors(info->texture_descriptors) +
                   Shader::NumDescriptors(info->image_descriptors);
    }
    for (u32 i = cbv_i; i < cbv_size; ++i)
        descriptors.WriteNullCbv(cbv_table, i);
    for (u32 i = srv_i; i < srv_size; ++i)
        descriptors.WriteNullSrv(srv_table, i, D3D12_SRV_DIMENSION_TEXTURE2D);
    for (u32 i = uav_i; i < uav_size; ++i)
        descriptors.WriteNullUav(uav_table, i);
    if (!reuse_sampler_plan || validate_sampler_plan) {
        for (u32 i = smp_i; i < sampler_table.count; ++i)
            descriptors.WriteNullSampler(sampler_table, i);
        if (validate_sampler_plan && sampler_plan.keys != resolved_sampler_plan.keys)
            throw std::invalid_argument("resolved sampler plan differs from reconstruction");
        if (cache_sampler_plan)
            resolved_sampler_plan.keys = sampler_plan.keys;
    }

    const auto predicate_trace = query_runtime.PreparePredicateTrace();

    // Reserve after feedback/uniform copies. A submission invalidates both remembered tables.
    const u64 table_tick = context.CurrentTick();
    const bool reuse_views = descriptors.CanReuseDrawTable(view_plan);
    const bool reuse_samplers = descriptors.CanReuseDrawTable(sampler_plan, true);
    descriptors.Reserve(reuse_views ? 0u : view_block.count, reuse_samplers ? 0u : sampler_size);
    if (context.CurrentTick() != table_tick)
        descriptors.Reserve(view_block.count, sampler_size);

    static const bool inject_draw_skip = BisectEnabled("NXBOX_D3D12_INJECT_DRAW_SKIP");
    if (!injected_draw_skip && inject_draw_skip) {
        injected_draw_skip = true;
        SkipDraw("injected descriptor recording failure");
        return;
    }

    // Render targets: rewrite the fixed RTV/DSV slots only when the attachments change.
    auto dsv_flags = D3D12_DSV_FLAG_NONE;
    if (has_depth && readonly_depth) {
        dsv_flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        const auto format = DsvFormat(depth_view->Description().Format);
        if (format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
            dsv_flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    }
    BoundTargets targets;
    targets.num_rts = num_rts;
    targets.layers = render_area.layers;
    targets.dsv_flags = has_depth ? dsv_flags : D3D12_DSV_FLAG_NONE;
    for (u32 slot = 0; slot < num_rts; ++slot) {
        if (const auto* view = color_views[slot]) {
            targets.resources[slot] = color_resources[slot];
            targets.views[slot] = {view->range.base.level, view->range.base.layer,
                                   static_cast<s32>(view->Description().Format)};
        }
    }
    if (has_depth) {
        targets.resources[VideoCommon::NUM_RT] = depth_view->ImageHandle();
        targets.views[VideoCommon::NUM_RT] = {depth_view->range.base.level,
                                              depth_view->range.base.layer,
                                              static_cast<s32>(depth_view->Description().Format)};
    }
    if (targets != bound_targets) {
        // OMSetRenderTargets captures descriptor contents at recording time.
        // The fixed handles alone cannot detect these CPU-side rewrites.
        context.InvalidateRecordedState();
        const SIZE_T rtv_stride =
            context.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (u32 slot = 0; slot < num_rts; ++slot) {
            const auto replacement =
                color_views[slot] && color_resources[slot] != color_views[slot]->ImageHandle()
                    ? color_resources[slot]
                    : ResourcePtr{};
            descriptors.CreateRtv({rtv_base.ptr + slot * rtv_stride}, color_views[slot],
                                  replacement, render_area.layers);
        }
        if (has_depth)
            descriptors.CreateDsv(dsv_handle, *depth_view, dsv_flags, render_area.layers);
        // Holding the resources keeps their addresses from being reused while cached.
        bound_targets = std::move(targets);
    }
    // Remaining attachment/UAV and storage/geometry aliases cannot have legal
    // combined states. Reject only this draw, leaving the GPU thread alive.
    for (u32 slot = 0; slot < num_rts; ++slot) {
        if (color_views[slot])
            need(color_resources[slot], D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (has_depth)
        need(depth_view->ImageHandle(),
             readonly_depth ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (invalid_states) {
        SkipDraw("conflicting resource states");
        return;
    }
    if (readonly_depth)
        ++feedback_readonly_depth;

    view_block = descriptors.FinishDrawTable(view_plan);
    cbv_table = view_block.Slice(0, cbv_size);
    srv_table = view_block.Slice(cbv_size, srv_size);
    uav_table = view_block.Slice(cbv_size + srv_size, uav_size);
    sampler_table = descriptors.FinishDrawTable(sampler_plan, true);

    draw_phase.emplace(context.Profile(), FramePhase::DrawRecording);
    // ---- 4. Record state (no more allocations from here on) ----
    // Recording only touches resources already resolved above and this thread's command list.
    // Releasing the caches here lets guest writes (OnCPUWrite) through instead of stalling the
    // game core for the whole recording; SM3DW spent ~25% of cpu0 samples waiting on this.
    // The draw trace below still reads cache views, so it keeps the locks.
    // Shader image writes publish cache modification ticks after recording the draw.
    if (!draw_trace_enabled && written_images.empty())
        texture_lock.unlock();
    auto* const commands = context.Commands();
    descriptors.SetHeaps(commands, true);
    // State collection has already merged aliases and rejected conflicting writes.
    // Submit the same ordered transitions in one call even without split feedback.
    context.TransitionBatch(state_scratch);
    if (subresource_feedback) {
        for (const auto& [resource, subresources] : sampled_subresources)
            context.TransitionSubresources(resource, subresources, SrvState);
    }

    auto& recorded = context.DrawState();
    if (context.RecordState(recorded.root_signature.Update(root_signature))) {
        commands->SetGraphicsRootSignature(root_signature);
        recorded.ClearRootArguments();
    }
    if (context.RecordState(recorded.pipeline.Update(pso)))
        commands->SetPipelineState(pso);
    const auto topology = converted_indices ? converted_indices->topology : pipeline->Topology();
    if (context.RecordState(recorded.topology.Update(topology)))
        commands->IASetPrimitiveTopology(topology);
    static const bool root_binding_batch = !BisectEnabled("NXBOX_D3D12_NO_ROOT_BINDING_BATCH");
    const bool batch_root_bindings = commands->stream && root_binding_batch;
    // At most 64 root parameters fit in a D3D12 root signature. Leave unused
    // scratch uninitialized; only populated entries are copied into the stream.
    std::array<RecordedRootBinding, 64> root_bindings;
    size_t root_binding_count = 0;
    const auto bind_table = [&](UINT parameter, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
        if (context.RecordState(recorded.root_args.tables.Update(parameter, handle.ptr))) {
            if (batch_root_bindings)
                root_bindings[root_binding_count++] = {handle.ptr, parameter, true};
            else
                commands->SetGraphicsRootDescriptorTable(parameter, handle);
        }
    };
    if (!root_cbvs)
        bind_table(RootCbvTable, cbv_table.gpu);
    const UINT table_shift = root_cbvs ? 1u : 0u;
    bind_table(RootSrvTable - table_shift, srv_table.gpu);
    bind_table(RootUavTable - table_shift, uav_table.gpu);
    bind_table(RootSamplerTable - table_shift, sampler_table.gpu);
    // Opt in independently: EndDraw deliberately invalidates general draw state
    // for a known title workaround. Track actual root writes without relaxing it.
    const bool root_cbv_reuse = commands->reuse_graphics_root_cbvs;
    for (UINT i = 0; i < root_cbv_addresses.size(); ++i) {
        const bool changed =
            recorded.root_args.cbvs.Update(kFirstRootCbv + i, root_cbv_addresses[i]);
        const bool root_changed =
            root_cbv_reuse
                ? commands->graphics_root_cbvs.cbvs.Update(kFirstRootCbv + i, root_cbv_addresses[i])
                : changed;
        if (context.RecordState(root_cbv_reuse ? root_changed : changed)) {
            if (batch_root_bindings)
                root_bindings[root_binding_count++] = {root_cbv_addresses[i], kFirstRootCbv + i,
                                                       false};
            else
                commands->SetGraphicsRootConstantBufferView(kFirstRootCbv + i,
                                                            root_cbv_addresses[i]);
            context.Profile().Count(FrameCount::RootCbvBinds);
        }
    }
    if (batch_root_bindings && commands->validation.enabled) {
        for (size_t i = 0; i < root_binding_count; ++i) {
            const auto& binding = root_bindings[i];
            if (binding.table)
                commands->validation.Table("SetGraphicsRootDescriptorTable", binding.parameter,
                                           binding.address);
            else
                commands->validation.Root("SetGraphicsRootConstantBufferView", binding.parameter,
                                          binding.address, true);
        }
    }
    if (batch_root_bindings)
        RecordGraphicsRootBindings<RecordingTarget>(*commands->stream, root_bindings.data(),
                                                    root_binding_count);

    const std::array<u64, 4> target_key{num_rts, num_rts ? rtv_base.ptr : 0,
                                        has_depth ? dsv_handle.ptr : 0, has_depth ? 1u : 0u};
    if (context.RecordState(recorded.targets.Update(target_key)))
        commands->OMSetRenderTargets(num_rts, num_rts ? &rtv_base : nullptr, TRUE,
                                     has_depth ? &dsv_handle : nullptr);
    context.GraphicsStateTracker().SetRenderArea(render_area.width, render_area.height);
    const GraphicsStateInputs dynamic_graphics{
        context.GraphicsStateTracker().Update(*maxwell3d),
        {},
        formats,
        static_cast<u32>(draw_state.topology),
        regs.zeta_enable,
        is_indexed,
        {regs.pipelines[2].enable != 0, regs.pipelines[3].enable != 0}};
    const DynamicStateInputs current_dynamic{dynamic_graphics,   render_area.width,
                                             render_area.height, static_cast<u32>(topology),
                                             pipeline,           texture_cache.IsRescaling()};
    const bool reuse_dynamic = StateFastpathEnabled() && dynamic_inputs.Matches(current_dynamic);
    if (!reuse_dynamic || StateVerifyEnabled()) {
        auto slow = ComputeDynamicState(regs, attachment_state, formats, render_area.width,
                                        render_area.height, *pipeline, texture_cache.IsRescaling());
        if (StateVerifyEnabled() && reuse_dynamic) {
            const auto compare = [](std::string_view what, const auto& fast, const auto& slow) {
                if (std::memcmp(&fast, &slow, sizeof(fast)) != 0)
                    StateVerifyMismatch(what, &fast, &slow, sizeof(fast));
            };
            compare("viewport", cached_dynamic_state.viewports, slow.viewports);
            compare("viewport", cached_dynamic_state.y_flip, slow.y_flip);
            compare("viewport", cached_dynamic_state.words, slow.words);
            compare("scissor", cached_dynamic_state.scissors, slow.scissors);
            compare("stencil", cached_dynamic_state.stencil, slow.stencil);
            compare("blend", cached_dynamic_state.blend, slow.blend);
            compare("depth_bounds", cached_dynamic_state.depth_bounds, slow.depth_bounds);
        }
        cached_dynamic_state = slow;
        dynamic_inputs.Store(current_dynamic);
    }
    const auto& viewports = cached_dynamic_state.viewports;
    const auto& scissors = cached_dynamic_state.scissors;
    static const bool depth_trace = BisectEnabled("NXBOX_D3D12_DEPTH_TRACE");
    if (has_depth && depth_trace && depth_trace_draws++ < 4) {
        const auto& viewport = viewports[0];
        Diagnostic(fmt::format(
            "D3D12_DEPTH_DRAW frame={} area={}x{} dsv={} depth_test={} "
            "depth_write={} compare={} stencil={} clip={} depth_mode={} "
            "z_scale={} z_translate={} viewport_z=[{},{}] indexed={} "
            "vertices={} instances={} base_vertex={} base_instance={}",
            feedback_frame, render_area.width, render_area.height, formats.depth,
            attachment_state.depth_enable, attachment_state.depth_write,
            attachment_state.depth_func, attachment_state.stencil_enable,
            attachment_state.depth_clip, static_cast<u32>(regs.depth_mode),
            regs.viewport_transform[0].scale_z, regs.viewport_transform[0].translate_z,
            viewport.MinDepth, viewport.MaxDepth, submitted.is_indexed, submitted.num_vertices,
            submitted.num_instances, submitted.base_vertex, submitted.base_instance));
    }
    if (context.RecordState(recorded.viewports.Update(viewports.data(), viewports.size())))
        commands->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
    if (context.RecordState(recorded.scissors.Update(scissors.data(), scissors.size())))
        commands->RSSetScissorRects(static_cast<UINT>(scissors.size()), scissors.data());
    if (context.RecordState(recorded.vertices.Update(vertex_views.data(), num_vertex_views)))
        commands->IASetVertexBuffers(0, num_vertex_views, vertex_views.data());
    if (needs_index && context.RecordState(recorded.index.Update(index_view)))
        commands->IASetIndexBuffer(&index_view);

    auto words = cached_dynamic_state.words;
    words[4] = std::bit_cast<u32>(
        texture_cache.IsRescaling() ? Settings::values.resolution_info.down_factor : 1.0f);
    words[7] = image_scaling_mask;
    for (size_t stage = 0; stage < texture_scaling_masks.size(); ++stage)
        words[11 + stage] = texture_scaling_masks[stage];
    words[8] = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first;
    words[9] = draw_state.base_instance;
    words[10] = indirect_draw_id;
    const auto& stencil = cached_dynamic_state.stencil;
    if (context.RecordState(recorded.stencil.Update(stencil))) {
        if (stencil[0]) {
            commands->OMSetFrontAndBackStencilRef(stencil[1], stencil[2]);
        } else {
            commands->OMSetStencilRef(stencil[1]);
        }
    }
    const auto& blend = cached_dynamic_state.blend;
    if (context.RecordState(recorded.blend.Update(blend)))
        commands->OMSetBlendFactor(blend.data());
    if (attachment_state.depth_bounds) {
        const auto& depth_bounds = cached_dynamic_state.depth_bounds;
        if (context.RecordState(recorded.depth_bounds.Update(depth_bounds))) {
            commands->OMSetDepthBounds(depth_bounds[0], depth_bounds[1]);
        }
    }
    if (context.RecordState(recorded.root_args.constants.Update(words.data(), words.size())))
        commands->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(words.size()), words.data(),
                                                0);

    draw_phase.emplace(context.Profile(), FramePhase::QueryState);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              regs.zpass_pixel_count_enable != 0);
    if (feedback_info)
        transform_feedback.Bind(true);
    else
        transform_feedback.Unbind(regs.transform_feedback_enabled == 0, true);
    query_runtime.BeginDraw(pipeline->StageInfo(3) != nullptr);
    {
        const FrameProfile::Scope profile_record{context.Profile(), FramePhase::DrawRecording};
        if (glyph_trace) {
            ++glyph_trace_draws;
            for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES; ++stage) {
                const Shader::Info* info = pipeline->StageInfo(stage);
                if (!info || info->storage_buffers_descriptors.empty() ||
                    bindings.stages[stage].storage.empty())
                    continue;
                const auto& desc = info->storage_buffers_descriptors[0];
                const GPUVAddr cbuf_slot =
                    maxwell3d->state.shader_stages[stage].const_buffers[desc.cbuf_index].address +
                    desc.cbuf_offset;
                const GPUVAddr ssbo = gpu_memory->Read<u64>(cbuf_slot);
                const u64 ssbo_size = gpu_memory->Read<u64>(cbuf_slot + 8);
                std::array<u8, GlyphTraceBytes> guest_bytes{};
                if (ssbo)
                    gpu_memory->ReadBlockUnsafe(ssbo, guest_bytes.data(), guest_bytes.size());
                const auto& host = bindings.stages[stage].storage[0];
                u32 host_copy = 0;
                if (host.resource && host.size) {
                    host_copy = std::min<u32>(GlyphTraceBytes, host.size);
                    query_runtime.PauseHostConditionalRendering();
                    context.Transition(glyph_readback, D3D12_RESOURCE_STATE_COPY_DEST);
                    const auto previous = host.resource->state;
                    context.Transition(host.resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    context.BeforeCopyWrite(glyph_readback, 0, host_copy);
                    context.Commands()->CopyBufferRegion(glyph_readback->handle.Get(), 0,
                                                         host.resource->handle.Get(),
                                                         host.offset, host_copy);
                    context.Transition(host.resource, previous);
                    query_runtime.ResumeHostConditionalRendering();
                }
                // Constant buffers 0 and 1 as the guest holds them, and constant buffer 1 as the
                // host bound it (descriptor order).
                const auto& stage_cbufs = maxwell3d->state.shader_stages[stage].const_buffers;
                std::array<u8, GlyphTraceBytes> guest_cbuf0{}, guest_cbuf1{};
                gpu_memory->ReadBlockUnsafe(stage_cbufs[0].address, guest_cbuf0.data(),
                                            guest_cbuf0.size());
                gpu_memory->ReadBlockUnsafe(stage_cbufs[1].address, guest_cbuf1.data(),
                                            guest_cbuf1.size());
                u32 host_cbuf1_copy = 0;
                {
                    size_t uniform_index = 0;
                    for (const auto& cbuf_desc : info->constant_buffer_descriptors) {
                        if (cbuf_desc.index == 1 &&
                            uniform_index < bindings.stages[stage].uniform.size()) {
                            const auto& cbuf = bindings.stages[stage].uniform[uniform_index];
                            if (cbuf.resource && cbuf.size) {
                                host_cbuf1_copy = std::min<u32>(GlyphTraceBytes, cbuf.size);
                                query_runtime.PauseHostConditionalRendering();
                                const auto previous = cbuf.resource->state;
                                context.Transition(cbuf.resource,
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
                                context.BeforeCopyWrite(glyph_readback, GlyphTraceBytes,
                                                        host_cbuf1_copy);
                                context.Commands()->CopyBufferRegion(
                                    glyph_readback->handle.Get(), GlyphTraceBytes,
                                    cbuf.resource->handle.Get(), cbuf.offset, host_cbuf1_copy);
                                context.Transition(cbuf.resource, previous);
                                query_runtime.ResumeHostConditionalRendering();
                            }
                            break;
                        }
                        uniform_index += cbuf_desc.count;
                    }
                }
                // Attribute 0 (the unit-quad corners) as the guest vertex stream holds it.
                std::array<u8, 192> guest_vertex{};
                u32 vertex_stride = 0;
                u32 vertex_offset = 0;
                {
                    const auto& attribute0 = native_pipeline->State().attributes[0];
                    if (attribute0.enabled == 1) {
                        const auto& stream = regs.vertex_streams[attribute0.slot];
                        vertex_stride = stream.stride.Value();
                        vertex_offset = attribute0.offset;
                        gpu_memory->ReadBlockUnsafe(stream.Address() + attribute0.offset,
                                                    guest_vertex.data(),
                                                    std::min<size_t>(guest_vertex.size(),
                                                                     size_t{vertex_stride} * 8));
                    }
                }
                const bool is_indexed_draw = submitted.is_indexed;
                const u32 draw_vertices = submitted.num_vertices;
                const u32 base_vertex_value = submitted.base_vertex;
                const auto hex = [](std::span<const u8> bytes) {
                    std::string out;
                    for (size_t i = 0; i < bytes.size(); ++i)
                        out += fmt::format("{:02x}{}", bytes[i], (i % 16) == 15 ? " " : "");
                    return out;
                };
                const u64 frame = texture_cache.CaptureFrame();
                const u32 instances = submitted.num_instances;
                const u32 base_instance = submitted.base_instance;
                const u32 host_offset = host.offset;
                const u32 host_size = host.size;
                context.AfterCompletion([readback = glyph_readback, guest_bytes, hex, frame,
                                         instances, base_instance, ssbo, ssbo_size, host_copy,
                                         host_offset, host_size, guest_cbuf0, guest_cbuf1,
                                         host_cbuf1_copy, guest_vertex, vertex_stride,
                                         vertex_offset, is_indexed_draw, draw_vertices,
                                         base_vertex_value, draw_index = glyph_trace_draws] {
                    Diagnostic(fmt::format(
                        "D3D12_GLYPH_DRAW draw={} indexed={} vertices={} base_vertex={} "
                        "base_instance={} vb0_stride={} vb0_offset={} vb0={}",
                        draw_index, is_indexed_draw, draw_vertices, base_vertex_value,
                        base_instance, vertex_stride, vertex_offset, hex(guest_vertex)));
                    Diagnostic(fmt::format("D3D12_GLYPH_CBUF0 draw={} guest={}", draw_index,
                                           hex(guest_cbuf0)));
                    Diagnostic(fmt::format("D3D12_GLYPH_CBUF1 draw={} guest={}", draw_index,
                                           hex(guest_cbuf1)));
                    Diagnostic(fmt::format(
                        "D3D12_GLYPH_CBUF1_HOST draw={} copied={} host={}", draw_index,
                        host_cbuf1_copy,
                        hex(readback->mapped.subspan(GlyphTraceBytes, host_cbuf1_copy))));
                    Diagnostic(fmt::format(
                        "D3D12_GLYPH_SSBO draw={} frame={} instances={} base_instance={} "
                        "gpu={:x} align16={} size={} host_offset={} host_size={}",
                        draw_index, frame, instances, base_instance, ssbo, ssbo & 15, ssbo_size,
                        host_offset, host_size));
                    // Records start at the guest address; the host range starts 16-aligned down.
                    for (u32 record = 0; record < 3; ++record) {
                        const size_t guest_at = record * 64;
                        const size_t host_at = guest_at + (ssbo & 15);
                        Diagnostic(fmt::format(
                            "D3D12_GLYPH_REC draw={} rec={} guest={} host={}", draw_index, record,
                            hex(std::span<const u8>{guest_bytes}.subspan(guest_at, 64)),
                            host_at + 64 <= host_copy
                                ? hex(readback->mapped.subspan(host_at, 64))
                                : std::string{"n/a"}));
                    }
                });
                break;
            }
        }
        context.MarkWork("guest draws");
        if (auto* profile = context.PassProfile()) {
            const auto size = framebuffer->Size();
            profile->Target({size.width, size.height, formats.colors[0], formats.depth,
                             native_pipeline->PixelGuestHash()});
        }
        u64 trace_draw = 0;
        if (draw_trace_enabled) {
            if (texture_cache.CaptureFrame() == draw_trace_frame && draw_trace_lines < 3000) {
                trace_draw = ++draw_trace_lines;
                std::string trace_rtvs;
                for (u32 trace_slot = 0; trace_slot < num_rts; ++trace_slot) {
                    trace_rtvs += fmt::format("{}{}", trace_slot ? "," : "",
                                              color_views[trace_slot]
                                                  ? formats.colors[trace_slot]
                                                  : static_cast<u32>(DXGI_FORMAT_R8G8B8A8_UNORM));
                }
                const auto& trace_viewport = viewports[0];
                const auto& trace_scissor = scissors[0];
                const auto& trace_vertex = vertex_views[0];
                const auto trace_float = [&words](size_t offset) {
                    return std::bit_cast<float>(words[offset]);
                };
                Diagnostic(fmt::format(
                    "D3D12_DRAW_TRACE frame={} draw={} pass={} rt0_gpu={:x} topology={} "
                    "count={} instances={} is_indexed={} guest_indexed={} generated_index={} "
                    "num_rts={} rtv=[{}] has_depth={} dsv={} readonly_depth={} {} "
                    "viewport=[{},{},{},{},{},{}] scissor=[{},{},{},{}] render_area=[{},{},{}] "
                    "nx_render_area=[{},{},{},{}] nx_y_direction={} nx_resolution_down_factor={} "
                    "vb0_addr_lo={:08x} vb0_size={} vb0_stride={} ib_size={} ib_format={} "
                    "ps_hash={:016x} vs_hash={:016x}",
                    draw_trace_frame, trace_draw, texture_cache.CapturePassSequence(),
                    color_views[0] ? color_views[0]->gpu_addr : GPUVAddr{0},
                    static_cast<u32>(draw_state.topology), submitted.num_vertices,
                    submitted.num_instances, submitted.is_indexed, is_indexed,
                    quads && converted_indices.has_value(), num_rts, trace_rtvs, has_depth,
                    formats.depth, readonly_depth, native_pipeline->DrawTracePso(),
                    trace_viewport.TopLeftX, trace_viewport.TopLeftY, trace_viewport.Width,
                    trace_viewport.Height, trace_viewport.MinDepth, trace_viewport.MaxDepth,
                    trace_scissor.left, trace_scissor.top, trace_scissor.right,
                    trace_scissor.bottom, render_area.width, render_area.height, render_area.layers,
                    trace_float(0), trace_float(1), trace_float(2), trace_float(3), trace_float(5),
                    trace_float(4), static_cast<u32>(trace_vertex.BufferLocation & 0xffffffffULL),
                    trace_vertex.SizeInBytes, trace_vertex.StrideInBytes, index_view.SizeInBytes,
                    static_cast<u32>(index_view.Format), native_pipeline->PixelGuestHash(),
                    native_pipeline->VertexGuestHash()));
                if (trace_vertex_bytes) {
                    ++vb_trace_draws;
                    std::array<u32, MaxTraceStreams> trace_streams{0, 0};
                    u32 trace_stream_count = 1;
                    const auto& trace_attributes = native_pipeline->State().attributes;
                    for (const auto& trace_attribute : trace_attributes) {
                        if (trace_attribute.enabled == 1 && trace_attribute.slot != 0) {
                            trace_streams[trace_stream_count++] = trace_attribute.slot;
                            break;
                        }
                    }
                    // Copies must not be suppressed by the guest's conditional rendering.
                    query_runtime.PauseHostConditionalRendering();
                    context.Transition(vertex_trace_readback, D3D12_RESOURCE_STATE_COPY_DEST);
                    for (u32 trace_stream_index = 0; trace_stream_index < trace_stream_count;
                         ++trace_stream_index) {
                        const u32 trace_stream_slot = trace_streams[trace_stream_index];
                        // A stream can feed several IA slots. Prefer its first enabled attribute.
                        u32 trace_input_slot = static_cast<u32>(trace_attributes.size());
                        for (u32 trace_input = 0; trace_input < trace_attributes.size();
                             ++trace_input) {
                            if (trace_attributes[trace_input].enabled == 1 &&
                                trace_attributes[trace_input].slot == trace_stream_slot) {
                                trace_input_slot = trace_input;
                                break;
                            }
                        }
                        if (trace_input_slot == trace_attributes.size()) {
                            // Unused stream 0 has no consumed IA binding. A disabled IA slot
                            // still has a bound zero view; report it explicitly when available.
                            for (u32 trace_input = 0; trace_input < trace_attributes.size();
                                 ++trace_input) {
                                if (trace_attributes[trace_input].enabled != 1) {
                                    trace_input_slot = trace_input;
                                    break;
                                }
                            }
                        }
                        const bool trace_bound = trace_input_slot < trace_attributes.size();
                        const bool trace_used =
                            trace_bound && trace_attributes[trace_input_slot].enabled == 1;
                        const u32 trace_first =
                            trace_used && trace_attributes[trace_input_slot].instanced
                                ? (trace_attributes[trace_input_slot].divisor == 0
                                       ? 0u
                                       : submitted.base_instance)
                                : submitted.base_vertex;
                        const auto& trace_guest_stream = regs.vertex_streams[trace_stream_slot];
                        const u32 trace_attribute_offset =
                            trace_used ? trace_attributes[trace_input_slot].offset : 0u;
                        const GPUVAddr trace_guest_address =
                            trace_guest_stream.Address() + trace_attribute_offset +
                            u64{trace_first} * trace_guest_stream.stride.Value();
                        std::array<u8, VertexTraceBytes> trace_guest_bytes{};
                        gpu_memory->ReadBlockUnsafe(trace_guest_address, trace_guest_bytes.data(),
                                                    trace_guest_bytes.size());
                        const auto trace_hex = [](std::span<const u8> trace_bytes) {
                            std::string trace_result;
                            trace_result.reserve(trace_bytes.size() * 2);
                            for (const u8 trace_byte : trace_bytes)
                                trace_result += fmt::format("{:02x}", trace_byte);
                            return trace_result;
                        };
                        const std::string trace_guest_hex = trace_hex(trace_guest_bytes);
                        const ResourcePtr trace_resource =
                            trace_bound ? trace_vertex_resources[trace_input_slot] : ResourcePtr{};
                        const char* trace_source =
                            !trace_resource || trace_resource == zero_vertex_buffer ? "zero"
                            : trace_resource == geometry.vertex[trace_stream_slot].resource
                                ? "cache"
                                : "padded";
                        const u32 trace_heap =
                            trace_resource ? static_cast<u32>(trace_resource->heap) : 0u;
                        u64 trace_copy_bytes = 0;
                        u64 trace_host_offset = 0;
                        if (trace_resource) {
                            const auto& trace_host_view = vertex_views[trace_input_slot];
                            const u64 trace_view_first =
                                u64{trace_first} * trace_host_view.StrideInBytes;
                            trace_host_offset = trace_host_view.BufferLocation -
                                                trace_resource->handle->GetGPUVirtualAddress() +
                                                trace_view_first;
                            trace_copy_bytes =
                                trace_view_first < trace_host_view.SizeInBytes
                                    ? std::min<u64>(VertexTraceBytes,
                                                    trace_host_view.SizeInBytes - trace_view_first)
                                    : 0;
                            if (trace_copy_bytes) {
                                // Restore the full combined draw state (VB plus any SRV/index
                                // uses). Upload heaps remain GENERIC_READ via Transition.
                                const auto trace_previous_state = trace_resource->state;
                                context.Transition(trace_resource,
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
                                commands->CopyBufferRegion(vertex_trace_readback->handle.Get(),
                                                           u64{trace_stream_index} *
                                                               VertexTraceBytes,
                                                           trace_resource->handle.Get(),
                                                           trace_host_offset, trace_copy_bytes);
                                context.Transition(trace_resource, trace_previous_state);
                            }
                        }
                        // Capture values and the readback owner only: completion never touches
                        // the rasterizer, guest memory, or command recording state.
                        context.AfterCompletion(
                            [trace_readback = vertex_trace_readback, trace_draw = draw_trace_lines,
                             trace_stream_slot, trace_input_slot, trace_used, trace_guest_address,
                             trace_guest_hex, trace_heap, trace_source, trace_copy_bytes,
                             trace_host_offset, trace_stream_index, trace_hex] {
                                const auto trace_host_bytes = trace_readback->mapped.subspan(
                                    static_cast<size_t>(trace_stream_index) * VertexTraceBytes,
                                    static_cast<size_t>(trace_copy_bytes));
                                Diagnostic(fmt::format(
                                    "D3D12_VB_TRACE draw={} slot={} guest={} host={} heap={} "
                                    "source={} guest_gpu={:016x} ia_slot={} used={} "
                                    "host_offset={} host_size={}",
                                    trace_draw, trace_stream_slot, trace_guest_hex,
                                    trace_hex(trace_host_bytes), trace_heap, trace_source,
                                    trace_guest_address, trace_input_slot, trace_used,
                                    trace_host_offset, trace_copy_bytes));
                            });
                    }
                    query_runtime.ResumeHostConditionalRendering();
                }
            }
        }
        query_runtime.TracePredicate(texture_cache.CaptureFrame(),
                                     texture_cache.CapturePassSequence(), trace_draw,
                                     predicate_trace);
        VideoCommon::SubmissionTrace::Log("host_draw_submit draw={} pass={}", trace_draw,
                                          texture_cache.CapturePassSequence());
        draw(commands);
        if (color_views[0])
            pipeline_cache->RecordGraphicsDraw(texture_cache.CaptureFrame());
        trace_submitted = true;
    }
    query_runtime.EndDraw();
    query_runtime.PauseHostConditionalRendering();
    draw_phase.emplace(context.Profile(), FramePhase::DrawCleanup);
    for (const auto& copy : color_copies) {
        const FrameProfile::Scope profile_extent{context.Profile(), FramePhase::ExtentCopies};
        context.Profile().Count(FrameCount::ExtentCopies, copy.region.src_subresource.num_layers);
        auto region = copy.region;
        std::swap(region.src_subresource, region.dst_subresource);
        texture_runtime.BlitHelpers().CopyImage(copy.original, copy.temporary,
                                                std::span{&region, 1});
    }
    if (totals.uav)
        FragmentBarrier();
    if (trace_submitted) {
        texture_cache.NotePassDumpRenderTargets("draw_submission");
        texture_cache.PublishShaderImageWrites(written_images, "graphics_uav");
    }
    // The scope guard releases these leases on every exit, including this one.
    if (!buffer_runtime.BufferFastpathEnabled()) {
        for (auto& stage : bindings.stages)
            stage.Clear(); // drop upload-page references so the staging ring can recycle them
    }
}

// Diagnostic (NXBOX_D3D12_CBUF_TRACE_FRAME=<frame>): word 4 of constant buffer
// NXBOX_D3D12_CBUF_TRACE_INDEX (default 9) as the guest holds it and as the GPU reads it.
void RasterizerD3D12::TraceUniformWord(size_t stage, u32 index, const BoundBuffer& buffer) {
    static const u64 frame = [] {
        const char* value = std::getenv("NXBOX_D3D12_CBUF_TRACE_FRAME");
        return value ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    static const u32 traced = [] {
        const char* value = std::getenv("NXBOX_D3D12_CBUF_TRACE_INDEX");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 9u;
    }();
    if (!frame || index != traced || texture_cache.CaptureFrame() != frame ||
        cbuf_trace_lines >= 200)
        return;
    const u32 line = ++cbuf_trace_lines;
    const auto& cb = maxwell3d->state.shader_stages[stage].const_buffers[index];
    const u32 guest = cb.enabled && cb.address ? gpu_memory->Read<u32>(cb.address + 16) : 0;
    const auto prefix =
        fmt::format("D3D12_CBUF_TRACE line={} stage={} index={} enabled={} guest_addr={:#x} "
                    "guest_size={} guest_word4={:#x} bound_size={} bound_offset={}",
                    line, stage, index, cb.enabled, cb.address, cb.size, guest, buffer.size,
                    buffer.offset);
    if (!buffer.resource || buffer.size < 20) {
        Diagnostic(prefix + " host=unbound");
        return;
    }
    if (buffer.resource->heap != D3D12_HEAP_TYPE_DEFAULT) {
        u32 host{};
        std::memcpy(&host, buffer.resource->mapped.data() + buffer.offset + 16, sizeof(host));
        Diagnostic(fmt::format("{} heap=upload host_word4={:#x}", prefix, host));
        return;
    }
    auto readback = context.CreateBuffer(256, D3D12_HEAP_TYPE_READBACK);
    context.Transition(buffer.resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.BeforeCopyWrite(readback, 0, 4);
    context.Commands()->CopyBufferRegion(readback->handle.Get(), 0, buffer.resource->handle.Get(),
                                         buffer.offset + 16, 4);
    context.AfterCompletion([readback, prefix] {
        u32 host{};
        std::memcpy(&host, readback->mapped.data(), sizeof(host));
        Diagnostic(fmt::format("{} heap=default host_word4={:#x}", prefix, host));
    });
}

bool RasterizerD3D12::PrepareDrawIndices(bool is_indexed, DrawParams& params,
                                         const IGraphicsPipeline& pipeline) {
    const FrameProfile::Scope profile_indices{context.Profile(), FramePhase::IndexConversion};
    const auto& state = maxwell3d->draw_manager.draw_state;
    const auto topology = static_cast<IndexConversion::Topology>(state.topology);
    const auto& regs = maxwell3d->regs;
    const bool vertex_only = !regs.IsShaderConfigEnabled(Maxwell::ShaderType::Geometry) &&
                             !regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) &&
                             !regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation);
    const auto* fragment = pipeline.StageInfo(4);
    bool has_primitive_outputs = false;
    for (size_t stage : {size_t{0}, size_t{1}}) {
        if (const auto* vertex = pipeline.StageInfo(stage))
            has_primitive_outputs |= vertex->stores[Shader::IR::Attribute::Layer] ||
                                     vertex->stores[Shader::IR::Attribute::ViewportIndex];
    }
    bool has_flat_inputs = false;
    if (fragment) {
        for (size_t i = 0; i < fragment->interpolation.size(); ++i)
            has_flat_inputs |= fragment->loads.Generic(i) &&
                               fragment->interpolation[i] == Shader::Interpolation::Flat;
    }
    const auto provoking = vertex_only && (has_flat_inputs || has_primitive_outputs)
                               ? (regs.provoking_vertex == Maxwell::ProvokingVertex::Last
                                      ? IndexConversion::Provoking::Last
                                      : IndexConversion::Provoking::First)
                               : IndexConversion::Provoking::Preserve;
    const auto format = state.inline_index_draw_indexes.empty()
                            ? static_cast<IndexConversion::Format>(state.index_buffer.format)
                            : IndexConversion::Format::Uint32;
    const IndexConversion::Restart restart{
        regs.primitive_restart.enabled != 0 &&
            IndexConversion::RestartCanMatch(format, regs.primitive_restart.index),
        regs.primitive_restart.index};
    // Reasons may overlap; count only real conversions, not cache hits.
    const auto count_conversion = [&] {
        context.Profile().Count(FrameCount::IndexConversions);
        if (is_indexed && format == IndexConversion::Format::Uint8)
            context.Profile().Count(FrameCount::IndexConversionUint8);
        if (is_indexed && restart.enabled)
            context.Profile().Count(FrameCount::IndexConversionRestart);
        if (topology != IndexConversion::Topology::Points &&
            topology != IndexConversion::Topology::Lines &&
            topology != IndexConversion::Topology::Triangles &&
            topology != IndexConversion::LinesAdjacency &&
            topology != IndexConversion::TrianglesAdjacency)
            context.Profile().Count(FrameCount::IndexConversionStripOrFan);
        if (provoking != IndexConversion::Provoking::Preserve &&
            topology != IndexConversion::Topology::Points)
            context.Profile().Count(FrameCount::IndexConversionProvoking);
        if (is_indexed && !state.inline_index_draw_indexes.empty())
            context.Profile().Count(FrameCount::IndexConversionInline);
    };
    if (is_indexed &&
        static_cast<u32>(topology) <= static_cast<u32>(IndexConversion::TriangleStripAdjacency) &&
        (IndexConversion::NeedsConversion(topology, format, restart, provoking) ||
         !state.inline_index_draw_indexes.empty())) {
        // PrepareDraw already drained inline invalidations before acquiring the cache lock.
        if (static_cast<u32>(format) > 2) {
            SkipDraw("invalid index_buffer.format");
            return false;
        }
        const size_t width = size_t{1} << static_cast<u32>(format);
        const size_t input_size = size_t{state.index_buffer.count} * width;
        if (input_size > 64 * 1024 * 1024) {
            SkipDraw("index conversion exceeds 64 MiB draw budget");
            return false;
        }
        const bool inline_indices = !state.inline_index_draw_indexes.empty();
        const auto address =
            state.index_buffer.StartAddress() + size_t{state.index_buffer.first} * width;
        const IndexConversion::CacheKey key{state.index_buffer.StartAddress(),
                                            0,
                                            state.index_buffer.count,
                                            state.index_buffer.first,
                                            static_cast<u32>(format),
                                            static_cast<u32>(topology),
                                            restart.index,
                                            static_cast<u32>(provoking),
                                            restart.enabled};
        // Storage/stream-output buffers can share the guest index resource after cache
        // merging. Keep the independent converted snapshot when a write state may alias it.
        static const bool no_native_index = BisectEnabled("NXBOX_D3D12_NO_NATIVE_INDEX");
        bool allow_native = regs.transform_feedback_enabled == 0 && !no_native_index;
        for (size_t stage = 0; stage < NUM_GRAPHICS_STAGES && allow_native; ++stage) {
            if (const auto* info = pipeline.StageInfo(stage))
                allow_native = info->storage_buffers_descriptors.empty() &&
                               info->image_buffer_descriptors.empty();
        }
        auto validation_key = key;
        ResourcePtr source;
        const u8* host{};
        bool validated_hit{};
        if (!inline_indices && input_size) {
            const FrameProfile::Scope profile_coherence{context.Profile(),
                                                        FramePhase::IndexCoherence};
            host = gpu_memory->GetSpan(address, input_size);
            if (host) {
                // ReadBlock also publishes overlapping image/query writes. Preserve that
                // coherence before trusting a buffer generation or reading a raw span.
                const auto cpu = gpu_memory->GpuToCpuAddress(address);
                validation_key.device_address = cpu.value_or(0);
                bool external_write{};
                if (cpu) {
                    const FrameProfile::Scope profile_gpu_modified{context.Profile(),
                                                                   FramePhase::IndexGpuModified};
                    external_write = MustFlushRegion(*cpu, input_size,
                                                     VideoCommon::CacheType::TextureCache |
                                                         VideoCommon::CacheType::QueryCache);
                }
                if (external_write) {
                    const FrameProfile::Scope profile_download{context.Profile(),
                                                               FramePhase::BufferDownload};
                    context.Profile().Count(FrameCount::IndexDownloads);
                    context.Profile().Count(FrameCount::IndexDownloadBytes, input_size);
                    FlushRegion(*cpu, input_size, VideoCommon::CacheType::All);
                    buffer_cache.WriteMemory(*cpu, input_size);
                }

                // Obtain the current identity first: creation/replacement also makes its
                // range CPU-dirty. Clean hits need no upload-range enumeration.
                const auto [buffer, offset] =
                    buffer_cache.ObtainBuffer(address, static_cast<u32>(input_size),
                                              VideoCommon::ObtainBufferSynchronize::NoSynchronize,
                                              VideoCommon::ObtainBufferOperation::DoNothing);
                (void)offset;
                source = buffer->Handle();
                if (cpu && buffer_cache.IsRegionCpuModified(*cpu, input_size)) {
                    buffer_runtime.synchronizing_indices = true;
                    SCOPE_EXIT {
                        buffer_runtime.synchronizing_indices = false;
                    };
                    // Consume CPU dirtiness and advance the resource generation before lookup.
                    const auto [synchronized, synchronized_offset] = buffer_cache.ObtainBuffer(
                        address, static_cast<u32>(input_size),
                        VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                        VideoCommon::ObtainBufferOperation::DoNothing);
                    (void)synchronized_offset;
                    source = synchronized->Handle();
                }
                // GPU UAV/copy uses advance the generation even for same-state writes.
                if (const auto found = index_converter.FindValidated(validation_key, source);
                    found && !BisectEnabled("NXBOX_D3D12_NO_INDEX_VALIDATED") &&
                    (!found->native || allow_native)) {
                    converted_indices = *found;
                    validated_hit = true;
                    context.Profile().Count(FrameCount::IndexConversionCacheHits);
                    context.Profile().Count(FrameCount::IndexValidatedCacheHits);
                }
            }
        }
        if (!validated_hit) {
            std::span<const std::byte> input;
            if (inline_indices) {
                if (input_size > state.inline_index_draw_indexes.size()) {
                    SkipDraw("inline_index_draw_indexes truncated");
                    return false;
                }
                input = {reinterpret_cast<const std::byte*>(state.inline_index_draw_indexes.data()),
                         input_size};
            } else {
                {
                    const FrameProfile::Scope profile_coherence{context.Profile(),
                                                                FramePhase::IndexGpuModified};
                    if (const auto cpu = gpu_memory->GpuToCpuAddress(address);
                        cpu && buffer_cache.IsRegionGpuModified(*cpu, input_size)) {
                        const FrameProfile::Scope profile_download{context.Profile(),
                                                                   FramePhase::BufferDownload};
                        context.Profile().Count(FrameCount::IndexDownloads);
                        context.Profile().Count(FrameCount::IndexDownloadBytes, input_size);
                        buffer_cache.DownloadMemory(*cpu, input_size);
                    }
                }
                const FrameProfile::Scope profile_read{context.Profile(), FramePhase::IndexRead};
                context.Profile().Count(FrameCount::IndexReadBytes, input_size);
                if (host) {
                    input = {reinterpret_cast<const std::byte*>(host), input_size};
                } else {
                    index_read_scratch.resize(input_size);
                    gpu_memory->ReadBlock(address, index_read_scratch.data(), input_size);
                    input = index_read_scratch;
                }
            }
            bool native{};
            if (!inline_indices && allow_native &&
                !IndexConversion::NeedsConversion(topology, format, {}, provoking)) {
                const FrameProfile::Scope profile_scan{context.Profile(),
                                                       FramePhase::IndexRestartScan};
                native = !IndexConversion::ContainsRestart(format, input, restart);
            }
            if (native) {
                converted_indices = ConvertedIndexBuffer{};
                converted_indices->native = true;
            } else if (!inline_indices) {
                bool cache_hit{};
                converted_indices = index_converter.ConvertCached(key, input, cache_hit);
                if (cache_hit) {
                    context.Profile().Count(FrameCount::IndexConversionCacheHits);
                } else {
                    count_conversion();
                    context.Profile().Count(FrameCount::IndexConversionBytes, input.size());
                }
            } else {
                count_conversion();
                context.Profile().Count(FrameCount::IndexConversionBytes, input.size());
                converted_indices =
                    index_converter.Convert(topology, format, input, restart, provoking);
            }
            index_converter.RememberValidated(validation_key, source, *converted_indices);
        }
        if (converted_indices->native) {
            context.Profile().Count(FrameCount::IndexNativeRestartDraws);
            converted_indices.reset(); // Bind through the normal index buffer cache below.
        }
    } else if (!is_indexed &&
               (topology == IndexConversion::Topology::Quads ||
                topology == IndexConversion::Topology::QuadStrip ||
                topology == IndexConversion::Topology::TriangleFan ||
                topology == IndexConversion::Topology::Polygon ||
                topology == IndexConversion::Topology::LineLoop ||
                (vertex_only && static_cast<u32>(topology) <= 9)) &&
               IndexConversion::NeedsConversion(topology, IndexConversion::Format::Uint32, {},
                                                provoking)) {
        count_conversion();
        converted_indices =
            index_converter.Generate(topology, state.vertex_buffer.count, 0, provoking);
    }
    if (converted_indices && !converted_indices->count) {
        converted_indices.reset();
        return false;
    }
    if (is_indexed && !converted_indices)
        context.Profile().Count(FrameCount::NativeIndexDraws);
    if (converted_indices) {
        params.num_vertices = converted_indices->count;
        params.first_index = 0;
        params.base_vertex = is_indexed ? state.base_index : state.vertex_buffer.first;
        params.is_indexed = true;
    }
    return true;
}

void RasterizerD3D12::TickWork() {
    // Sync callbacks can wait for decoder progress or dispatch more renderer work.
    // The consumer must finish its job before the producer services those requests.
    if (!gpu_pipeline || !gpu_pipeline->IsConsumer())
        m_gpu.TickWork();
}

void RasterizerD3D12::DrainGpuPipeline() {
    if (gpu_pipeline)
        gpu_pipeline->Drain();
}

void RasterizerD3D12::Draw(bool is_indexed, u32 instance_count) {
    if (OnGpuPipeline()) {
        auto snapshot = VideoCommon::CaptureDrawSnapshot(*frontend_maxwell);
        gpu_pipeline->Push([this, snapshot = std::move(snapshot), is_indexed, instance_count] {
            snapshot.Apply(*maxwell3d);
            DrawSynchronous(is_indexed, instance_count);
        });
        // Guest sync requests belong to the producer. A request that accesses
        // memory drains the queue through the rasterizer/memory boundary.
        TickWork();
        return;
    }
    DrawSynchronous(is_indexed, instance_count);
    // Indirect expansion runs on the consumer; its caller ticks on the producer
    // after the entire synchronous operation has returned.
    if (gpu_pipeline)
        TickWork();
}

void RasterizerD3D12::DrawSynchronous(bool is_indexed, u32 instance_count) try {
    const FrameProfile::Scope profile_dispatch{context.Profile(), FramePhase::DrawDispatch};
    static const bool no_draws = BisectEnabled("NXBOX_D3D12_NO_DRAWS");
    if (no_draws) {
        if (!gpu_pipeline)
            TickWork();
        return;
    }
    converted_indices.reset();
    query_runtime.PauseHostConditionalRendering();
    DrawParams params =
        MakeDrawParams(maxwell3d->draw_manager.draw_state, instance_count, is_indexed);
    PrepareDraw(is_indexed, DrawCommand{params});
    converted_indices.reset();
    if (!gpu_pipeline)
        TickWork();
} catch (const std::exception& error) {
    const FrameProfile::Scope profile_failure{context.Profile(), FramePhase::DrawCleanup};
    converted_indices.reset();
    SkipDraw(error.what());
    if (!gpu_pipeline)
        TickWork();
} catch (...) {
    const FrameProfile::Scope profile_failure{context.Profile(), FramePhase::DrawCleanup};
    converted_indices.reset();
    SkipDraw("unknown exception in direct draw");
    if (!gpu_pipeline)
        TickWork();
}

ID3D12CommandSignature* RasterizerD3D12::IndirectSignature(bool indexed) {
    auto& signature = indexed ? indexed_signature : draw_signature;
    if (!signature) {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type =
            indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride =
            indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &argument;
        Check(context.Device()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)),
              "CreateCommandSignature", context.Device());
    }
    return signature.Get();
}

void RasterizerD3D12::DrawIndirect() try {
    if (OnGpuPipeline()) {
        RunGpuPipeline([&] { return DrawIndirect(); });
        TickWork();
        return;
    }

    const FrameProfile::Scope profile_dispatch{context.Profile(), FramePhase::DrawDispatch};
    static const bool no_draws = BisectEnabled("NXBOX_D3D12_NO_DRAWS");
    if (no_draws) {
        TickWork();
        return;
    }
    query_runtime.PauseHostConditionalRendering();
    const auto params = maxwell3d->draw_manager.indirect_state;
    if (params.is_byte_count) {
        SkipDraw("draw_auto_stride byte-count draw requires guest transform feedback linkage");
        TickWork();
        return;
    }
    const size_t command_size =
        params.is_indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
    const size_t stride = params.stride ? params.stride : command_size;
    gpu_memory->FlushCaching();
    // Download GPU-written arguments/counts before CPU expansion. Reuse Draw so
    // every command gets its own base-ID constants, restart and topology conversion.
    const auto synchronize = [&](GPUVAddr address, size_t size) {
        if (const auto cpu = gpu_memory->GpuToCpuAddress(address)) {
            std::scoped_lock lock{buffer_cache.mutex};
            buffer_cache.DownloadMemory(*cpu, size);
        }
    };
    u32 guest_count = UINT32_MAX;
    if (params.include_count) {
        synchronize(params.count_start_address, sizeof(guest_count));
        guest_count = gpu_memory->Read<u32>(params.count_start_address);
    }
    const u64 count = Pso::IndirectCount(params.buffer_size, stride, command_size,
                                         params.max_draw_counts, guest_count);
    VideoCommon::SubmissionTrace::Log(
        "host_indirect args_gpu={:x} count_gpu={:x} guest_count={} expanded_count={}",
        params.indirect_start_address, params.count_start_address, guest_count, count);
    if (!count) {
        TickWork();
        return;
    }
    const size_t bytes = static_cast<size_t>((count - 1) * stride + command_size);
    if (params.indirect_start_address > UINT64_MAX - bytes) {
        SkipDraw("indirect_start_address range overflow");
        return;
    }
    synchronize(params.indirect_start_address, bytes);
    std::vector<u8> arguments(bytes);
    gpu_memory->ReadBlock(params.indirect_start_address, arguments.data(), arguments.size());
    auto& state = maxwell3d->draw_manager.draw_state;
    const auto saved_state = state;
    const auto saved_draw_id = indirect_draw_id;
    SCOPE_EXIT {
        state = saved_state;
        indirect_draw_id = saved_draw_id;
    };
    for (u64 i = 0; i < count; ++i) {
        indirect_draw_id = static_cast<u32>(i);
        const auto* data = arguments.data() + static_cast<size_t>(i) * stride;
        if (params.is_indexed) {
            D3D12_DRAW_INDEXED_ARGUMENTS command;
            std::memcpy(&command, data, sizeof(command));
            state.index_buffer.count = command.IndexCountPerInstance;
            state.index_buffer.first = command.StartIndexLocation;
            state.base_index = static_cast<u32>(command.BaseVertexLocation);
            state.base_instance = command.StartInstanceLocation;
            Draw(true, command.InstanceCount);
        } else {
            D3D12_DRAW_ARGUMENTS command;
            std::memcpy(&command, data, sizeof(command));
            state.vertex_buffer.count = command.VertexCountPerInstance;
            state.vertex_buffer.first = command.StartVertexLocation;
            state.base_instance = command.StartInstanceLocation;
            Draw(false, command.InstanceCount);
        }
    }
    TickWork();
} catch (const std::exception& error) {
    if (OnGpuPipeline())
        throw;
    const FrameProfile::Scope profile_failure{context.Profile(), FramePhase::DrawCleanup};
    SkipDraw(error.what());
    TickWork();
} catch (...) {
    if (OnGpuPipeline())
        throw;
    const FrameProfile::Scope profile_failure{context.Profile(), FramePhase::DrawCleanup};
    SkipDraw("unknown exception in indirect draw");
    TickWork();
}

bool RasterizerD3D12::AccelerateConditionalRendering() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return AccelerateConditionalRendering(); });

    gpu_memory->FlushCaching();
    return query_cache.AccelerateHostConditionalRendering();
}
void RasterizerD3D12::PreparePresentation() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return PreparePresentation(); });

    query_runtime.PauseHostConditionalRendering();
}
void RasterizerD3D12::DrawTexture() {
    if (OnGpuPipeline()) {
        RunGpuPipeline([&] { return DrawTexture(); });
        TickWork();
        return;
    }

    const auto execute_blit = [this] {
        ++feedback_draw;
        ++feedback_epoch;
        gpu_memory->FlushCaching();
        query_runtime.PauseHostConditionalRendering();
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.SynchronizeDescriptors(false);
        texture_cache.UpdateRenderTargets(false);
        const auto* current = texture_cache.GetFramebuffer();
        if (!current)
            return;
        Framebuffer framebuffer = *current;
        auto* target = framebuffer.Color(framebuffer.DrawBuffer(0));
        if (!target || !target->ImageHandle())
            return;
        if (!(target->ImageHandle()->handle->GetDesc().Flags &
              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
            SkipDraw("DrawTexture target lacks ALLOW_RENDER_TARGET");
            return;
        }
        query_runtime.PrepareDraw();
        const auto& blit = maxwell3d->draw_manager.draw_texture_state;
        // Descriptor discovery may relocate ImageViews; own the source snapshot.
        const ImageView source = texture_cache.GetImageView(blit.src_texture);
        const auto* sampler = texture_cache.GetSampler(blit.src_sampler, false);
        auto sampled = source.ImageHandle();
        auto description = source.Description();
        if (!sampled || !sampler)
            return;
        if (sampled->handle->GetDesc().SampleDesc.Count != 1 ||
            (description.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D &&
             description.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY) ||
            VideoCore::Surface::IsPixelFormatInteger(source.format) ||
            VideoCore::Surface::IsPixelFormatInteger(target->format)) {
            SkipDraw("draw_texture_state requires single-sample float/normalized 2D views");
            return;
        }
        texture_cache.PrepareImage(source.image_id, false, false);
        if (sampled == target->ImageHandle()) {
            sampled = CopyFeedback(sampled, &source.range);
            if (!sampled) {
                SkipDraw("DrawTexture feedback copy budget");
                return;
            }
        }
        if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            const auto mip = description.Texture2D.MostDetailedMip;
            const auto levels = description.Texture2D.MipLevels;
            const auto plane = description.Texture2D.PlaneSlice;
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Texture2DArray = {mip, levels, 0, 1, plane, 0};
        }
        Pso::GraphicsPsoState state{};
        FillGraphicsState(*maxwell3d, state);
        state.attributes.fill({});
        state.cull_mode = Pso::kCullNone;
        state.cull_all = 0;
        state.fill_mode = 3; // D3D12_FILL_MODE_SOLID
        state.topology_type = Pso::kTopologyTypeTriangle;
        state.depth_enable = state.depth_write = state.stencil_enable = state.depth_bounds = 0;
        state.logic_enable = state.alpha_to_coverage = state.conservative_raster = 0;
        state.depth_bias = 0;
        state.bias_clamp_bits = state.slope_scaled_bias_bits = 0;
        for (auto& blend : state.blend) {
            blend.enable = 0;
            blend.write_mask = 15;
        }
        Pso::FramebufferFormats formats{};
        formats.colors[0] = static_cast<u32>(target->Description().Format);
        formats.sample_count =
            static_cast<u8>(target->ImageHandle()->handle->GetDesc().SampleDesc.Count);
        Pso::ApplyFramebufferFormats(state, formats);
        const std::array<StageInput, 2> stages{{
            {Shader::Stage::VertexB, 0, Shader::Backend::HLSL::DRAW_TEXTURE_VERTEX, {}},
            {Shader::Stage::Fragment,
             0,
             Shader::Backend::HLSL::DRAW_TEXTURE_FRAGMENT,
             {0, 1, 0, 1}},
        }};
        auto& pipeline = default_pipeline_cache->GetGraphics(
            state, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, stages);
        descriptors.Reserve(3, 1, 1);
        const auto tables = descriptors.AllocateViews(3);
        const auto sampler_table = descriptors.AllocateSamplers(1);
        const auto rtv = descriptors.AllocateRtv(1);
        descriptors.WriteNullCbv(tables, 0);
        context.ValidateViewFormat(sampled, description.Format, "CreateShaderResourceView");
        context.Device()->CreateShaderResourceView(sampled->handle.Get(), &description,
                                                   tables.Slot(1));
        TraceApiCall(context.Device(), "CreateShaderResourceView(copy)", sampled->handle.Get(),
                     &description, sizeof(description));
        descriptors.WriteNullUav(tables, 2);
        descriptors.WriteSampler(sampler_table, 0, *sampler, false);
        descriptors.CreateRtv(rtv, target);
        context.Transition(sampled, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context.Transition(target->ImageHandle(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        context.Retain(sampled);
        context.Retain(target->ImageHandle());
        context.InvalidateRecordedState();
        auto* commands = context.Commands();
        descriptors.SetHeaps(commands);
        commands->SetGraphicsRootSignature(pipeline.RootSignature());
        commands->SetPipelineState(pipeline.PipelineState());
        commands->SetGraphicsRootDescriptorTable(1, tables.gpu);
        commands->SetGraphicsRootDescriptorTable(2, tables.Slice(1, 1).gpu);
        commands->SetGraphicsRootDescriptorTable(3, tables.Slice(2, 1).gpu);
        commands->SetGraphicsRootDescriptorTable(4, sampler_table.gpu);
        const auto size = framebuffer.Size();
        const std::array<float, 12> constants{
            blit.dst_x0,
            blit.dst_y0,
            blit.dst_x1,
            blit.dst_y1,
            blit.src_x0,
            blit.src_y0,
            blit.src_x1,
            blit.src_y1,
            static_cast<float>(texture_cache.IsRescaling()
                                   ? static_cast<u32>(maxwell3d->regs.surface_clip.width)
                                   : size.width),
            static_cast<float>(texture_cache.IsRescaling()
                                   ? static_cast<u32>(maxwell3d->regs.surface_clip.height)
                                   : size.height),
            static_cast<float>(source.size.width),
            static_cast<float>(source.size.height)};
        if (!size.width || !size.height || !source.size.width || !source.size.height)
            return;
        commands->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()),
                                                constants.data(), 0);
        commands->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
        const D3D12_VIEWPORT viewport{
            0, 0, static_cast<float>(size.width), static_cast<float>(size.height), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(size.width),
                                 static_cast<LONG>(size.height)};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        transform_feedback.Unbind();
        query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                                  maxwell3d->regs.zpass_pixel_count_enable != 0);
        query_runtime.BeginDraw(false);
        context.MarkWork("DrawTexture blit");
        if (auto* profile = context.PassProfile())
            profile->Target({size.width, size.height, target->HostFormat().view_format, 0, 0});
        VideoCommon::SubmissionTrace::Log("host_draw_texture src_gpu={:x} dst_gpu={:x}",
                                          source.GpuAddr(), target->GpuAddr());
        commands->DrawInstanced(4, 1, 0, 0);
        if (pipeline_cache)
            pipeline_cache->RecordGraphicsDraw(texture_cache.CaptureFrame());
        query_runtime.EndDraw();
        query_runtime.PauseHostConditionalRendering();
        texture_cache.RecordPassDumpRenderTargetWrite(framebuffer.DrawBuffer(0), "draw_texture",
                                                      source.GpuAddr());
    };
    try {
        execute_blit();
    } catch (const std::exception& error) {
        if (OnGpuPipeline())
            throw;
        SkipDraw(error.what());
    } catch (...) {
        if (OnGpuPipeline())
            throw;
        SkipDraw("unknown exception in DrawTexture");
    }
    // Tick callbacks may throw. Let the GPU-thread boundary cancel their waiters;
    // executing them in a noexcept scope-exit destructor could terminate instead.
    TickWork();
}
void RasterizerD3D12::Clear(u32 layer_count) try {
    if (OnGpuPipeline()) {
        RunGpuPipeline([&] { return Clear(layer_count); });
        TickWork();
        return;
    }

    ++feedback_epoch;
    transform_feedback.Unbind();
    gpu_memory->FlushCaching();
    query_runtime.PauseHostConditionalRendering();
    std::scoped_lock lock{texture_cache.mutex};
    const auto& regs = maxwell3d->regs;
    u32 control{};
    std::memcpy(&control, &regs.clear_control, sizeof(control));
    u32 surface{};
    std::memcpy(&surface, &regs.clear_surface, sizeof(surface));
    const auto selection = ClearOperations::Decode(surface, control, regs.stencil_front_mask);
    texture_cache.BeginPassDumpImageWrite();
    texture_cache.UpdateRenderTargets(true);
    auto* framebuffer = texture_cache.GetFramebuffer();
    if (!framebuffer)
        return;
    if (selection.target >= VideoCommon::NUM_RT)
        return;
    auto* color = framebuffer->Color(selection.target);
    auto* depth = framebuffer->Depth();
    ClearValues values;
    values.color = regs.clear_color;
    values.depth = regs.clear_depth;
    values.stencil = static_cast<u8>(regs.clear_stencil);
    if (depth && (selection.depth || selection.stencil) &&
        BisectEnabled("NXBOX_D3D12_DEPTH_TRACE") && depth_trace_clears++ < 4) {
        Diagnostic(fmt::format("D3D12_DEPTH_CLEAR frame={} depth={} stencil={} clear_depth={} "
                               "clear_stencil={} extent={}x{}",
                               feedback_frame, selection.depth, selection.stencil, values.depth,
                               values.stencil, depth->size.width, depth->size.height));
    }
    if (color) {
        const auto pixel = color->format;
        if (VideoCore::Surface::IsPixelFormatInteger(pixel)) {
            values.color_type = VideoCore::Surface::IsPixelFormatSignedInteger(pixel)
                                    ? ClearColorType::Sint
                                    : ClearColorType::Uint;
            for (size_t i = 0; i < 4; ++i) {
                values.integer_color[i] = std::bit_cast<u32>(regs.clear_color[i]);
            }
        }
    }
    const auto size = framebuffer->Size();
    const auto& resolution = Settings::values.resolution_info;
    const ResolutionScale scale = texture_cache.IsRescaling()
                                      ? ResolutionScale{resolution.up_scale, resolution.down_shift}
                                      : ResolutionScale{};
    const u32 guest_width = maxwell3d->regs.surface_clip.width;
    const u32 guest_height = maxwell3d->regs.surface_clip.height;
    ClearOperations::Rect clip{0, 0, static_cast<s32>(guest_width), static_cast<s32>(guest_height)};
    if (regs.clear_control.use_scissor && regs.scissor_test[0].enable) {
        const auto& scissor = regs.scissor_test[0];
        clip = ClearOperations::Intersect(
            clip, {static_cast<s32>(scissor.min_x), static_cast<s32>(scissor.min_y),
                   static_cast<s32>(scissor.max_x), static_cast<s32>(scissor.max_y)});
    }
    if (regs.clear_control.use_clear_rect) {
        clip = ClearOperations::Intersect(clip, {static_cast<s32>(regs.clear_rect.x_min),
                                                 static_cast<s32>(regs.clear_rect.y_min),
                                                 static_cast<s32>(regs.clear_rect.x_max),
                                                 static_cast<s32>(regs.clear_rect.y_max)});
    }
    if (regs.clear_control.use_viewport_clip0) {
        const auto& viewport = regs.viewports[0];
        clip = ClearOperations::Intersect(clip, {static_cast<s32>(viewport.x),
                                                 static_cast<s32>(viewport.y),
                                                 static_cast<s32>(viewport.x + viewport.width),
                                                 static_cast<s32>(viewport.y + viewport.height)});
    }
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
        const auto top = clip.top;
        clip.top = static_cast<s32>(guest_height) - clip.bottom;
        clip.bottom = static_cast<s32>(guest_height) - top;
    }
    const auto scaled_clip = scale.Rect(clip.left, clip.top, clip.right, clip.bottom);
    clip = ClearOperations::Intersect(
        {scaled_clip.left, scaled_clip.top, scaled_clip.right, scaled_clip.bottom},
        {0, 0, static_cast<s32>(size.width), static_cast<s32>(size.height)});
    for (u32 layer = selection.layer; layer < selection.layer + layer_count; ++layer) {
        ClearAttachment color_attachment, depth_attachment;
        descriptors.Reserve(0, 0, selection.color_mask ? 1u : 0u,
                            (selection.depth || selection.stencil) ? 1u : 0u);
        const auto prepare = [&](ImageView* view, bool is_depth, ClearAttachment& attachment) {
            if (!view || !view->ImageHandle())
                return false;
            const auto flags = view->ImageHandle()->handle->GetDesc().Flags;
            const auto required = is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                           : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (!(flags & required)) {
                view->LogUnsupported(is_depth ? "clear attachment lacks ALLOW_DEPTH_STENCIL"
                                              : "clear attachment lacks ALLOW_RENDER_TARGET");
                SkipDraw("unsupported clear attachment flags");
                return false;
            }
            const auto& srv = view->Description();
            const bool msaa = view->ImageHandle()->handle->GetDesc().SampleDesc.Count > 1;
            const bool msaa_array = srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            const u32 first = msaa_array ? srv.Texture2DMSArray.FirstArraySlice
                              : srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                  ? srv.Texture2DArray.FirstArraySlice
                                  : 0;
            const u32 layers = msaa_array ? srv.Texture2DMSArray.ArraySize
                               : srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                   ? srv.Texture2DArray.ArraySize
                                   : 1;
            const u32 mip = msaa ? 0
                            : srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                ? srv.Texture2DArray.MostDetailedMip
                                : srv.Texture2D.MostDetailedMip;
            // A clear view the resource cannot back leaves the RTV/DSV slot unwritten in a release
            // runtime, and the Xbox driver then reads a null pointer inside ClearRenderTargetView
            // (Super Mario Odyssey on Series S). Volume targets clear one depth slice.
            const auto resource_desc = view->ImageHandle()->handle->GetDesc();
            const bool volume = resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            const u32 slices =
                volume ? std::max<u32>(1, resource_desc.DepthOrArraySize >> mip) : layers;
            if (layer >= slices)
                return false;
            const char* invalid = nullptr;
            if (resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
                invalid = "clear attachment is a 1D texture";
            else if (mip >= resource_desc.MipLevels ||
                     (!volume && first + layer >= resource_desc.DepthOrArraySize))
                invalid = "clear attachment subresource is out of range";
            else if (!SupportsFormat(context.Device(),
                                     is_depth ? DsvFormat(srv.Format) : srv.Format,
                                     is_depth ? D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL
                                              : D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
                invalid = is_depth ? "clear attachment format cannot be a depth target"
                                   : "clear attachment format cannot be a render target";
            if (invalid) {
                view->LogUnsupported(invalid);
                SkipDraw(invalid);
                return false;
            }
            attachment.resource = view->ImageHandle();
            attachment.width =
                static_cast<u32>(attachment.resource->handle->GetDesc().Width >> mip);
            attachment.height = attachment.resource->handle->GetDesc().Height >> mip;
            attachment.samples = attachment.resource->handle->GetDesc().SampleDesc;
            if (is_depth) {
                attachment.descriptor = descriptors.AllocateDsv();
                attachment.format = DsvFormat(srv.Format);
                attachment.has_depth = true;
                attachment.has_stencil = attachment.format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                                         attachment.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
                desc.Format = attachment.format;
                if (msaa) {
                    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                    desc.Texture2DMSArray = {first + layer, 1};
                } else {
                    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                    desc.Texture2DArray = {mip, first + layer, 1};
                }
                context.ValidateViewFormat(attachment.resource, desc.Format,
                                           "CreateDepthStencilView");
                context.Device()->CreateDepthStencilView(attachment.resource->handle.Get(), &desc,
                                                         attachment.descriptor);
                TraceApiCall(context.Device(), "CreateDepthStencilView(layer)",
                             attachment.resource->handle.Get(), &desc, sizeof(desc));
            } else {
                attachment.descriptor = descriptors.AllocateRtv(1);
                attachment.format = srv.Format;
                D3D12_RENDER_TARGET_VIEW_DESC desc{};
                desc.Format = srv.Format;
                if (msaa) {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    desc.Texture2DMSArray = {first + layer, 1};
                } else if (volume) {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                    desc.Texture3D = {mip, layer, 1};
                } else {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    desc.Texture2DArray = {mip, first + layer, 1, 0};
                }
                context.ValidateViewFormat(attachment.resource, desc.Format,
                                           "CreateRenderTargetView");
                context.Device()->CreateRenderTargetView(attachment.resource->handle.Get(), &desc,
                                                         attachment.descriptor);
                TraceApiCall(context.Device(), "CreateRenderTargetView(layer)",
                             attachment.resource->handle.Get(), &desc, sizeof(desc));
            }
            return true;
        };
        const bool has_color = selection.color_mask && prepare(color, false, color_attachment);
        const bool has_depth =
            (selection.depth || selection.stencil) && prepare(depth, true, depth_attachment);
        VideoCommon::SubmissionTrace::Log(
            "host_clear color_gpu={:x} depth_gpu={:x} color={} depth={} stencil={} "
            "layer={} rect={},{},{},{} value={},{},{},{}",
            has_color ? color->GpuAddr() : 0, has_depth ? depth->GpuAddr() : 0,
            has_color ? selection.color_mask : 0, selection.depth, selection.stencil, layer,
            clip.left, clip.top, clip.right, clip.bottom, values.color[0], values.color[1],
            values.color[2], values.color[3]);
        clear_renderer.Execute(selection, values, has_color ? &color_attachment : nullptr,
                               has_depth ? &depth_attachment : nullptr, &clip);
        if (has_color)
            texture_cache.RecordPassDumpRenderTargetWrite(selection.target, "clear", 0);
        if (has_depth)
            texture_cache.RecordPassDumpDepthWrite("clear_depth");
    }
    TickWork();
} catch (const std::exception& error) {
    if (OnGpuPipeline())
        throw;
    SkipDraw(error.what());
    TickWork();
} catch (...) {
    if (OnGpuPipeline())
        throw;
    SkipDraw("unknown exception in clear");
    TickWork();
}
void RasterizerD3D12::DispatchCompute() try {
    if (OnGpuPipeline()) {
        RunGpuPipeline([&] { return DispatchCompute(); });
        TickWork();
        return;
    }

    bool trace_submitted = false;
    VideoCommon::SubmissionTrace::Log("host_compute_begin");
    SCOPE_EXIT {
        VideoCommon::SubmissionTrace::Log("host_compute_end submitted={}", trace_submitted);
    };
    const Common::GpuProfile::PhaseScope gpu_profile_scope{
        Common::GpuProfile::Phase::ComputeDispatchSetup};
    if (BisectEnabled("NXBOX_D3D12_NO_COMPUTE")) {
        TickWork();
        return;
    }
    gpu_memory->FlushCaching();
    query_runtime.PauseHostConditionalRendering();
    auto* pipeline = default_pipeline_cache->CurrentComputePipeline();
    if (!pipeline) {
        VideoCommon::SubmissionTrace::Log("host_compute_drop reason=pending_or_failed_pso");
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto& info = pipeline->info;
    const auto& qmd = kepler_compute->launch_description;
    texture_cache.SynchronizeDescriptors(true);
    bindings.current_stage = 0;
    bindings.stages[0].Clear();
    std::copy_n(info.constant_buffer_used_sizes.begin(), compute_uniform_sizes.size(),
                compute_uniform_sizes.begin());
    buffer_cache.SetComputeUniformBufferState(info.constant_buffer_mask, &compute_uniform_sizes);
    buffer_cache.UnbindComputeStorageBuffers();
    size_t storage_index = 0;
    for (const auto& desc : info.storage_buffers_descriptors)
        buffer_cache.BindComputeStorageBuffer(storage_index++, desc.cbuf_index, desc.cbuf_offset,
                                              desc.is_written);
    view_scratch.clear();
    sampler_scratch.clear();
    const auto handle = [&](const auto& desc, u32 index) {
        const u32 offset = index << desc.size_shift;
        u32 value = gpu_memory->Read<u32>(qmd.const_buffer_config[desc.cbuf_index].Address() +
                                          desc.cbuf_offset + offset);
        if constexpr (requires { desc.has_secondary; }) {
            value <<= desc.shift_left;
            if (desc.has_secondary)
                value |= gpu_memory->Read<u32>(
                             qmd.const_buffer_config[desc.secondary_cbuf_index].Address() +
                             desc.secondary_cbuf_offset + offset)
                         << desc.secondary_shift_left;
        }
        return Tegra::Texture::TexturePair(value, qmd.linked_tsc != 0);
    };
    const auto gather = [&](const auto& descs, bool written) {
        for (const auto& desc : descs)
            for (u32 i = 0; i < desc.count; ++i) {
                std::optional<VideoCore::Surface::PixelFormat> format;
                if constexpr (requires { desc.format; })
                    format = PixelFormatFromImageFormat(desc.format);
                view_scratch.push_back({.index = handle(desc, i).first,
                                        .blacklist = written,
                                        .id = {},
                                        .format_override = format});
            }
    };
    gather(info.texture_buffer_descriptors, false);
    gather(info.image_buffer_descriptors, true);
    gather(info.texture_descriptors, false);
    gather(info.image_descriptors, true);
    for (const auto& desc : info.texture_descriptors)
        for (u32 i = 0; i < desc.count; ++i)
            sampler_scratch.push_back(texture_cache.GetSamplerId(handle(desc, i).second, true));
    texture_cache.FillImageViews(std::span(view_scratch.data(), view_scratch.size()), true,
                                 !info.image_descriptors.empty());
    buffer_cache.UnbindComputeTextureBuffers();
    size_t texel_index = 0, view_index = 0;
    for (const auto& desc : info.texture_buffer_descriptors)
        for (u32 i = 0; i < desc.count; ++i) {
            auto& view = texture_cache.GetImageView(view_scratch[view_index++].id);
            buffer_cache.BindComputeTextureBuffer(texel_index++, view.GpuAddr(), view.BufferSize(),
                                                  view.format, false, false);
        }
    for (const auto& desc : info.image_buffer_descriptors)
        for (u32 i = 0; i < desc.count; ++i) {
            auto& view = texture_cache.GetImageView(view_scratch[view_index++].id);
            const auto format = PixelFormatFromImageFormat(desc.format);
            if (!format && desc.format != Shader::ImageFormat::Typeless)
                throw std::runtime_error("D3D12 unsupported compute image buffer format");
            const auto guest_format = format.value_or(view.format);
            if (const auto problem =
                    StorageImageProblem(context, guest_format, desc.format, desc.is_integer,
                                        desc.is_read, desc.is_written)) {
                SkipDraw(problem);
                return;
            }
            buffer_cache.BindComputeTextureBuffer(texel_index++, view.GpuAddr(), view.BufferSize(),
                                                  guest_format, desc.is_written, true);
        }
    buffer_cache.UpdateComputeBuffers();
    buffer_cache.BindHostComputeBuffers();
    std::optional<FrameProfile::Scope> profile_descriptors;
    profile_descriptors.emplace(context.Profile(), FramePhase::DescriptorAndPipeline);
    const auto counts = CountStageTables(info);
    const bool root_cbvs = pipeline->UsesRootCbvs();
    if (root_cbvs)
        PrepareZeroUniformBuffer();
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> root_cbv_addresses;
    root_cbv_addresses.reserve(counts.cbv);
    const u32 cbv_size = root_cbvs ? 0u : TableSize(counts.cbv);
    const u32 srv_size = TableSize(counts.srv), uav_size = TableSize(counts.uav);
    descriptors.Reserve(cbv_size + srv_size + uav_size, TableSize(counts.sampler));
    const auto allocation = descriptors.AllocateViews(cbv_size + srv_size + uav_size);
    const auto cbv = allocation.Slice(0, cbv_size), srv = allocation.Slice(cbv_size, srv_size),
               uav = allocation.Slice(cbv_size + srv_size, uav_size);
    const auto sampler = descriptors.AllocateSamplers(TableSize(counts.sampler));
    std::vector<ComputeResourceUse> resources;
    const auto use = [&](const ResourcePtr& resource, D3D12_RESOURCE_STATES state) {
        if (resource)
            resources.push_back({resource, state});
    };
    // Texture reads and image writes keep their subresources: a pass that reads one mip of a
    // texture and writes another (a mip chain or a depth pyramid) reads its mips through
    // per-subresource transitions instead of being skipped as a whole-resource conflict.
    using Subresources = std::pair<ResourcePtr, std::vector<u32>>;
    std::vector<Subresources> image_reads, image_writes, split_reads;
    const auto subresources_of = [](const ResourcePtr& resource, const auto& view) {
        Subresources out{resource, {}};
        const auto desc = resource->handle->GetDesc();
        const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const u32 first_layer = volume ? 0u : static_cast<u32>(view.range.base.layer);
        const u32 layers = volume ? 1u : static_cast<u32>(view.range.extent.layers);
        for (u32 layer = first_layer; layer < first_layer + layers; ++layer)
            for (s32 mip = view.range.base.level;
                 mip < view.range.base.level + view.range.extent.levels; ++mip)
                out.second.push_back(static_cast<u32>(mip) + layer * desc.MipLevels);
        return out;
    };
    const auto& bound = bindings.stages[0];
    for (u32 i = 0; i < (root_cbvs ? counts.cbv : cbv_size); ++i) {
        BoundBuffer buffer;
        if (i < bound.uniform.size())
            buffer = bound.uniform[i];
        if (buffer.resource && IsStorageBuffer(bindings, buffer.resource))
            buffer = buffer_runtime.CopyUniform(buffer);
        if (root_cbvs) {
            if (!buffer.resource || !buffer.size)
                buffer = {zero_uniform_buffer, 0, 65536};
            root_cbv_addresses.push_back(buffer.resource->handle->GetGPUVirtualAddress() +
                                         buffer.offset);
        } else {
            descriptors.WriteCbv(cbv, i, buffer);
        }
        use(buffer.resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    u32 srv_index = 0, sampler_index = 0;
    u32 compute_texture_scaling_mask = 0, compute_image_scaling_mask = 0;
    u32 compute_texture_descriptor = 0;
    for (const auto& desc : info.texture_descriptors) {
        const u32 scaling_index = compute_texture_descriptor++;
        for (u32 i = 0; i < desc.count; ++i) {
            auto& view = texture_cache.GetImageView(view_scratch[view_index++].id);
            if (scaling_index < 32 && view.IsRescaled())
                compute_texture_scaling_mask |= 1u << scaling_index;
            descriptors.WriteImageSrv(srv, srv_index++, view,
                                      SrvDimension(desc.type, desc.is_multisample));
            if (const auto& image = view.ImageHandle())
                image_reads.push_back(subresources_of(image, view));
            descriptors.WriteSampler(sampler, sampler_index,
                                     texture_cache.GetSampler(sampler_scratch[sampler_index]),
                                     desc.is_depth);
            ++sampler_index;
        }
    }
    const u32 texel_count = Shader::NumDescriptors(info.texture_buffer_descriptors);
    for (u32 i = 0; i < texel_count; ++i) {
        descriptors.WriteTexelSrv(srv, srv_index++, bound.texel[i]);
        use(bound.texel[i].resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (; srv_index < srv_size; ++srv_index)
        descriptors.WriteNullSrv(srv, srv_index, D3D12_SRV_DIMENSION_TEXTURE2D);
    for (; sampler_index < sampler.count; ++sampler_index)
        descriptors.WriteNullSampler(sampler, sampler_index);
    u32 uav_index = 0;
    for (const auto& buffer : bound.storage) {
        descriptors.WriteStorageUav(uav, uav_index++, buffer);
        use(buffer.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    std::vector<VideoCommon::ImageViewId> written_images;
    const u32 image_start = uav_index;
    uav_index += Shader::NumDescriptors(info.image_descriptors);
    for (size_t i = texel_count; i < bound.texel.size(); ++i) {
        descriptors.WriteTexelUav(uav, uav_index++, bound.texel[i]);
        use(bound.texel[i].resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const u32 uav_end = uav_index;
    uav_index = image_start;
    for (const auto& desc : info.image_descriptors)
        for (u32 i = 0; i < desc.count; ++i) {
            const auto view_id = view_scratch[view_index++].id;
            auto& view = texture_cache.GetImageView(view_id);
            if (desc.is_written && view_id != VideoCommon::NULL_IMAGE_VIEW_ID)
                written_images.push_back(view_id);
            if (uav_index < 32 && view.IsRescaled())
                compute_image_scaling_mask |= 1u << uav_index;
            const auto format = PixelFormatFromImageFormat(desc.format);
            if (!format && desc.format != Shader::ImageFormat::Typeless)
                throw std::runtime_error("D3D12 unsupported compute storage image format");
            const auto guest_format = format.value_or(view.format);
            if (const auto problem =
                    StorageImageProblem(context, guest_format, desc.format, desc.is_integer,
                                        desc.is_read, desc.is_written)) {
                SkipDraw(problem);
                return;
            }
            if (!descriptors.WriteImageUav(
                    uav, uav_index++, view,
                    static_cast<DXGI_FORMAT>(MaxwellFormat(guest_format).view_format))) {
                SkipDraw("unsupported compute storage image view");
                return;
            }
            use(view.ImageHandle(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (const auto& image = view.ImageHandle())
                image_writes.push_back(subresources_of(image, view));
        }
    uav_index = uav_end;
    for (auto& read : image_reads) {
        bool written = false, overlaps = false;
        for (const auto& write : image_writes) {
            if (write.first != read.first)
                continue;
            written = true;
            for (const u32 sub : read.second)
                overlaps |= std::ranges::find(write.second, sub) != write.second.end();
        }
        const auto flags = read.first->handle->GetDesc().Flags;
        if (written && !overlaps && !(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            split_reads.push_back(std::move(read));
        else
            use(read.first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (written && overlaps) {
            // The dispatch is still skipped as a state conflict; name the subresources involved.
            static u32 logged = 0;
            if (logged++ < 16) {
                std::string writes;
                for (const auto& write : image_writes)
                    if (write.first == read.first)
                        writes += fmt::format("{} ", fmt::join(write.second, ","));
                Diagnostic(fmt::format("D3D12_COMPUTE_READ_WRITE_OVERLAP reads={} writes={}",
                                       fmt::join(read.second, ","), writes));
            }
        }
    }
    for (; uav_index < uav_size; ++uav_index)
        descriptors.WriteNullUav(uav, uav_index);
    ComputeBindings dispatch;
    dispatch.pipeline = pipeline->PipelineState();
    dispatch.root_signature = pipeline->RootSignature();
    dispatch.resource_heap = descriptors.ResourceHeap();
    dispatch.sampler_heap = descriptors.SamplerHeap();
    dispatch.tables = {cbv.gpu, srv.gpu, uav.gpu, sampler.gpu};
    dispatch.root_cbvs = root_cbvs;
    dispatch.root_cbv_addresses = root_cbv_addresses;
    Shader::Backend::HLSL::RootConstants constants;
    constants.texture_scaling_mask = compute_texture_scaling_mask;
    constants.image_scaling_mask = compute_image_scaling_mask;
    std::memcpy(dispatch.constants.data(), &constants, sizeof(constants));
    dispatch.resources = resources;
    dispatch.split_reads = split_reads;
    profile_descriptors.reset();
    const ComputeGroups groups{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
    if (!HasComputeWork(groups)) {
        VideoCommon::SubmissionTrace::Log("host_compute_drop reason=zero_grid");
        bindings.stages[0].Clear();
        TickWork();
        return;
    }
    texture_cache.BeginPassDumpImageWrite();
    context.MarkWork("guest compute");
    VideoCommon::SubmissionTrace::Log("host_compute_submit program={:x} grid={},{},{}",
                                      qmd.program_start, qmd.grid_dim_x.Value(),
                                      qmd.grid_dim_y.Value(), qmd.grid_dim_z.Value());
    compute_dispatch.Direct(dispatch, groups);
    trace_submitted = true;
    // VisitImageView prepares reads only. Publish UAV writes to the cache as Vulkan does,
    // so alias synchronization and guest downloads see the dispatched version.
    texture_cache.PublishShaderImageWrites(written_images, "compute_uav", qmd.program_start);
    bindings.stages[0].Clear();
    TickWork();
} catch (const std::exception& error) {
    if (OnGpuPipeline())
        throw;
    SkipDraw(error.what());
    TickWork();
} catch (...) {
    if (OnGpuPipeline())
        throw;
    SkipDraw("unknown exception in compute dispatch");
    TickWork();
}
void RasterizerD3D12::ResetCounter(VideoCommon::QueryType type) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return ResetCounter(type); });

    query_cache.CounterReset(type);
}
void RasterizerD3D12::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                            VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return Query(gpu_addr, type, flags, payload, subreport); });

    query_cache.CounterReport(gpu_addr, type, flags, payload, subreport);
}
void RasterizerD3D12::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                u32 size) {
    if (OnGpuPipeline())
        return RunGpuPipeline(
            [&] { return BindGraphicsUniformBuffer(stage, index, gpu_addr, size); });

    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}
void RasterizerD3D12::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return DisableGraphicsUniformBuffer(stage, index); });

    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}
void RasterizerD3D12::FlushAll() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return FlushAll(); });

    fence_manager.SignalReference();
    fence_manager.WaitPendingFences(true);
    context.Finish(FrameCount::ForcedSubmitFlushAll);
}
void RasterizerD3D12::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return FlushRegion(addr, size, which); });

    query_runtime.FlushPendingReports(addr, size);
    if (!addr || !size)
        return;
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(addr, size);
    }
    // A report can be newer than overlapping cached buffer/image contents.
    // Publish queries last so downloads cannot overwrite the final guest value.
    if (True(which & VideoCommon::CacheType::QueryCache)) {
        VideoCommon::SubmissionTrace::Log("query_flush_begin cpu={:x} size={}", addr, size);
        query_cache.FlushRegion(addr, size);
        VideoCommon::SubmissionTrace::Log("query_flush_end cpu={:x} size={}", addr, size);
    }
}
bool RasterizerD3D12::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return MustFlushRegion(addr, size, which); });

    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    return (True(which & VideoCommon::CacheType::QueryCache) &&
            query_cache.IsRegionGpuModified(addr, size)) ||
           (True(which & VideoCommon::CacheType::BufferCache) &&
            buffer_cache.IsRegionGpuModified(addr, size)) ||
           (True(which & VideoCommon::CacheType::TextureCache) &&
            texture_cache.IsRegionGpuModified(addr, size));
}
void RasterizerD3D12::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return InvalidateRegion(addr, size, which); });

    query_runtime.FlushPendingReports(addr, size);
    if (!addr || !size)
        return;
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    if (True(which & VideoCommon::CacheType::BufferCache))
        buffer_cache.WriteMemory(addr, size);
    if (True(which & VideoCommon::CacheType::TextureCache))
        texture_cache.WriteMemory(addr, size);
    if (True(which & VideoCommon::CacheType::ShaderCache))
        default_pipeline_cache->InvalidateRegion(addr, size);
    if (True(which & VideoCommon::CacheType::QueryCache))
        query_cache.InvalidateRegion(addr, size);
}
void RasterizerD3D12::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return InnerInvalidation(sequences); });

    for (const auto& [addr, size] : sequences)
        query_runtime.FlushPendingReports(addr, size);

    // FlushCaching drains guest inline writes, even when CPU inner invalidation is skipped.
    // Match Vulkan: make those bytes CPU-owned before the next buffer synchronization.
    {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            texture_cache.WriteMemory(addr, size);
        }
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            buffer_cache.WriteMemory(addr, size);
        }
    }
    for (const auto& [addr, size] : sequences) {
        query_cache.InvalidateRegion(addr, size);
        default_pipeline_cache->InvalidateRegion(addr, size);
    }
}
bool RasterizerD3D12::OnCPUWrite(DAddr addr, u64 size) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return OnCPUWrite(addr, size); });

    VideoCommon::SubmissionTrace::Log("cpu_write_notify cpu={} bytes={}", addr, size);

    {
        std::scoped_lock lock{buffer_cache.mutex};
        // Defer cached CPU stores while the buffer still owns GPU-written bytes.
        if (buffer_cache.OnCPUWrite(addr, size))
            return true;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    default_pipeline_cache->InvalidateRegion(addr, size);
    return false;
}
void RasterizerD3D12::OnCacheInvalidation(DAddr addr, u64 size) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return OnCacheInvalidation(addr, size); });

    query_runtime.FlushPendingReports(addr, size);
    // Gathering cached CPU stores is not an explicit guest query overwrite.
    // Keep pending reports alive, as the OpenGL/Vulkan backends do.
    InvalidateRegion(addr, size,
                     VideoCommon::CacheType::BufferCache | VideoCommon::CacheType::TextureCache |
                         VideoCommon::CacheType::ShaderCache);
}
VideoCore::RasterizerDownloadArea RasterizerD3D12::GetFlushArea(PAddr addr, u64 size) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return GetFlushArea(addr, size); });

    {
        std::scoped_lock lock{texture_cache.mutex};
        if (const auto area = texture_cache.GetFlushArea(addr, size))
            return *area;
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (const auto area = buffer_cache.GetFlushArea(addr, size))
            return *area;
    }
    VideoCore::RasterizerDownloadArea new_area{
        .start_address = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),
        .end_address = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),
        .preemtive = true,
    };
    return new_area;
}
void RasterizerD3D12::InvalidateGPUCache() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return InvalidateGPUCache(); });

    // invalidate_texture_data_cache and fence signals make attachment writes visible to
    // texture reads: later feedback samples need a fresh copy.
    ++feedback_epoch;
    m_gpu.InvalidateGPUCache();
}
void RasterizerD3D12::UnmapMemory(DAddr addr, u64 size) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return UnmapMemory(addr, size); });

    query_runtime.FlushPendingReports(addr, size);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    default_pipeline_cache->InvalidateRegion(addr, size);
    query_cache.InvalidateRegion(addr, size);
    texture_cache.UnmapMemory(addr, size);
    buffer_cache.WriteMemory(addr, size);
}
void RasterizerD3D12::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return ModifyGPUMemory(as_id, addr, size); });

    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    texture_cache.UnmapGPUMemory(as_id, addr, size);
    buffer_cache.UnmapGPUMemory(as_id, addr, size);
}
void RasterizerD3D12::SignalFence(std::function<void()>&& func) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return SignalFence(std::move(func)); });

    fence_manager.SignalFence(std::move(func));
}
void RasterizerD3D12::SyncOperation(std::function<void()>&& func) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return SyncOperation(std::move(func)); });

    fence_manager.SyncOperation(std::move(func));
}
void RasterizerD3D12::ProfileGpuThread(bool active) {
    if (OnGpuPipeline()) {
        if (!active) {
            // The GPU thread exits before channel payloads are destroyed. Join
            // here too, including after a failed job, while their snapshots are valid.
            gpu_pipeline->Close();
            Common::GpuProfile::enabled.store(false);
            return;
        }
        return gpu_pipeline->Invoke([&] { ProfileGpuThread(active); });
    }

    if (!context.Profile().Enabled())
        return;
    if (active) {
        Common::GpuProfile::Reset();
        Common::GpuProfile::enabled.store(true);
        context.Profile().BindRecordingThread(true);
        Common::GpuProfile::sink = &context.Profile();
        Common::GpuProfile::recording_enabled = true;
        Common::MaxwellMethodProfile::collector =
            Common::MaxwellMethodProfile::Enabled() ? &context.Profile() : nullptr;
    } else {
        Common::MaxwellMethodProfile::collector = nullptr;
        Common::GpuProfile::recording_enabled = false;
        Common::GpuProfile::sink = nullptr;
        Common::GpuProfile::enabled.store(false);
    }
}
void RasterizerD3D12::SignalSyncPoint(u32 value) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return SignalSyncPoint(value); });

    fence_manager.SignalSyncPoint(value);
}
void RasterizerD3D12::SignalReference() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return SignalReference(); });

    fence_manager.SignalReference();
}
void RasterizerD3D12::ReleaseFences(bool force) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return ReleaseFences(force); });

    // Every blocking drain (puller semaphores/syncpoints, queries) goes through here. Recorded
    // without a SyncWait phase so sync_wait stays comparable; overlaps puller_acquire.
    const auto start = force ? Common::GpuProfile::Start() : Common::GpuProfile::Tick{};
    fence_manager.WaitPendingFences(force);
    Common::GpuProfile::Record(Common::GpuProfile::Wait::ForcedDrain, start);
}
void RasterizerD3D12::FlushAndInvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return FlushAndInvalidateRegion(addr, size, which); });

    FlushRegion(addr, size, which);
    InvalidateRegion(addr, size, which);
}
void RasterizerD3D12::WaitForIdle() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return WaitForIdle(); });

    const Common::GpuProfile::PhaseScope gpu_profile_scope{Common::GpuProfile::Phase::SyncWait};
    ++feedback_epoch;
    query_cache.NotifyWFI();
    // Guest WFI is a GPU-side ordering point, as in the Vulkan backend. Waiting on the CPU here
    // ran ~34 times per frame in SM3DW and serialized the GPU thread with the GPU.
    static const bool cpu_wait = BisectEnabled("NXBOX_D3D12_WFI_FINISH");
    if (cpu_wait) {
        context.Finish(FrameCount::ForcedSubmitWaitForIdle);
        fence_manager.WaitPendingFences(true);
    } else {
        context.GlobalUavBarrier();
    }
    fence_manager.SignalOrdering();
}
void RasterizerD3D12::FragmentBarrier() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return FragmentBarrier(); });

    ++feedback_epoch;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    context.Commands()->ResourceBarrier(1, &barrier);
}
void RasterizerD3D12::TiledCacheBarrier() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return TiledCacheBarrier(); });

    FragmentBarrier();
}
void RasterizerD3D12::FlushCommands() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return FlushCommands(); });

    // DispatchCalls reaches this boundary before the GPU command queue can go idle.
    // Submit also flushes query reduction, but only a fence publishes its CPU callbacks.
    // SignalFence may reenter FlushCommands; its operations have already been moved then.
    fence_manager.CommitPendingOperations();
    context.Submit(FrameCount::ForcedSubmitGuestFlush);
    fence_manager.NotifyDeferredSubmit();
    default_pipeline_cache->FlushDiskCache();
}
void RasterizerD3D12::FlushDeferredFences() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return FlushDeferredFences(); });

    fence_manager.FlushPendingFences();
}
void RasterizerD3D12::NotifyFencesSubmitted() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return NotifyFencesSubmitted(); });

    fence_manager.NotifyDeferredSubmit();
}
void RasterizerD3D12::TickFrame() {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return TickFrame(); });

    default_pipeline_cache->TickFrame();
    if (VideoCommon::ShaderDiscovery::Enabled())
        VideoCommon::ShaderDiscovery::EndFrame(gpu_memory);
    // Bisect toggles: each one skips one cache's per-frame aging.
    if (!BisectEnabled("NXBOX_D3D12_TICK_NO_FENCE"))
        fence_manager.TickFrame();
    if (!BisectEnabled("NXBOX_D3D12_TICK_NO_DESCRIPTORS"))
        descriptors.TickFrame();
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    if (!BisectEnabled("NXBOX_D3D12_TICK_NO_BUFFER"))
        buffer_cache.TickFrame();
    if (!BisectEnabled("NXBOX_D3D12_TICK_NO_TEXTURE"))
        texture_cache.TickFrame();
    VideoCommon::SubmissionTrace::EndFrame(texture_cache.CaptureFrame());
    if (feedback_frame < 8 || feedback_frame % 120 == 0) {
        u64 pool_bytes{};
        for (const auto& entry : feedback_pool)
            pool_bytes += entry.resource->allocation_size;
        Diagnostic("D3D12_FEEDBACK copies=" + std::to_string(feedback_copies) +
                   " readonly_depth=" + std::to_string(feedback_readonly_depth) +
                   " skipped_draws=" + std::to_string(skipped_draws) +
                   " frame=" + std::to_string(feedback_frame) +
                   " pool_entries=" + std::to_string(feedback_pool.size()) +
                   " pool_bytes=" + std::to_string(pool_bytes) +
                   " memory_bytes=" + std::to_string(context.MemoryUsage()));
    }
    feedback_copies = feedback_readonly_depth = skipped_draws = 0;
    std::erase_if(feedback_pool,
                  [this](const auto& entry) { return entry.frame != feedback_frame; });
    std::erase_if(attachment_pool,
                  [this](const auto& entry) { return entry.frame != feedback_frame; });
    depth_trace_draws = depth_trace_clears = 0;
    ++feedback_frame;
}
Tegra::Engines::AccelerateDMAInterface& RasterizerD3D12::AccessAccelerateDMA() {
    if (OnGpuPipeline())
        return RunGpuPipeline(
            [&]() -> Tegra::Engines::AccelerateDMAInterface& { return AccessAccelerateDMA(); });

    return m_accelerate_dma;
}
bool RasterizerD3D12::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                            const Tegra::Engines::Fermi2D::Surface& dst,
                                            const Tegra::Engines::Fermi2D::Config& copy_config) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return AccelerateSurfaceCopy(src, dst, copy_config); });

    const auto format =
        MaxwellFormat(VideoCore::Surface::PixelFormatFromRenderTargetFormat(dst.format));
    const u64 bytes =
        u64{static_cast<u32>(std::max<s64>(0, s64{copy_config.dst_x1} - copy_config.dst_x0))} *
        static_cast<u32>(std::max<s64>(0, s64{copy_config.dst_y1} - copy_config.dst_y0)) *
        VideoCore::Surface::BytesPerBlock(
            VideoCore::Surface::PixelFormatFromRenderTargetFormat(dst.format));
    const auto src_format =
        MaxwellFormat(VideoCore::Surface::PixelFormatFromRenderTargetFormat(src.format));
    const auto plain = [](const auto& f) {
        return f.Supported() && !f.depth && !f.converted_astc &&
               f.conversion == FormatConversion::None && f.block_width == 1 &&
               f.block_height == 1;
    };
    bool accelerated = false;
    const char* reason = nullptr;
    // Color SrcCopy blits run on the GPU: exact same-format copies as copies, scaled or
    // format-changing ones as draws (CanBlitImages checks the cached images).
    // Depth, ROP/blend operations and converted formats retain the software path.
    if (!BisectEnabled("NXBOX_D3D12_NO_FERMI_COPY") && src.layer == 0 && dst.layer == 0 &&
        src.depth == 1 && dst.depth == 1 && plain(format) && plain(src_format) &&
        copy_config.operation == Tegra::Engines::Fermi2D::Operation::SrcCopy) {
        std::scoped_lock lock{texture_cache.mutex};
        auto gpu_copy = copy_config;
        // Preserve the shared cache's CPU-only heuristic for tiny copies; creating
        // and uploading two textures can cost more than the software copy itself.
        gpu_copy.must_accelerate = copy_config.must_accelerate || bytes >= (256ULL << 10);
        texture_runtime.blit_reject = nullptr;
        accelerated = texture_cache.BlitImage(dst, src, gpu_copy);
        reason = texture_runtime.blit_reject ? texture_runtime.blit_reject
                                             : "no cached image for the surfaces";
    } else {
        reason = src.layer != 0 || dst.layer != 0 || src.depth != 1 || dst.depth != 1
                     ? "layered or 3D surface"
                 : !plain(format) || !plain(src_format) ? "surface format is depth or converted"
                 : copy_config.operation != Tegra::Engines::Fermi2D::Operation::SrcCopy
                     ? "operation is not SrcCopy"
                     : "disabled by NXBOX_D3D12_NO_FERMI_COPY";
    }
    if (!accelerated) {
        texture_runtime.RecordSoftwareBlit(dst.Address());
        // The first 32 large software blits, then one in 512: enough to name the cause.
        static std::atomic<u32> logged{0};
        const u32 index = logged.fetch_add(1, std::memory_order_relaxed);
        if (bytes >= (256ULL << 10) && (index < 32 || index % 512 == 0)) {
            Diagnostic(fmt::format(
                "D3D12_FERMI_SOFTWARE reason=\"{}\" src_format={} dst_format={} src={}x{} "
                "dst={}x{} src_linear={} dst_linear={} src_region={},{}-{},{} "
                "dst_region={},{}-{},{} filter={} operation={} src_addr={:#x} dst_addr={:#x}",
                reason, static_cast<u32>(src.format), static_cast<u32>(dst.format), src.width,
                src.height, dst.width, dst.height,
                src.linear == Tegra::Engines::Fermi2D::MemoryLayout::Pitch,
                dst.linear == Tegra::Engines::Fermi2D::MemoryLayout::Pitch, copy_config.src_x0,
                copy_config.src_y0, copy_config.src_x1, copy_config.src_y1, copy_config.dst_x0,
                copy_config.dst_y0, copy_config.dst_x1, copy_config.dst_y1,
                static_cast<u32>(copy_config.filter), static_cast<u32>(copy_config.operation),
                src.Address(), dst.Address()));
        }
    }
    VideoCommon::SubmissionTrace::Log(
        "host_fermi_blit src_gpu={:x} dst_gpu={:x} src_format={} dst_format={} accelerated={}",
        src.Address(), dst.Address(), static_cast<u32>(src.format), static_cast<u32>(dst.format),
        accelerated);
    context.Profile().Count(accelerated ? FrameCount::FermiBlitsAccelerated
                                        : FrameCount::FermiBlitsSoftware);
    context.Profile().Count(accelerated ? FrameCount::FermiBlitAcceleratedBytes
                                        : FrameCount::FermiBlitSoftwareBytes,
                            bytes);
    return accelerated;
}
void RasterizerD3D12::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                               std::span<const u8> memory) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return AccelerateInlineToMemory(address, copy_size, memory); });

    if (gpu_memory && copy_size <= memory.size()) {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
    }
}
void RasterizerD3D12::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                        const VideoCore::DiskResourceLoadCallback& callback) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return LoadDiskResources(title_id, stop_loading, callback); });

    default_pipeline_cache->LoadDiskResources(title_id, stop_loading, callback);
}
void RasterizerD3D12::LoadDiskResourcesWithPreload(
    u64 title_id, std::stop_token stop_loading, const VideoCore::DiskResourceLoadCallback& callback,
    const std::function<bool()>& skip_wait) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] {
            return LoadDiskResourcesWithPreload(title_id, stop_loading, callback, skip_wait);
        });

    default_pipeline_cache->LoadDiskResources(title_id, stop_loading, callback, true, skip_wait);
}

void RasterizerD3D12::InitializeChannel(Tegra::Control::ChannelState& channel) {
    if (OnGpuPipeline())
        return gpu_pipeline->Invoke([&] { InitializeChannel(channel); });
    if (gpu_pipeline && !channel.pipeline_maxwell) {
        channel.pipeline_maxwell =
            std::make_unique<Tegra::Engines::Maxwell3D>(*channel.memory_manager);
        channel.pipeline_maxwell->BindRasterizer(this);
    }

    // Preserve common cache flags while registering the native register dependencies.
    auto& dirty = channel.payload->maxwell_3d.dirty;
    StateTracker::SetupDirtyFlags(dirty.tables);
    dirty.flags.set();
    if (channel.pipeline_maxwell)
        channel.pipeline_maxwell->dirty = dirty;
    CreateChannel(channel);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.CreateChannel(channel);
    texture_cache.CreateChannel(channel);
    default_pipeline_cache->CreateChannel(channel);
    query_cache.CreateChannel(channel);
}
void RasterizerD3D12::BindChannel(Tegra::Control::ChannelState& channel) {
    if (OnGpuPipeline()) {
        DrainGpuPipeline();
        pipeline_frontend_owner = this;
        frontend_channel_id = channel.bind_id;
        // Set only after the previous channel has drained.
        frontend_maxwell = &channel.payload->maxwell_3d;
        return gpu_pipeline->Invoke([&] { BindChannel(channel); });
    }

    BindToChannel(channel.bind_id);
    StateTracker::Bind(*maxwell3d);
    context.GraphicsStateTracker().Invalidate();
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.BindToChannel(channel.bind_id);
    texture_cache.BindToChannel(channel.bind_id);
    default_pipeline_cache->BindToChannel(channel.bind_id);
    query_cache.BindToChannel(channel.bind_id);
    query_runtime.Bind3DEngine(maxwell3d);
}
void RasterizerD3D12::ReleaseChannel(s32 channel_id) {
    if (OnGpuPipeline()) {
        RunGpuPipeline([&] { ReleaseChannel(channel_id); });
        if (frontend_channel_id == channel_id) {
            frontend_maxwell = nullptr;
            frontend_channel_id = -1;
        }
        return;
    }

    EraseChannel(channel_id);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    buffer_cache.EraseChannel(channel_id);
    texture_cache.EraseChannel(channel_id);
    default_pipeline_cache->EraseChannel(channel_id);
    query_cache.EraseChannel(channel_id);
}

RasterizerD3D12::DisplayTexture RasterizerD3D12::FramebufferTexture(
    const Tegra::FramebufferConfig& config) {
    if (OnGpuPipeline())
        return RunGpuPipeline([&] { return FramebufferTexture(config); });

    std::scoped_lock lock{texture_cache.mutex};
    auto [view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, config.address + config.offset);
    const auto log_source = [&](const ResourcePtr& resource, DXGI_FORMAT format, bool cached) {
        if (cached)
            texture_cache.SnapshotPassDumpPresentation(*view);
        const auto mapping = texture_cache.RecordPassDumpPresentation(
            config.address + config.offset, cached ? view : nullptr);
        if (!mapping.empty())
            Diagnostic("D3D12_PASS_PRESENT " + mapping);
        if (presentation_logged)
            return;
        presentation_logged = true;
        const auto desc = resource->handle->GetDesc();
        Diagnostic("D3D12_DISPLAY address=" + std::to_string(config.address + config.offset) +
                   " source=" + (cached ? "texture_cache" : "guest_memory") + " image=" +
                   std::to_string(reinterpret_cast<std::uintptr_t>(resource->handle.Get())) +
                   " format=" + std::to_string(static_cast<u32>(format)) +
                   " width=" + std::to_string(desc.Width) +
                   " height=" + std::to_string(desc.Height) + " scaled=" + std::to_string(scaled));
    };
    if (view && view->ImageHandle()) {
        view = texture_cache.PrepareImageForPresentation(view);
        if (!view || !view->ImageHandle())
            return {};
        auto description = view->Description();
        if (view->ImageHandle()->handle->GetDesc().SampleDesc.Count > 1) {
            auto resource = texture_runtime.ResolveFramebuffer(*view);
            if (!resource)
                return {};
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Texture2DArray = {0, 1, 0, 1, 0, 0};
            log_source(resource, description.Format, true);
            return {resource, description, scaled, view->size.width, view->size.height};
        }
        if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            const auto mip = description.Texture2D.MostDetailedMip;
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Texture2DArray = {mip, 1, 0, 1, 0, 0};
        } else if (description.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
            description.Texture2DArray.ArraySize = 1;
            description.Texture2DArray.MipLevels = 1;
        } else {
            view->LogUnsupported("framebuffer must be a 2D texture view");
            return {};
        }
        log_source(view->ImageHandle(), description.Format, true);
        return {view->ImageHandle(), description, scaled, view->size.width, view->size.height};
    }
    // The fallback reads guest RAM directly. An incompatible format/size alias may
    // still own newer GPU pixels; DownloadMemory writes whole images overlapping
    // the display base, ordered by modification tick, before that CPU read.
    // Default on: without it MK8D, Odyssey and Wonder present black frames (the fallback reads
    // guest memory the GPU never wrote back). NXBOX_PRESENT_FALLBACK_DOWNLOAD=0 disables it.
    const bool download_before_fallback = [] {
        const char* value = std::getenv("NXBOX_PRESENT_FALLBACK_DOWNLOAD");
        return !(value && value[0] == '0');
    }();
    if (download_before_fallback)
        texture_cache.DownloadMemory(config.address + config.offset, 1);
    auto resource = texture_runtime.UploadFramebuffer(device_memory, config);
    if (!resource)
        return {};
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = static_cast<DXGI_FORMAT>(
        MaxwellFormat(VideoCore::Surface::PixelFormatFromGPUPixelFormat(config.pixel_format))
            .view_format);
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2DArray = {0, 1, 0, 1, 0, 0};
    log_source(resource, description.Format, false);
    return {resource, description};
}
} // namespace D3D12
