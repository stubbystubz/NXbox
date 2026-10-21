// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>

// Renderer-neutral instrumentation. Only an enabled D3D12 recording thread installs
// a sink. Disabled scopes perform no clock reads, allocations, or locking.
namespace Common::GpuProfile {
using Tick = std::uint64_t;
inline Tick Now() {
    return static_cast<Tick>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
}
enum class Phase {
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
    QueueIdle
};
struct Sink {
    virtual ~Sink() = default;
    virtual std::size_t Enter(Phase phase) = 0;
    virtual void Leave(std::size_t previous) = 0;
};
inline thread_local Sink* sink{};
inline std::atomic<bool> enabled{};
// Recording-thread-only gate. Set alongside sink on that same thread; generic
// PhaseScope/WaitScope callers on other threads must continue to use the TLS sink.
inline thread_local bool recording_enabled{};
class RecordingScope {
public:
    explicit RecordingScope(Phase phase) {
        if (recording_enabled) [[unlikely]] {
            owner = sink;
            if (owner)
                previous = owner->Enter(phase);
        }
    }
    ~RecordingScope() {
        if (owner)
            owner->Leave(previous);
    }
    RecordingScope(const RecordingScope&) = delete;
    RecordingScope& operator=(const RecordingScope&) = delete;

private:
    Sink* owner{};
    std::size_t previous{};
};
class PhaseScope {
public:
    explicit PhaseScope(Phase phase, bool active = true) : owner{active ? sink : nullptr} {
        if (owner)
            previous = owner->Enter(phase);
    }
    ~PhaseScope() {
        if (owner)
            owner->Leave(previous);
    }
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;

private:
    Sink* owner{};
    std::size_t previous{};
};
// Detailed per-method accounting is opt-in: even sampled clocks leave TLS counters
// and scope bookkeeping on every register write. Coarse exclusive phases remain on.
inline bool MethodProfilingEnabled() {
    // Latch after launcher environment overrides have loaded, not at static init.
    static const bool method_profiling = [] {
        const char* value = std::getenv("NXBOX_GPU_METHOD_PROFILE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return method_profiling;
}
// Recording-thread method classes. Time one call in 64 to keep clock reads out
// of the per-register hot path; counts/units are exact, time is an estimate.
enum class MethodClass : std::size_t {
    DirtyFlags,
    ConstantBuffer,
    DrawState,
    QueryReport,
    MacroUpload,
    DrawDispatch,
    Count
};
inline constexpr std::array MethodNames{"dirty_flags",  "constant_buffer", "draw_state",
                                        "query_report", "macro_upload",    "draw_dispatch"};
static_assert(MethodNames.size() == static_cast<std::size_t>(MethodClass::Count));
struct MethodSample {
    Tick count{}, units{}, timed_calls{}, sampled_ns{}, skipped{};
};
using MethodWindow = std::array<MethodSample, static_cast<std::size_t>(MethodClass::Count)>;
inline thread_local MethodWindow method_window{};
inline thread_local std::array<Tick, static_cast<std::size_t>(MethodClass::Count)>
    method_sequence{};
class MethodScope {
public:
    explicit MethodScope(MethodClass category, Tick units = 1) {
        if (!recording_enabled || !MethodProfilingEnabled())
            return;
        const auto index = static_cast<std::size_t>(category);
        sample = &method_window[index];
        work_units = units;
        if ((method_sequence[index]++ & 63) == 0)
            start = Now();
    }
    ~MethodScope() {
        if (!sample)
            return;
        // Submit can report/reset a window inside a method. Attribute the whole
        // completed call to its completion window, keeping counts and time paired.
        ++sample->count;
        sample->units += work_units;
        sample->skipped += skipped ? 1 : 0;
        if (start) {
            ++sample->timed_calls;
            sample->sampled_ns += Now() - start;
        }
    }
    void Skip() {
        skipped = true;
    }
    MethodScope(const MethodScope&) = delete;
    MethodScope& operator=(const MethodScope&) = delete;

private:
    MethodSample* sample{};
    Tick start{}, work_units{};
    bool skipped{};
};
inline MethodWindow TakeMethodWindow() {
    const auto result = method_window;
    method_window = {};
    return result;
}
enum class Wait : std::size_t {
    SyncpointHost,
    SyncpointGuest,
    NvdrvEvent,
    RequestSyncOperation,
    RequestFlush,
    WaitIdle,
    TickGPU,
    GpuThreadCommand,
    GpuQueueIdle,
    DmaMemorySync,
    PullerAcquire,
    GuestFenceCompletion,
    ForcedDrain,
    Count
};
inline constexpr std::array WaitNames{
    "syncpoint_host", "syncpoint_guest", "nvdrv_event",    "request_sync_operation",
    "request_flush",  "wait_idle",       "tick_gpu",       "gpu_thread_command",
    "gpu_queue_idle", "dma_memory_sync", "puller_acquire", "guest_fence_completion",
    "forced_drain"};
static_assert(WaitNames.size() == static_cast<std::size_t>(Wait::Count));
struct Sample {
    Tick count{}, total_ns{}, max_ns{};
    void Add(Tick ns) {
        ++count;
        total_ns += ns;
        if (ns > max_ns)
            max_ns = ns;
    }
};
struct Window {
    std::array<Sample, static_cast<std::size_t>(Wait::Count)> waits{};
    Sample fence_queued, fence_execution, fence_total;
    Tick dropped_requests{}, pending_requests{};
};
inline std::mutex mutex;
inline Window window;
// Only submitted nvhost_gpu output fences are matched; unmatched internal
// increments are excluded. Bound storage even if a game abandons its fences.
inline std::map<std::uint64_t, Tick> requests;
inline void Record(Wait reason, Tick start) {
    if (!start)
        return;
    const auto elapsed = Now() - start;
    const std::lock_guard lock{mutex};
    window.waits[static_cast<std::size_t>(reason)].Add(elapsed);
}
class WaitScope {
public:
    explicit WaitScope(Wait reason_, bool waiting = true)
        : phase{reason_ == Wait::GpuQueueIdle ? Phase::QueueIdle : Phase::SyncWait, waiting},
          reason{reason_}, start{waiting && enabled.load(std::memory_order_relaxed) ? Now() : 0} {}
    ~WaitScope() {
        Record(reason, start);
    }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    // An empty producer queue is idle time, not a renderer synchronization stall.
    // Non-waiting operations must preserve their caller's phase as well as counters.
    PhaseScope phase;
    Wait reason;
    Tick start;
};
inline Tick Start() {
    return enabled.load(std::memory_order_relaxed) ? Now() : 0;
}
inline std::uint64_t FenceKey(std::uint32_t id, std::uint32_t value) {
    return (std::uint64_t{id} << 32) | value;
}
inline void RequestFence(std::uint32_t id, std::uint32_t value) {
    if (!enabled.load(std::memory_order_relaxed))
        return;
    const auto now = Now();
    const std::lock_guard lock{mutex};
    if (requests.size() >= 4096) {
        ++window.dropped_requests;
        return;
    }
    requests[FenceKey(id, value)] = now;
}
struct FenceLatency {
    Tick requested{}, processed{};
};
inline FenceLatency ProcessFence(std::uint32_t id, std::uint32_t value) {
    if (!enabled.load(std::memory_order_relaxed))
        return {};
    const auto now = Now();
    const std::lock_guard lock{mutex};
    const auto it = requests.find(FenceKey(id, value));
    if (it == requests.end())
        return {};
    const auto start = it->second;
    requests.erase(it);
    return {start, now};
}
inline void CompleteFence(FenceLatency latency) {
    if (!latency.requested)
        return;
    const auto now = Now();
    const std::lock_guard lock{mutex};
    // All three samples belong to the completion window, including cross-window fences.
    window.fence_queued.Add(latency.processed - latency.requested);
    window.fence_execution.Add(now - latency.processed);
    window.fence_total.Add(now - latency.requested);
}
inline Window TakeWindow() {
    const std::lock_guard lock{mutex};
    auto result = window;
    result.pending_requests = requests.size();
    window = {};
    return result;
}
inline void Reset() {
    method_window = {};
    method_sequence = {};
    const std::lock_guard lock{mutex};
    window = {};
    requests.clear();
}
} // namespace Common::GpuProfile
