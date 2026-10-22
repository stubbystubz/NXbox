// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/gpu_thread_profile.h"
#include "common/maxwell_method_profile.h"
#include "common/nxbox_memory.h"
#include "video_core/renderer_d3d12/d3d12_flush_policy.h"
#include "video_core/renderer_d3d12/d3d12_format_cast.h"
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
#include "video_core/renderer_d3d12/d3d12_present/display_statistics.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_events.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_pacing_policy.h"
#include "video_core/renderer_d3d12/d3d12_recording_progress.h"

namespace D3D12 {
enum class FramePhase : size_t {
    DrawSetup,
    DescriptorWrites,
    Barriers,
    Pso,
    BufferUpload,
    TextureUpload,
    Submit,
    FenceWait,
    PipelineLookup,
    ShaderTranslation,
    DxcCompile,
    ExtentCopies,
    FeedbackCopies,
    IndexConversion,
    BufferCache,
    TextureCache,
    BufferDownload,
    TextureDownload,
    DrawDispatch,
    DrawMemory,
    GraphicsDescriptors,
    RenderTargets,
    AttachmentSetup,
    QueryState,
    GraphicsState,
    ResourceBindings,
    ImageViews,
    GraphicsBuffers,
    GeometryBuffers,
    StageBuffers,
    BufferValidation,
    FeedbackSetup,
    GeometrySetup,
    VertexCopies,
    DescriptorSetup,
    DrawRecording,
    DrawCleanup,
    DmaPusher,
    Maxwell3DMethods,
    MacroExecution,
    DmaEngine,
    InlineToMemory,
    ComputeDispatchSetup,
    Fermi2D,
    SyncWait,
    BufferCacheBind,
    TextureCacheUpdate,
    DescriptorAndPipeline,
    QueueIdle,
    TextureUploadDiagnostic,
    TextureRepack,
    BetweenDraws,
    IndexRead,
    IndexHash,
    IndexFlushCaching,
    IndexCoherence,
    IndexGpuModified,
    IndexRestartScan,
    UploadAllocation,
    UploadTransitions,
    UploadRecording,
    StagingCollect,
    UniformCopyLookup,
    Count
};
enum class FrameCount : size_t {
    Draws,
    Dispatches,
    Barriers,
    DescriptorCopies,
    DescriptorTableReuses,
    DescriptorWrites,
    RootCbvBinds,
    DescriptorCopyCalls,
    DescriptorWritesCbv,
    DescriptorWritesSrv,
    DescriptorWritesUav,
    DescriptorWritesSampler,
    PsoCompiles,
    Executes,
    PsoCacheMisses,
    PrewarmedPsoHits,
    GuestPipelineMisses,
    NativeIndexDraws,
    ShaderTranslations,
    DxcCompiles,
    DxcCacheMisses,
    NegativeCacheHits,
    DrawsSkippedPendingPso,
    DispatchesSkippedPendingPso,
    ExtentCopies,
    FeedbackCopies,
    IndexConversions,
    IndexConversionBytes,
    IndexConversionUint8,
    IndexConversionRestart,
    IndexConversionStripOrFan,
    IndexConversionProvoking,
    IndexConversionInline,
    IndexConversionCacheHits,
    BufferUploadBytes,
    BufferDownloadBytes,
    SkippedDraws,
    ForcedSubmitMemoryPressure,
    ForcedSubmitAllocationRetry,
    ForcedSubmitStagingPressure,
    ForcedSubmitDescriptorExhaustion,
    ForcedSubmitQueryBank,
    ForcedSubmitQueryReadback,
    ForcedSubmitBufferDownload,
    ForcedSubmitTextureDownload,
    ForcedSubmitSkippedDraw,
    ForcedSubmitGuestFlush,
    ForcedSubmitGuestFence,
    ForcedSubmitOther,
    VertexUploadBytes,
    IndexUploadBytes,
    UniformUploadBytes,
    StorageUploadBytes,
    TexelUploadBytes,
    OtherUploadBytes,
    TextureUploadBytes,
    StagingCopyBytes,
    GpuBufferCopyBytes,
    UniformCopyCacheHits,
    VertexRepackBytes,
    VertexPaddingBytes,
    ForcedSubmitFenceWait,
    ForcedSubmitWaitForIdle,
    ForcedSubmitFlushAll,
    ForcedSubmitBufferClear,
    ForcedSubmitLifecycle,
    ForcedSubmitPresent,
    RedundantStateSkips,
    CopyHazardBarriers,
    FeedbackDisjoint,
    FeedbackAvoided,
    FeedbackCopyBytes,
    FeedbackReused,
    GpuResolvedQueryReports,
    QueryReportFenceWaits,
    IndexValidatedCacheHits,
    IndexNativeRestartDraws,
    IndexReadBytes,
    IndexDownloads,
    IndexDownloadBytes,
    StagingRequests,
    StagingFastRequests,
    StagingCollections,
    UploadCopyCommands,
    VertexUploadCount,
    IndexUploadCount,
    UniformUploadCount,
    StorageUploadCount,
    TexelUploadCount,
    OtherUploadCount,
    UniformCopyCacheMisses,
    FermiBlitsAccelerated,
    FermiBlitsSoftware,
    FermiBlitAcceleratedBytes,
    FermiBlitSoftwareBytes,
    TextureDirectUploadBytes,
    TextureRepackBytes,
    TextureRefreshBytes,
    FermiDiscardUploadBytes,
    StagingNewResources,
    UniformSnapshotRequests,
    UniformSnapshotNewResources,
    ForcedSubmitFlushDraws,
    AttachmentMetadataReuses,
    QueryBatchDispatches,
    GpuDepthReinterpretBytes,
    CpuReinterpretBytes,
    Count
};
// Monotonic wall samples, independent of the host clock API, for exclusive phases.
struct FrameTiming {
    std::array<std::uint64_t, static_cast<size_t>(FramePhase::Count)> cpu{};
    // Residual recording-thread work (guest commands and housekeeping) outside
    // draw scopes. Nested upload/submit/wait scopes keep their own attribution.
    FramePhase active{FramePhase::BetweenDraws};
    std::uint64_t last{};
    void Switch(FramePhase phase, std::uint64_t now) {
        if (now >= last)
            cpu[static_cast<size_t>(active)] += now - last;
        last = now;
        active = phase;
    }
    void Reset(std::uint64_t now) {
        cpu.fill(0);
        last = now;
    }
};
// The producer supplies immutable, owned inputs. The worker never reads Maxwell
// registers, records commands or mutates the pipeline cache on the GPU thread.
template <class Key, class Value, class Hash = std::hash<Key>>
class AsyncPsoCache {
public:
    using Result = std::shared_ptr<Value>;
    using Compile = std::function<Result()>;
    using Report = std::function<void(std::string_view)>;
    explicit AsyncPsoCache(bool enabled_, Report report_)
        : enabled{enabled_}, report{std::move(report_)} {}
    ~AsyncPsoCache() {
        Stop();
    }
    AsyncPsoCache(const AsyncPsoCache&) = delete;
    AsyncPsoCache& operator=(const AsyncPsoCache&) = delete;

