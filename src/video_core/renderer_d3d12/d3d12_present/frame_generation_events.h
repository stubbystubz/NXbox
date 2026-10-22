// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace D3D12::Present {
enum class FrameGenerationEvent : std::size_t {
    Candidate,
    Gated,
    GateFloor,
    GateFast,
    GateRefresh,
    Timing,
    History,
    ResourcePressure,
    Difference,
    GpuBudget,
    Failure,
    CancelReal,
    CancelFence,
    CancelReady,
    CancelHidden,
    Submitted,
    DeadlineMiss,
    CancelBackpressure,
    Count
};
struct FrameGenerationEvents {
    static constexpr std::array<std::string_view,
                                static_cast<std::size_t>(FrameGenerationEvent::Count)>
        Names{"candidates",           "gated",
              "gated_floor_recovery", "gated_fast",
              "gated_refresh",        "rejected_timing",
              "rejected_history",     "rejected_resources",
              "rejected_difference",  "rejected_gpu_budget",
              "rejected_failure",     "cancel_real_arrival",
              "cancel_real_fence",    "cancel_real_ready",
              "cancel_hidden",        "submitted",
              "deadline_misses",      "cancel_backpressure"};
    std::array<std::uint64_t, Names.size()> counts{};
    double real_present_ms{}, generated_service_ms{}, fence_ms{}, late_ms{};
    double configure_ms{}, prepare_ms{};
    void Count(FrameGenerationEvent event) {
        ++counts[static_cast<std::size_t>(event)];
    }
};
} // namespace D3D12::Present
