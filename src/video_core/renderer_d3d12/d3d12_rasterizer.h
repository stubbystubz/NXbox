// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <optional>
#include <tuple>
#include "common/common_types.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu_draw_pipeline.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_clear.h"
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
#include "video_core/renderer_d3d12/d3d12_dispatch.h"
#include "video_core/renderer_d3d12/d3d12_draw_resource_index.h"
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"
#include "video_core/renderer_d3d12/d3d12_index_converter.h"
#include "video_core/renderer_d3d12/d3d12_lighting_trace.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_iface.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_render_area.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/d3d12_transform_feedback.h"

namespace Core {
class System;
}

namespace D3D12 {

// Shared by the draw path and PrepareDrawIndices (a member), so it cannot stay file-local.
struct DrawParams {
    u32 base_instance;
    u32 num_instances;
    u32 base_vertex;
    u32 num_vertices;
    u32 first_index;
    bool is_indexed;
};

class RasterizerD3D12;
class PipelineCache;

class AccelerateDMA : public Tegra::Engines::AccelerateDMAInterface {
public:
    explicit AccelerateDMA();
    bool BufferCopy(GPUVAddr start_address, GPUVAddr end_address, u64 amount) override;
    bool BufferClear(GPUVAddr src_address, u64 amount, u32 value) override;
    bool ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::ImageOperand& src,
                       const Tegra::DMA::BufferOperand& dst) override {
        return false;
    }
    bool BufferToImage(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& src,
                       const Tegra::DMA::ImageOperand& dst) override {
        return false;
    }
};

// Shared guest caches drive native graphics/compute pipelines and query reporting.
class RasterizerD3D12 final : public VideoCore::RasterizerInterface,
                              protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    explicit RasterizerD3D12(Tegra::GPU& gpu, Tegra::MaxwellDeviceMemoryManager& memory,
                             Context& context);

    struct DisplayTexture {
        ResourcePtr resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        bool scaled{};
        u32 guest_width{}, guest_height{};
    };
    DisplayTexture FramebufferTexture(const Tegra::FramebufferConfig& config);
    ~RasterizerD3D12() override;

    void DrainGpuPipeline() override;
    bool HasGpuPipeline() const override {
        return gpu_pipeline != nullptr;
    }
    bool OnGpuPipeline() const {
        return gpu_pipeline && !gpu_pipeline->IsConsumer();
    }
    // Synchronous calls and presentation execute on the same consumer as draws.
    template <class Func>
    auto RunGpuPipeline(Func&& function) -> std::invoke_result_t<Func> {
        if (OnGpuPipeline()) {
            // Only the GPU producer may read frontend registers. CPU invalidation
            // callbacks use the queue without reading the concurrently decoding engine.
            if (pipeline_frontend_owner == this && frontend_maxwell) {
                auto snapshot = VideoCommon::CaptureDrawSnapshot(*frontend_maxwell);
                return gpu_pipeline->Invoke(
                    [&, snapshot = std::move(snapshot)]() -> std::invoke_result_t<Func> {
                        snapshot.Apply(*maxwell3d);
                        return function();
                    });
            }
            return gpu_pipeline->Invoke(std::forward<Func>(function));
        }
        return function();
    }

    void Draw(bool is_indexed, u32 instance_count) override;
    void DrawIndirect() override;
    // Overrides the built-in guest shader pipeline cache for alternate providers.
    void SetPipelineCache(IPipelineCache* cache) {
        if (OnGpuPipeline())
            return RunGpuPipeline([&] { SetPipelineCache(cache); });
        pipeline_cache = cache;
    }
    void DrawTexture() override;
    bool AccelerateConditionalRendering() override;
    void PreparePresentation();
    void Clear(u32 layer_count) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override;
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override;
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override;
    void FlushAll() override;
    void FlushRegion(DAddr addr, u64 size,
                     VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(DAddr addr, u64 size,
                         VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void InvalidateRegion(DAddr addr, u64 size,
                          VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) override;
    void OnCacheInvalidation(DAddr addr, u64 size) override;
    bool OnCPUWrite(DAddr addr, u64 size) override;
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr addr, u64 size) override;
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) override;
    void SignalFence(std::function<void()>&& func) override;
    void SyncOperation(std::function<void()>&& func) override;
    void SignalSyncPoint(u32 value) override;
    void ProfileGpuThread(bool active) override;
    void SignalReference() override;
    void ReleaseFences(bool force) override;
    void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void FlushDeferredFences() override;
    // Present submitted the recording; wake the fencing thread if fences are batched.
    void NotifyFencesSubmitted();
    void TickFrame() override;
    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                               const Tegra::Engines::Fermi2D::Surface& dst,
                               const Tegra::Engines::Fermi2D::Config& copy_config) override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override;
    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) override;
    void LoadDiskResourcesWithPreload(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback,
                                      const std::function<bool()>& skip_wait) override;
    void InitializeChannel(Tegra::Control::ChannelState& channel) override;
    void BindChannel(Tegra::Control::ChannelState& channel) override;
    void ReleaseChannel(s32 channel_id) override;

