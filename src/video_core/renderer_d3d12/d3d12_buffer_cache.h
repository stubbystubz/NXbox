// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <string_view>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"
#include "video_core/renderer_d3d12/d3d12_uniform_stream.h"
#include "video_core/renderer_d3d12/d3d12_upload_cache.h"
#include "video_core/renderer_d3d12/d3d12_usage_cache.h"

namespace D3D12 {
class BufferCacheRuntime;
class Buffer : public VideoCommon::BufferBase {
public:
    Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams);
    Buffer(BufferCacheRuntime&, VAddr address, u64 size, bool sparse);
    ResourcePtr Handle() const {
        return resource;
    }
    operator ResourcePtr() const {
        return resource;
    }
    bool IsSparseCompatible() const {
        return false;
    }
    bool IsRegionUsed(u64 offset, u64 size) const {
        return usage.IsUsed(offset, size);
    }
    void MarkUsage(u64 offset, u64 size);
    void ResetUsageTracking() {
        usage.Reset();
        usage_ranges.Clear();
        usage_cached = false;
    }
    u64 LastUsageTick() const {
        return tick;
    }

private:
    Context* context{};
    ResourcePtr resource;
    VideoCommon::UsageTracker usage{0};
    u64 tick{};
    u64 usage_offset{}, usage_size{};
    bool usage_cached{};
    UsageCache usage_ranges;
    bool usage_reuse{[] {
        const char* value = std::getenv("NXBOX_D3D12_USAGE_REUSE");
        return !value || std::string_view{value} != "0";
    }()};
    bool binding_cache{BisectEnabled("NXBOX_D3D12_BINDING_CACHE")};
};
class BufferCacheRuntime {
public:
    BufferCacheRuntime(Context& context, BindingState& bindings)
        : context{context}, bindings{bindings},
          upload{context, D3D12_HEAP_TYPE_UPLOAD, [this] { uniform_stream.Clear(); }},
          readback{context, D3D12_HEAP_TYPE_READBACK},
          uniform_snapshots{context, D3D12_HEAP_TYPE_DEFAULT, [this] {
                                uniform_copies.Clear();
                                InvalidateUniformBindings();
                            }} {}
    bool synchronizing_indices{};
    bool BufferFastpathEnabled() const {
        return buffer_fastpath;
    }
    bool BindingResolutionCacheEnabled() const {
        return binding_resolution_cache;
    }
    bool TryReuseUniformBinding(const Buffer& buffer, u32 offset, u32 size, u32 read_limit);
    bool TryReuseStorageBinding(const Buffer& buffer, u32 offset, u32 size, bool is_written);
    bool TryReuseVertexBinding(u32 index, const Buffer& buffer, u32 offset, u32 size, u32 stride);
    bool TryReuseIndexBinding(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology,
                              Tegra::Engines::Maxwell3D::Regs::IndexFormat format,
                              const Buffer& buffer, u32 offset, u32 size);
    bool BindingCacheEnabled() const {
        return binding_cache;
    }
    bool PreferCachedUniformBuffers() const {
        return true;
    }
    void RecordBufferUpload(VideoCommon::BufferUploadSource source, u64 bytes);
    void TickFrame(Common::SlotVector<Buffer>& buffers);
    u64 CurrentTick() {
        return context.CurrentTick();
    }
    bool IsFree(u64 tick) {
        return context.IsFree(tick);
    }
    void Wait(u64 tick) {
        context.Wait(tick, "buffer_write");
    }
    void Finish() {
        context.Wait(context.Submit(FrameCount::ForcedSubmitBufferDownload), "buffer_download");
    }
    void FinishDmaCopy() {
        // This is a DEFAULT-to-DEFAULT copy, not a CPU readback. Subsequent GPU work
        // uses the same ordered queue, and dirty guest ranges keep the real CPU read
        // on DownloadBufferMemory's synchronous path. Keep deferral opt-in until
        // console validation covers guest DMA/semaphore interactions.
        if (!BisectEnabled("NXBOX_D3D12_ASYNC_DMA")) {
            context.Wait(context.Submit(FrameCount::ForcedSubmitBufferDownload), "dma_copy");
        }
    }
    u64 GetDeviceLocalMemory() const {
        return memory_target;
    }
    u64 GetDeviceMemoryUsage() const {
        const auto usage = context.MemoryUsage();
        return BufferMemoryPressure(usage, memory_target, NativeBufferMemoryTarget(usage));
    }
    bool CanReportMemoryUsage() const {
        return true;
    }
    u32 GetUniformBufferAlignment() const {
        return 256;
    }
    u32 GetStorageBufferAlignment() const {
        return 16;
    }
    StagingBufferRef UploadStagingBuffer(size_t size) {
        const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
        context.Profile().Count(FrameCount::BufferUploadBytes, size);
        context.Profile().Count(upload_source, size);
        upload_source = FrameCount::OtherUploadBytes;
        return upload.Request(size);
    }
    StagingBufferRef DownloadStagingBuffer(size_t size, bool = false) {
        const FrameProfile::Scope profile_download{context.Profile(), FramePhase::BufferDownload};
        context.Profile().Count(FrameCount::BufferDownloadBytes, size);
        return readback.Request(size);
    }
    void FreeDeferredStagingBuffer(StagingBufferRef& ref) {
        ref = {};
    }
    bool CanReorderUpload(const Buffer&, std::span<const VideoCommon::BufferCopy>) {
        return false;
    }
    void PreCopyBarrier() {}
    void PostCopyBarrier() {}
    void CopyBuffer(ResourcePtr dst, ResourcePtr src,
                    std::span<const VideoCommon::BufferCopy> copies, bool, bool = false);
    void ClearBuffer(ResourcePtr dst, u32 offset, size_t size, u32 value);
    // P3 binding hooks: record into BindingState; the rasterizer builds the descriptor
    // tables and vertex/index views from it at draw time.
    void BindIndexBuffer(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology,
                         Tegra::Engines::Maxwell3D::Regs::IndexFormat format, u32, u32,
                         ResourcePtr buffer, u32 offset, u32 size);
    void BindQuadIndexBuffer(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology,
                             u32 first, u32 count);
    void BindVertexBuffer(u32 index, ResourcePtr buffer, u32 offset, u32 size, u32 stride);
    void BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings);
    void BindTransformFeedbackBuffer(u32, ResourcePtr, u32, u32) {}
    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>&) {}
    std::span<u8> BindMappedUniformBuffer(size_t stage, u32, u32 size);
    void BindUniformBuffer(const Buffer& buffer, u32 offset, u32 size);
    /// Bytes the bound shader reads from the next uniform binding; consumed by that binding.
    void SetUniformReadLimit(u32 limit) {
        uniform_read_limit = limit;
    }
    /// Snapshot of a uniform range in its own buffer, for when the source must be a UAV in the
    /// same draw (D3D12 cannot hold one buffer in CONSTANT_BUFFER and UNORDERED_ACCESS at once).
    BoundBuffer CopyUniform(const BoundBuffer& uniform);
    void BindStorageBuffer(const Buffer& buffer, u32 offset, u32 size, bool is_written);
    void BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                           VideoCore::Surface::PixelFormat format);
    void BindImageBuffer(Buffer& buffer, u32 offset, u32 size,
                         VideoCore::Surface::PixelFormat format);
    Context& context;