    // Null means skip this frame. A failed key is logged once and stays failed.
    // Disabled mode preserves synchronous exception/fallback policy at the caller.
    Result Request(const Key& key, Compile compile) {
        if (!enabled)
            return compile();
        const bool failed = worker_failed.load(std::memory_order_acquire);
        if (failed && !worker_failure_reported.test_and_set())
            ReportFailure(worker_error);
        std::unique_lock lock{mutex};
        if (stopping)
            return {};
        const auto found = entries.find(key);
        if (found != entries.end()) {
            auto& entry = *found->second;
            if (entry.failure && !entry.reported) {
                entry.reported = true;
                const auto failure = entry.failure;
                lock.unlock();
                ReportFailure(failure);
                return {};
            }
            return entry.result;
        }
        // Completed results remain usable after a worker infrastructure failure.
        // Never enqueue new work onto that stopped worker.
        if (failed)
            return {};
        // Bounded outstanding jobs: never turn a shader burst into unbounded RAM.
        if (pending >= MaxPending)
            return {};
        if (!worker.joinable()) {
            worker = std::thread{[this] {
                try {
                    Run();
                } catch (...) {
                    // Queue/wait failures also stay inside the thread boundary.
                    // Publish before the release store; reporting stays on the caller.
                    worker_error = std::current_exception();
                    worker_failed.store(true, std::memory_order_release);
                }
            }};
        }
        auto entry = std::make_shared<Entry>();
        jobs.push_back({entry, std::move(compile)});
        try {
            entries.emplace(key, entry);
        } catch (...) {
            jobs.pop_back();
            throw;
        }
        ++pending;
        wake.notify_one();
        return {};
    }
    bool HasRequest(const Key& key) {
        const std::lock_guard lock{mutex};
        return entries.contains(key);
    }
    // Call before destroying any captured compiler/device dependencies.
    void Stop() {
        {
            const std::lock_guard lock{mutex};
            stopping = true;
            jobs.clear();
        }
        wake.notify_one();
        if (worker.joinable())
            worker.join();
    }
    static constexpr size_t MaxPending = 128;

private:
    struct Entry {
        Result result;
        std::exception_ptr failure;
        bool reported{};
    };
    struct Job {
        std::shared_ptr<Entry> entry;
        Compile compile;
    };
    void ReportFailure(const std::exception_ptr& failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            report(error.what());
        } catch (...) {
            report("D3D12 async PSO compiler: non-standard exception");
        }
    }
    void Run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock{mutex};
                wake.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping)
                    return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            Result result;
            std::exception_ptr failure;
            try {
                result = job.compile();
                if (!result)
                    throw std::runtime_error("D3D12 async PSO compiler returned no pipeline");
            } catch (...) {
                // Preserve what() without allocating a string on the worker's
                // exception path (including compiler allocation failures).
                failure = std::current_exception();
            }
            {
                const std::lock_guard lock{mutex};
                job.entry->result = std::move(result);
                job.entry->failure = std::move(failure);
                --pending;
            }
        }
    }
    bool enabled{};
    Report report;
    std::mutex mutex;
    std::condition_variable wake;
    std::unordered_map<Key, std::shared_ptr<Entry>, Hash> entries;
    std::deque<Job> jobs;
    size_t pending{};
    bool stopping{};
    std::exception_ptr worker_error;
    std::atomic<bool> worker_failed{};
    std::atomic_flag worker_failure_reported = ATOMIC_FLAG_INIT;
    std::thread worker;
};
} // namespace D3D12

