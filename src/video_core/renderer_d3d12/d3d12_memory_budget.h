// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace D3D12 {
struct TextureMemoryBudget {
    std::uint64_t expected = 320ULL << 20;
    std::uint64_t critical = 448ULL << 20;
};

// Xbox CPU, guest DRAM and GPU resources share the process commit limit. Subtract
// non-cache usage before choosing GC watermarks, leaving room for transient work.
// These are soft targets: live images and dirty readback retain the existing LRU rules.
constexpr TextureMemoryBudget TextureMemoryThresholds(std::uint64_t limit, std::uint64_t usage,
                                                      std::uint64_t cache, unsigned pressure = 0) {
    constexpr std::uint64_t reserve = 256ULL << 20;
    constexpr std::uint64_t hysteresis = 64ULL << 20;
    const auto extra_reserve = pressure >= 2 ? 256ULL << 20 : pressure == 1 ? 128ULL << 20 : 0;
    limit -= (std::min)(limit, std::uint64_t{extra_reserve});
    const auto other = usage - (std::min)(usage, cache);
    const auto available = limit - (std::min)(limit, other);
    const auto target = available - (std::min)(available, reserve);
    TextureMemoryBudget result;
    result.critical = (std::min)(result.critical, target);
    result.expected = (std::min)(result.expected, target - (std::min)(target, hysteresis));
    return result;
}
inline bool TextureMemoryPressureEnabled() {
#ifdef NXBOX_UWP
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_TEXTURE_MEMORY_PRESSURE");
        return !value || std::string_view{value} != "0";
    }();
    return enabled;
#else
    return false;
#endif
}
TextureMemoryBudget NativeTextureMemoryBudget(std::uint64_t cache) noexcept;

inline bool AstcMemoryBudgetEnabled() {
#ifdef NXBOX_UWP
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_ASTC_MEMORY_BUDGET");
        return !value || std::string_view{value} != "0";
    }();
    return enabled;
#else
    return false;
#endif
}
// Reserve both async ASTC input and expanded output until upload retirement.
// Bound the reservation before allocating either; a rejected job uses the sync path.
constexpr std::uint64_t DecodeMemoryBudget(std::uint64_t limit, std::uint64_t usage,
                                           unsigned pressure) {
    constexpr std::uint64_t headroom = 512ULL << 20;
    const auto available = limit - (std::min)(limit, usage);
    if (pressure >= 2 || available <= headroom)
        return 0;
    const auto ceiling = pressure == 1 ? 32ULL << 20 : 128ULL << 20;
    return (std::min)(std::uint64_t{ceiling}, (available - headroom) / 4);
}
constexpr bool AdmitDecode(std::uint64_t budget, std::uint64_t queued, std::uint64_t input,
                           std::uint64_t output) {
    return queued <= budget && input <= budget - queued && output <= budget - queued - input;
}
std::uint64_t NativeDecodeMemoryBudget() noexcept;
} // namespace D3D12