private:
    const bool buffer_fastpath = [] {
        const char* value = std::getenv("NXBOX_D3D12_BUFFER_FASTPATH");
        return !value || value[0] != '0' || value[1] != '\0';
    }();
    const bool binding_resolution_cache = [] {
        const char* value = std::getenv("NXBOX_D3D12_BUFFER_BINDINGS");
        return !value || value[0] != '0' || value[1] != '\0';
    }();
    const bool binding_cache{BisectEnabled("NXBOX_D3D12_BINDING_CACHE")};
    struct UniformBindingResolution {
        ResourcePtr source;
        u64 version{};
        u32 offset{}, size{}, read_limit{};
        BoundBuffer resolved;
    };
    // Indexed by the dense per-stage slot, also used by compute. Full keys handle
    // shader/channel switches; writes invalidate snapshots through content_version.
    std::array<std::array<UniformBindingResolution, VideoCommon::NUM_GRAPHICS_UNIFORM_BUFFERS>,
               BindingState::NumStages>
        uniform_bindings{};
    std::array<std::array<BoundBuffer, VideoCommon::NUM_STORAGE_BUFFERS>, BindingState::NumStages>
        storage_bindings{};
    void InvalidateUniformBindings() {
        uniform_bindings = {};
    }
    UniformCopyCache<Resource> uniform_copies;
    FrameCount upload_source{FrameCount::OtherUploadBytes};
    BindingState& bindings;
    const u64 memory_target{NativeBufferMemoryTarget(context.MemoryUsage())};
    UniformStream<StagingBufferRef> uniform_stream;
    StagingRing upload;
    StagingRing readback;
    StagingRing uniform_snapshots;
    u32 uniform_read_limit = ~0U;
    ResourcePtr witness_resource;
    u64 witness_offset{}, witness_hash{}, witness_frame{};
};
struct BufferCacheParams {
    using Runtime = BufferCacheRuntime;
    using Buffer = D3D12::Buffer;
    using Async_Buffer = StagingBufferRef;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;
    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = true;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};
using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;
} // namespace D3D12