#ifdef _WIN32
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_recorded_state.h"
#include "video_core/renderer_d3d12/d3d12_state_tracker.h"

namespace D3D12 {
using Microsoft::WRL::ComPtr;
void Diagnostic(std::string_view message) noexcept;
bool BisectEnabled(const char* name) noexcept;
// NXBOX_D3D12_FLUSH_DRAWS, 0 when unset or not a positive number (see ParseFlushDraws).
std::uint32_t FlushDrawsKnob() noexcept;
bool DxcStrictEnabled() noexcept;
bool AsyncPsoEnabled() noexcept;
void Check(HRESULT result, const char* operation, ID3D12Device* device = nullptr);
void CheckPipeline(HRESULT result, const char* operation, ID3D12Device* device, u64 hash);
// NXBOX_D3D12_SUBMIT_TRACE=1 only: call right after a D3D12 call that returns nothing (view and
// descriptor creation). The Xbox runtime removes the device on an invalid call instead of failing
// it, so the first call after which the device is gone is logged once with its arguments.
void TraceApiCall(ID3D12Device* device, const char* call, ID3D12Resource* resource,
                  const void* desc = nullptr, size_t desc_size = 0) noexcept;
struct MemoryBudget {
    std::atomic<u64> used{};
    static constexpr u64 Limit = 512ULL << 20;
};
struct Resource {
    ComPtr<ID3D12Resource> handle;
    // Resource handles/descriptions are immutable. The recording thread warms this
    // inline cache once, including resources obtained outside CreateResource (swapchain).
    const D3D12_RESOURCE_DESC& Description() const {
        if (!description)
            description = handle->GetDesc();
        return *description;
    }
    mutable std::optional<D3D12_RESOURCE_DESC> description;
    std::vector<std::uint32_t> castable_formats;
    bool SupportsViewFormat(DXGI_FORMAT format) const {
        return CompatibleViewFormat(Description().Format, format, castable_formats);
    }
    D3D12_RESOURCE_STATES state{};
    // Empty means uniform; otherwise state is superseded by per-subresource states.
    std::vector<D3D12_RESOURCE_STATES> sub_states;
    D3D12_HEAP_TYPE heap{};
    std::span<u8> mapped;
    std::shared_ptr<MemoryBudget> budget;
    u64 allocation_size{};
    std::optional<NxboxMemory::Gauge> memory_gauge;
    std::optional<NxboxMemory::Gauge> format_gauge;
    u64 content_version{};
    // Only the recording thread reads/writes this stamp; batches own the actual lease.
    u64 retained_generation{};
    // Recording tick of the last copy into this resource; copy hazards span command lists.
    u64 last_copy_tick{};
    ~Resource();
};
using ResourcePtr = std::shared_ptr<Resource>;
struct StagingBufferRef {
    ResourcePtr buffer;
    size_t offset{};
    std::span<u8> mapped_span;
};
// Phase intervals are exclusive wall estimates (nested scopes suspend the parent).
// Queue idle and fence waits are reported separately from the CPU-work estimates.
class FrameProfile : public Common::GpuProfile::Sink,
                     public Common::MaxwellMethodProfile::Collector {
public:
    class Scope {
    public:
        Scope(FrameProfile& profile_, FramePhase phase) {
            if (profile_.Enabled()) [[unlikely]]
                Begin(profile_, phase);
        }
        ~Scope() {
            if (profile) [[unlikely]]
                End();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        void Begin(FrameProfile& profile_, FramePhase phase);
        void End();
        FrameProfile* profile{};
        FramePhase previous{};
        u64 wall_start{};
        bool wait{};
    };
    bool Enabled() const {
        return enabled;
    }
    void Count(FrameCount counter, u64 amount = 1) {
        if (enabled) [[unlikely]]
            CountEnabled(counter, amount);
    }
    // Diagnostic: histogram of transition barriers by before/after state and resource kind.
    void NoteTransition(u32 before, u32 after, bool buffer);
    void NoteWait(std::string_view site, u64 wall);
    void EndFrame(bool presented = true);
    Present::PresentGapSamples present_gaps, present_call_gaps;
    double native_fps{};
    u64 output_presents{}, real_present_calls{}, generated_present_calls{};
    Present::DisplayStatistics display_statistics;
    Present::FrameGenerationEvents frame_generation;
    bool frame_generation_gated{true};
    std::function<std::string()> guest_visibility;
    void BindRecordingThread(bool rebind = false);
    std::size_t Enter(Common::GpuProfile::Phase phase) override;
    void Leave(std::size_t previous) override;
    // GPU execution time of submitted command lists, in 100 ns, added on completion.
    std::atomic<u64> gpu_busy{};
    void BindRecorder(void* handle) {
        recorder_thread = handle;
    }
    u64 TakeRecorderCpu();
    Common::GpuProfile::Tick ReadTicks() override;
    u64 recorder_waits{}, recorder_wait_ns{};

private:
    void* recorder_thread{};
    u64 recorder_initial_cpu{};
    void CountEnabled(FrameCount counter, u64 amount);
    bool OnOwner();
    void Switch(FramePhase phase);
    bool method_enabled{Common::MaxwellMethodProfile::Enabled()};
    bool enabled{method_enabled || BisectEnabled("NXBOX_D3D12_FRAME_PROFILE")};
    u64 method_clock_base{};
    std::atomic<DWORD> owner{};
    FrameTiming timing;
    std::array<u64, static_cast<size_t>(FrameCount::Count)> counts{};
    std::unordered_map<u64, u64> transitions;
    std::unordered_map<std::string_view, std::pair<u64, u64>> waits;
    u64 initial_cpu{}, start_wall{}, fence_wall{}, frames{};
};

} // namespace D3D12
#include "video_core/renderer_d3d12/d3d12_commands.h"
namespace D3D12 {

// One direct queue, bounded in-flight allocators. Every referenced resource is retained
// until completion, including resources evicted from the shared caches.
class SimSeriesS;
class Context {
public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    CommandFacade* Commands();
    void SimulateSeriesSFrame();
    void GpuPassFrame();
    GpuPassProfile* PassProfile() {
        return pass_profile.get();
    }
    void Acknowledge(u64 tick);
    // Every recording path outside the cached guest draw must invalidate before
    // recording. Resource barriers and predication do not change cached state.
    // The state tracker's caches hold values computed from registers, not command-list state:
    // re-emission is RecordState's job, so a new recording does not recompute them.
    void InvalidateRecordedState() {
        ++recording_generation;
    }
    StateTracker& GraphicsStateTracker() {
        return graphics_state_tracker;
    }
    RecordedDrawState& DrawState() {
        // Diagnostic fallback for comparing validity-only and full state resets.
        static const bool full_state_reset = BisectEnabled("NXBOX_D3D12_FULL_STATE_RESET");
        recorded_state.Refresh(recording_generation, full_state_reset);
        return recorded_state;
    }
    bool RecordState(bool changed) {
        if (!changed)
            profile.Count(FrameCount::RedundantStateSkips);
        return changed;
    }
    // Recording-thread producers must materialize deferred work before sealing a list.
    std::function<void()> before_submit;
    std::function<void(const ResourcePtr&)> before_resource_use;
    u64 Submit(FrameCount reason = FrameCount::ForcedSubmitOther);
    // Between draws: submits the open list once it holds NXBOX_D3D12_FLUSH_DRAWS recorded draws.
    // Returns true when it did, so the caller can wake the fencing thread like any other submit.
    bool FlushDrawsIfDue();
    void MarkWork(std::string_view label);
    void Finish(FrameCount reason = FrameCount::ForcedSubmitOther);
    void ReportFailure(const std::exception& error) noexcept;
    void LogInfoQueue() noexcept;
    void Wait(u64 tick, std::string_view site = "other");
    bool IsFree(u64 tick) const;
    u64 CompletedTick() const;
    // Recording thread: include pending commands without forcing a queue submission.
    u64 PendingFenceTick() const {
        return recording ? CurrentTick() : LastSealedTick();
    }
    u64 LastSubmittedTick() const {
        return submitted.load(std::memory_order_acquire);
    }
    // True once the queue signals this tick. A failed Submit also counts, so waiters reach
    // WaitSubmitted and report the failure instead of waiting for a submission that cannot come.
    bool IsTickSubmitted(u64 tick) const {
        return LastSubmittedTick() >= tick || submit_failed.load(std::memory_order_acquire);
    }
    // Completion thread: never submit or touch command recording state.
    void WaitSubmitted(u64 tick, std::string_view site = "other");
    void Collect();
    u64 CurrentTick() const {
        return LastSealedTick() + 1;
    }
    void Transition(const ResourcePtr& resource, D3D12_RESOURCE_STATES state);
    // Color textures only: mip + layer * mip count (one plane; 3D has one layer).
    void TransitionSubresources(const ResourcePtr& resource, std::span<const u32> subresources,
                                D3D12_RESOURCE_STATES state);
    // Orders overlapping copy writes within the current command list.
    void BeforeCopyWrite(const ResourcePtr& resource, u64 offset, u64 size);
    // Copy write-after-write barriers recorded so far (diagnostics).
    u64 copy_hazard_count{};
    /// Orders every prior UAV write before later work on the GPU, without a CPU wait.
    void GlobalUavBarrier();
    // Explicit batch keeps raw Commands() users ordered; no hidden deferred barriers.
    void TransitionBatch(std::span<const std::pair<ResourcePtr, D3D12_RESOURCE_STATES>> uses);
    FrameProfile& Profile() {
        return profile;
    }
    void Retain(const ResourcePtr& resource);
    void AfterCompletion(std::function<void()> callback);
    ResourcePtr CreateResource(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap,
                               D3D12_RESOURCE_STATES state);
    void ValidateViewFormat(const ResourcePtr& resource, DXGI_FORMAT format,
                            const char* call) const;
    // Optional capture resources must fail instead of draining the render queue on pressure.
    ResourcePtr CreateResourceNoWait(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap,
                                     D3D12_RESOURCE_STATES state);
    ResourcePtr CreateBuffer(u64 size, D3D12_HEAP_TYPE heap);
    ID3D12Device* Device() const {
        return device.Get();
    }
    bool TypedUavLoadAdditionalFormats() const {
        return typed_uav_load_additional_formats;
    }
    ID3D12CommandQueue* Queue() const {
        return queue.Get();
    }
    u64 DeviceLocalMemory() const {
#ifdef NXBOX_UWP
        return MemoryBudget::Limit;
#else
        return device_local_memory;
#endif
    }
    u64 MemoryUsage() const {
        return budget->used;
    }

private:
    u64 LastSealedTick() const {
        return deferred ? sealed : LastSubmittedTick();
    }
    void InitializeLists();
    void StartRecorder();
    void RecorderBegin();
    void RecorderSubmit(u64 tick);
    u64 SealRecording(FrameCount reason);
    void CheckRecorder();
    bool deferred{};
    u64 sealed{};
    CommandFacade facade;
    std::unique_ptr<RecordDescriptors> record_descriptors;
    std::unique_ptr<RecordStream<>> stream;
    std::thread recorder;
    ComPtr<ID3D12GraphicsCommandList1> extended1;
    ComPtr<ID3D12GraphicsCommandList8> extended8;
    bool typed_uav_load_additional_formats{};
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    ComPtr<ID3D12Device10> casting_device;
#endif
    void CrossListCopyBarrier(Resource& resource, u64 tick, u64 offset, u64 size);
#ifndef NXBOX_UWP
    u64 device_local_memory{};
#endif
    FrameProfile profile;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::unique_ptr<SimSeriesS> sim_series_s;
    std::unique_ptr<GpuPassProfile> pass_profile;
    struct AllocatorSlot {
        ComPtr<ID3D12CommandAllocator> handle;
        u64 tick{};
    };
    // Games that fence often submit ~9 lists a frame (Kirby); eight slots made the recorder
    // wait ~15 ms for an allocator every other frame while the GPU sat mostly idle.
    std::array<AllocatorSlot, 16> allocators;
    // Profile-only timestamps bracketing each command list, one pair per allocator slot.
    ComPtr<ID3D12QueryHeap> gpu_time_heap;
    ResourcePtr gpu_time_readback;
    double gpu_ticks_to_100ns{};
    size_t allocator_index{};
    ComPtr<ID3D12GraphicsCommandList> commands;
    struct InfoCallback {
        ComPtr<ID3D12InfoQueue1> queue;
        DWORD cookie{};
        bool registered{};
        ~InfoCallback() {
            if (registered)
                queue->UnregisterMessageCallback(cookie);
        }
    } info_callback;
    u64 execute_sequence{};
    std::vector<std::string> work;
    struct CopyWriteRange {
        u64 offset;
        u64 size;
    };
    std::unordered_map<ID3D12Resource*, std::vector<CopyWriteRange>> copy_writes;
    // Used only after Commands/Retain have run completion callbacks. No callbacks
    // may run while a transition method is collecting or emitting these barriers.
    std::vector<D3D12_RESOURCE_BARRIER> transition_barriers;

