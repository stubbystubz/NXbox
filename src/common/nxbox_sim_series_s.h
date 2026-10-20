// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace NxboxSimSeriesS {
struct Config {
    bool enabled{};
    double gpu_factor{3.0};
    double cpu_factor{1.05};
};

inline double ParseFactor(const char* value, double fallback, double maximum) {
    if (!value || !*value)
        return fallback;
    char* end{};
    const double factor = std::strtod(value, &end);
    return end != value && *end == '\0' && std::isfinite(factor) && factor >= 1.0 &&
                   factor <= maximum
               ? factor
               : fallback;
}

inline const Config& Settings() {
    // Read once before running guest code. Factor overrides alone never enable simulation.
    static const Config config = [] {
        Config result;
        const char* enabled = std::getenv("NXBOX_SIM_SERIES_S");
        result.enabled = enabled && std::strcmp(enabled, "1") == 0;
        if (result.enabled) {
            result.gpu_factor = ParseFactor(std::getenv("NXBOX_SIM_GPU_FACTOR"), 3.0, 10.0);
            result.cpu_factor = ParseFactor(std::getenv("NXBOX_SIM_CPU_FACTOR"), 1.05, 2.0);
        }
        return result;
    }();
    return config;
}

using Clock = std::chrono::steady_clock;
inline Clock::duration ExtraCpuTime(Clock::duration elapsed, double factor) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(elapsed) *
                                                       (factor - 1.0));
}

// Lives immediately around Jit::Run, so callbacks/compilation in that timeslice count too.
// Disabled mode performs no clock reads, spinning, allocation or synchronization.
class CpuTimeslice {
public:
    CpuTimeslice() : factor{Settings().enabled ? Settings().cpu_factor : 1.0} {
        if (factor > 1.0)
            start = Clock::now();
    }
    void Finish() const {
        if (factor <= 1.0)
            return;
        const auto end = Clock::now();
        const auto deadline = end + ExtraCpuTime(end - start, factor);
        while (Clock::now() < deadline)
            std::atomic_signal_fence(std::memory_order_seq_cst);
    }

private:
    double factor;
    Clock::time_point start{};
};
} // namespace NxboxSimSeriesS
