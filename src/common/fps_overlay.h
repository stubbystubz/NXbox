// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace Common::FpsOverlay {
inline std::atomic<bool> enabled{false};
inline std::atomic<bool> forced{false};
inline std::atomic<std::uint64_t> generation{0};
inline std::atomic<std::uint64_t> guest_presents{0};
inline std::atomic<std::uint64_t> displayed_presents{0};
inline std::atomic<bool> frame_generation{false};
inline std::atomic<unsigned> frame_generation_target{0};
inline void Presented() {
    if (enabled.load(std::memory_order_relaxed))
        displayed_presents.fetch_add(1, std::memory_order_relaxed);
}
inline bool EnvironmentForces(const char* value) {
    return value && std::string_view(value) == "1";
}
inline void SetPreference(bool preference) {
    const bool next = preference || forced.load(std::memory_order_relaxed);
    if (enabled.exchange(next, std::memory_order_relaxed) != next)
        generation.fetch_add(1, std::memory_order_relaxed);
}
inline void Initialize(bool preference, const char* environment) {
    forced.store(EnvironmentForces(environment), std::memory_order_relaxed);
    SetPreference(preference);
}
inline void GuestPresent() {
    if (enabled.load(std::memory_order_relaxed))
        guest_presents.fetch_add(1, std::memory_order_relaxed);
}
// The callback owns all clock reads, formatting, allocation and GPU work.
template <typename Draw>
void DrawIfEnabled(Draw&& draw) {
    if (enabled.load(std::memory_order_relaxed))
        draw();
}
struct Sample {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start{};
    std::uint64_t baseline{}, epoch{};
    bool started{};
    double fps{}, milliseconds{};
    void Update(Clock::time_point now, std::uint64_t count, std::uint64_t current_epoch) {
        if (!started || epoch != current_epoch) {
            start = now;
            baseline = count;
            epoch = current_epoch;
            started = true;
            fps = milliseconds = 0;
            return;
        }
        const double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed < 1.0)
            return;
        fps = static_cast<double>(count - baseline) / elapsed;
        milliseconds = fps > 0 ? 1000.0 / fps : 0;
        start = now;
        baseline = count;
    }
};
} // namespace Common::FpsOverlay
