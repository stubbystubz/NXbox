// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <span>
#include <string>
#include <utility>

#include "video_core/query_cache/query_cache_base.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"

namespace VideoCommon {
class StreamerInterface;
}

namespace D3D12 {
class TransformFeedback;
class BufferCacheRuntime;
struct QueryCacheRuntimeImpl;

// Default-on draw bookkeeping shortcuts; set NXBOX_D3D12_RASTERIZER_STATE_FASTPATH=0 to bisect.
bool RasterizerStateFastpathEnabled();

// Integration: construct beside BufferCache/TransformFeedback, then construct
// QueryCache with (gpu, rasterizer, device_memory, runtime). Route channel binding,
// Query/ResetCounter, WFI, invalidation and fence flushes to QueryCache as Vulkan
// does. FenceManager commits/pops cache batches and defers guest callbacks;
// FlushCommands only submits native commands.
// Draw calls PrepareDraw BEFORE setting any command-list state (bank drain
// can reset the list), then BeginDraw/EndDraw immediately around Draw[Indexed].
// Enable ZPassPixelCount64 from regs.zpass_pixel_count_enable. Bind SO targets
// before BeginDraw; pass whether the last stage is a geometry shader. Clear and
// dispatch do not participate. NotifySegment pauses/resumes predication; BeginDraw
// also reapplies it after list resets. Never submit while a draw query is open.
// Results are accumulated on the GPU. Batches copy report values into guest buffers;
// readback snapshots retire with fences and the query cache delivers CPU-visible values.
// Guest report timestamps remain Tegra::GPU::GetTicks() in QueryCacheBase; CaptureTimestamp is
// an independent host GPU timestamp facility, never a substitute guest clock.
// Pipeline IAPrimitives is the fallback for generated primitives without a GS;
// tessellated primitive counts require SO statistics (no exact D3D12 DS count).
class QueryCacheRuntime {
public:
    QueryCacheRuntime(Context& context, Tegra::MaxwellDeviceMemoryManager& device_memory,
                      BufferCache& buffer_cache, TransformFeedback& transform_feedback);
    ~QueryCacheRuntime();
    void Bind3DEngine(Tegra::Engines::Maxwell3D* engine) {
        maxwell3d = engine;
    }
    template <typename Func>
    void View3DRegs(Func&& func) {
        if (maxwell3d) {
            std::forward<Func>(func)(*maxwell3d);
        }
    }
    template <typename SyncValuesType>
    void SyncValues(std::span<SyncValuesType> values, ResourcePtr source = {}) {
        for (const auto& entry : values) {
            if constexpr (SyncValuesType::GeneratesBaseBuffer) {
                WriteValue(entry.address, &entry.value, entry.size);
            } else {
                CopyValue(entry.address, source, entry.offset, entry.size);
            }
        }
    }
    VideoCommon::StreamerInterface* GetStreamerInterface(VideoCommon::QueryType type);
    void Barriers(bool is_prebarrier);
    void EndHostConditionalRendering();
    void PauseHostConditionalRendering();
    void ResumeHostConditionalRendering();
    bool HostConditionalRenderingCompareValue(VideoCommon::LookupData object, bool qc_dirty);
    bool HostConditionalRenderingCompareValues(VideoCommon::LookupData first,
                                               VideoCommon::LookupData second, bool qc_dirty,
                                               bool equal_check);
    void ReportQueryFenceWait();
    u64 ReportPending(VAddr address);
    void ReportCompleted(bool invalidated, u64 token);
    std::string VisibilityDiagnostics() const;
    void FlushPendingReports(DAddr address, u64 size);
    void PrepareDraw();
    void BeginDraw(bool has_geometry_shader = false);
    void EndDraw();
    void CancelDraw();
    struct CounterResult {
        u64 value{};
        StagingBufferRef result;
    };
    CounterResult ReadCounter(VideoCommon::QueryType type, u32 stream = 0, DAddr address = 0,
                              u32 size = 0);
    void ResetCounter(VideoCommon::QueryType type);
    void EnableCounter(VideoCommon::QueryType type, bool enabled);
    struct Timestamp {
        StagingBufferRef result;
        u64 tick{};
        u64 frequency{};
    };
    StagingBufferRef PreparePredicateTrace();
    void TracePredicate(u64 frame, u64 pass, u64 draw, const StagingBufferRef& result);
    Timestamp CaptureTimestamp();
    u64 ReadTimestamp(const Timestamp& timestamp);

private:
    void SetGpuPredicate(DAddr address, D3D12_PREDICATION_OP op, bool compare_words = false);
    bool CompareConditional(VideoCommon::LookupData first, VideoCommon::LookupData second,
                            bool equal_check);
    void WriteValue(DAddr address, const void* value, u64 size);
    void CopyValue(DAddr address, const ResourcePtr& source, u64 offset, u64 size);
    Tegra::Engines::Maxwell3D* maxwell3d{};
    std::unique_ptr<QueryCacheRuntimeImpl> impl;
};
struct QueryCacheParams {
    using RuntimeType = QueryCacheRuntime;
    static constexpr bool REQUIRE_GPU_COMPLETION = true;
};
using QueryCache = VideoCommon::QueryCacheBase<QueryCacheParams>;
} // namespace D3D12
