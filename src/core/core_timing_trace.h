// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

// Bounded, opt-in flight recorder. Producers never wait for logging or allocation.
// Values are observations/publications, not proof that guest code consumed them.
namespace Core::Timing::Trace {
enum class Source : std::size_t {
    ClockTicks,
    GpuTicks,
    VsyncSignal,
    NativeTimestamp,
    QueryIssue,
    QueryPublish,
    SwapInterval,
    Count,
};
inline constexpr std::array Names{"clock_ticks",         "gpu_ticks_raw", "vsync_signal",
                                  "native_timestamp_ns", "query_issue",   "query_publish",
                                  "swap_interval"};
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_TIMING_TRACE");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}
inline std::uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Sample {
    std::uint64_t host_ns{}, thread{}, value{}, detail{}, id{};
};
struct Batch {
    Source source;
    std::uint64_t overwritten{}, contended{};
    std::vector<Sample> samples;
};
class Recorder {
public:
    static constexpr std::size_t Capacity = 1024;
    void Record(Source source, std::uint64_t value, std::uint64_t detail = 0,
                std::uint64_t id = 0) {
        auto& ring = rings[static_cast<std::size_t>(source)];
        std::unique_lock lock{ring.mutex, std::try_to_lock};
        if (!lock.owns_lock()) {
            ring.contended.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        ring.samples[ring.writes++ % Capacity] = {NowNs(), thread, value, detail, id};
    }
    Batch Take(Source source) {
        auto& ring = rings[static_cast<std::size_t>(source)];
        Batch result{source, 0, 0, {}};
        result.samples.reserve(Capacity);
        std::scoped_lock lock{ring.mutex};
        const auto count = std::min(ring.writes, Capacity);
        result.overwritten = ring.writes - count;
        result.contended = ring.contended.exchange(0, std::memory_order_relaxed);
        for (auto i = ring.writes - count; i < ring.writes; ++i) {
            result.samples.push_back(ring.samples[i % Capacity]);
        }
        ring.writes = 0;
        return result;
    }

private:
    struct Ring {
        std::mutex mutex;
        std::array<Sample, Capacity> samples{};
        std::size_t writes{};
        std::atomic<std::uint64_t> contended{};
    };
    std::array<Ring, static_cast<std::size_t>(Source::Count)> rings;
};
inline Recorder& FlightRecorder() {
    static Recorder recorder;
    return recorder;
}
inline void Record(Source source, std::uint64_t value, std::uint64_t detail = 0,
                   std::uint64_t id = 0) {
    if (Enabled()) {
        FlightRecorder().Record(source, value, detail, id);
    }
}
// Called by the diagnostic worker, never by a guest, compositor or timing event thread.
void Flush();
} // namespace Core::Timing::Trace