    std::atomic<u64> submitted{};
    // Set when Submit throws, cleared by the next completed submission.
    std::atomic<bool> submit_failed{};
    RecordingProgress recorder_progress{submitted, submit_failed};
    bool recording{};
    u64 recording_generation{};
    DrawFlushCadence flush_cadence{FlushDrawsKnob()};
    StateTracker graphics_state_tracker;
    RecordedDrawState recorded_state;
    std::shared_ptr<MemoryBudget> budget = std::make_shared<MemoryBudget>();
    struct Batch {
        u64 tick;
        std::vector<std::function<void()>> callbacks;
        std::vector<ResourcePtr> resources;
    };
    std::recursive_mutex completion_mutex;
    std::deque<Batch> batches;
    void CompleteThrough(u64 tick);
    std::vector<ResourcePtr> retained;
    // Stamp of the open recording for Retain; unique per recording, unlike recording_generation
    // (which InvalidateRecordedState bumps many times inside one recording).
    u64 retention_generation{};
    std::vector<std::function<void()>> completion;
};
class StagingRing {
public:
    StagingRing(Context& context, D3D12_HEAP_TYPE heap, std::function<void()> release_leases = {})
        : context{context}, heap{heap}, release_leases{std::move(release_leases)} {}
    StagingBufferRef Request(size_t size);
    void Collect(bool trim = false);

private:
    struct Page {
        ResourcePtr resource;
        u64 cursor{};
        u64 tick{};
        u64 capacity{};
    };
    Context& context;
    D3D12_HEAP_TYPE heap;
    std::function<void()> release_leases;
    std::vector<Page> pages;
};
} // namespace D3D12

#endif // _WIN32