private:
    Tegra::Engines::Maxwell3D* frontend_maxwell{};
    inline static thread_local const RasterizerD3D12* pipeline_frontend_owner{};
    s32 frontend_channel_id{-1};
    // Destroy/join explicitly before cache and context teardown.
    std::unique_ptr<VideoCommon::GpuDrawPipeline> gpu_pipeline;
    struct DynamicState {
        std::array<D3D12_VIEWPORT, Tegra::Engines::Maxwell3D::Regs::NumViewports> viewports{};
        std::array<D3D12_RECT, Tegra::Engines::Maxwell3D::Regs::NumViewports> scissors{};
        std::array<UINT, 3> stencil{};
        std::array<float, 4> blend{};
        std::array<float, 2> depth_bounds{};
        std::array<u32, 16> words{};
        bool y_flip{};
    };
    static DynamicState ComputeDynamicState(const Tegra::Engines::Maxwell3D::Regs& regs,
                                            const Pso::GraphicsPsoState& attachment_state,
                                            const Pso::FramebufferFormats& formats, u32 width,
                                            u32 height, const IGraphicsPipeline& pipeline,
                                            bool rescaled);
    // Snapshot identity includes views, draw-buffer mapping and framebuffer size. Holding
    // the snapshot prevents pointer reuse after eviction; rt_control.count is separate.
    struct DrawAttachments {
        std::shared_ptr<Framebuffer::Attachments> snapshot;
        u32 guest_rts{};
        Pso::FramebufferFormats formats{};
        std::array<ImageView*, VideoCommon::NUM_RT> colors{};
        bool area_valid{};
        AttachmentExtent area{}, matching_extent{};
    } draw_attachments;
    StateInputCache<GraphicsStateInputs> attachment_inputs;
    Pso::GraphicsPsoState cached_attachment_state{};
    StateInputCache<DynamicStateInputs> dynamic_inputs;
    DynamicState cached_dynamic_state;
    Context& context;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    BindingState bindings;
    D3D12DescriptorAllocator descriptors;
    /// Attachments last written into the fixed RTV/DSV slots (index NUM_RT is depth).
    struct BoundTargets {
        struct View {
            s32 level{}, layer{}, format{};
            bool operator==(const View&) const = default;
        };
        std::array<ResourcePtr, VideoCommon::NUM_RT + 1> resources{};
        std::array<View, VideoCommon::NUM_RT + 1> views{};
        u32 num_rts{}, layers{};
        D3D12_DSV_FLAGS dsv_flags{};
        bool operator==(const BoundTargets&) const = default;
    };
    BoundTargets bound_targets;
    IPipelineCache* pipeline_cache{};
    std::unique_ptr<PipelineCache> default_pipeline_cache;
    BufferCacheRuntime buffer_runtime;
    TextureCacheRuntime texture_runtime;
    BufferCache buffer_cache;
    TextureCache texture_cache;
    IndexConverter index_converter;
    std::vector<std::byte> index_read_scratch;
    std::optional<ConvertedIndexBuffer> converted_indices;
    ClearRenderer clear_renderer;
    ComputeDispatch compute_dispatch;
    TransformFeedback transform_feedback;
    QueryCacheRuntime query_runtime;
    QueryCache query_cache;
    FenceManager fence_manager;
    void TraceUniformWord(size_t stage, u32 index, const BoundBuffer& buffer);
    bool PrepareDrawIndices(bool is_indexed, DrawParams& params, const IGraphicsPipeline& pipeline);
    template <typename Func>
    void PrepareDraw(bool is_indexed, Func&& draw);
    ID3D12CommandSignature* IndirectSignature(bool indexed);
    std::array<u32, NUM_GRAPHICS_STAGES> uniform_masks{};
    VideoCommon::UniformBufferSizes uniform_sizes{};
    VideoCommon::ComputeUniformBufferSizes compute_uniform_sizes{};
    std::vector<VideoCommon::ImageViewInOut> view_scratch;
    std::vector<VideoCommon::SamplerId> sampler_scratch;
    // Copies only the sampled view's subresources when a range is given (color, single-sample).
    ResourcePtr CopyFeedback(const ResourcePtr& source,
                             const VideoCommon::SubresourceRange* range = nullptr);
    void SkipDraw(const char* reason);
    void TickWork();
    void DrawSynchronous(bool is_indexed, u32 instance_count);
    struct FeedbackTexture {
        ResourcePtr resource;
        const Resource* source{};
        u64 draw{};
        u64 frame{};
        std::vector<bool> copied; // per subresource, for the current draw
        u64 epoch{};              // feedback_epoch when copied (NXBOX_D3D12_FEEDBACK_REUSE)
        u64 source_version{};     // source content_version when copied
    };
    // Guest-visible coherence points: barriers, clears, attachment changes.
    u64 feedback_epoch{};
    std::array<const Resource*, VideoCommon::NUM_RT + 1> feedback_targets{};
    std::vector<FeedbackTexture> feedback_pool;
    std::vector<FeedbackTexture> attachment_pool;
    u64 feedback_draw{};
    u32 draw_trace_lines{};
    LightingTraceBudget lighting_trace;
    u32 cbuf_trace_lines{};
    u32 vb_trace_draws{};
    u32 glyph_trace_draws{};
    u64 feedback_frame{};
    u64 feedback_copies{};
    u64 feedback_readonly_depth{};
    u64 skipped_draws{};
    u32 attachment_diagnostics{};
    u32 attachment_extent_diagnostics{};
    u32 depth_trace_draws{}, depth_trace_clears{};
    bool feedback_failure_logged{};
    bool injected_draw_skip{};
    bool presentation_logged{};
    bool presentation_lookup_logged{};
    ResourcePtr zero_vertex_buffer;
    // Repacked vertex streams are ring slices, not a placed resource per draw.
    StagingRing vertex_repack{context, D3D12_HEAP_TYPE_UPLOAD};
    struct PaddedVertexBuffer {
        ResourcePtr source;
        ResourcePtr padded;
    };
    std::map<std::tuple<const Resource*, u64, u64>, PaddedVertexBuffer> padded_vertex_buffers;
    ResourcePtr zero_uniform_buffer;
    void PrepareZeroUniformBuffer();
    std::vector<std::pair<ResourcePtr, D3D12_RESOURCE_STATES>> state_scratch;
    DrawResourceIndex<> draw_resource_index;
    DescriptorPlan draw_view_plan{{0, 0, 0}};
    DescriptorPlan draw_sampler_plan{{0, 0, 0}};
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> draw_root_cbv_addresses;
    ComPtr<ID3D12CommandSignature> draw_signature;
    ComPtr<ID3D12CommandSignature> indexed_signature;
    bool draw_logged{};
    bool quad_logged{};
    bool image_uav_logged{};
    bool compute_logged{};
    Tegra::GPU& m_gpu;
    AccelerateDMA m_accelerate_dma;
};

} // namespace D3D12
