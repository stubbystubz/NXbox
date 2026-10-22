// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <initializer_list>

namespace D3D12::Present {
struct FrameGenerationTarget {
    unsigned output{}, multiplier{}, interval{};
    bool operator==(const FrameGenerationTarget&) const = default;
};
// Normalize fractional broadcast rates (59.94/119.88). Unknown output cannot lock.
inline FrameGenerationTarget SelectFrameGenerationTarget(double refresh, double real_fps) {
    if (!std::isfinite(refresh) || !std::isfinite(real_fps) || real_fps <= 0)
        return {};
    const unsigned hz = refresh >= 119 && refresh <= 121 ? 120
                        : refresh >= 59 && refresh <= 61 ? 60
                                                         : 0;
    if (!hz)
        return {};
    for (unsigned output : {120u, 60u, 40u, 30u}) {
        if (output > hz || hz % output)
            continue;
        // Prefer 2x when it can sustain the same output: fewer artifacts and lower
        // interpolation delay. Otherwise 4x wins over a lower output rate.
        for (unsigned multiplier : {2u, 4u}) {
            if (hz == 60 && multiplier == 4)
                continue; // 60/4 and 30/2 both need 15 real fps; 2x has fewer artifacts.
            if (real_fps >= double(output) / multiplier * 1.05)
                return {output, multiplier, hz / output};
        }
    }
    return {};
}
struct FrameGenerationTargetPolicy {
    FrameGenerationTarget current{}, promotion{};
    double elapsed{}, work{}, stable{};
    unsigned samples{};
    bool initialized{};
    // Producer time excludes our own presentation waits. Using wall cadence here
    // would feed the lock back into the estimator and prevent promotion forever.
    bool Observe(double producer_seconds, double refresh, double wall_seconds = 0) {
        if (!std::isfinite(producer_seconds) || producer_seconds <= 0 || producer_seconds > 0.2) {
            elapsed = work = stable = 0;
            samples = 0;
            return false;
        }
        elapsed +=
            wall_seconds > 0 && std::isfinite(wall_seconds) ? wall_seconds : producer_seconds;
        work += producer_seconds;
        ++samples;
        if (elapsed < 2.0)
            return false;
        const auto next = SelectFrameGenerationTarget(refresh, samples / work);
        const double window = elapsed;
        elapsed = work = 0;
        samples = 0;
        if (next == current) {
            stable = 0;
            promotion = {};
            return false;
        }
        // Refresh changes invalidate the old divisor immediately.
        const bool incompatible =
            current.output && std::abs(refresh - current.output * current.interval) > 1.0;
        if (!initialized || incompatible || next.output < current.output ||
            (next.output == current.output && next.multiplier > current.multiplier)) {
            initialized = true;
            current = next;
            stable = 0;
            promotion = {};
            return true;
        }
        if (!(promotion == next)) {
            promotion = next;
            stable = 0;
        }
        stable += window;
        if (stable < 5.0)
            return false;
        initialized = true;
        current = next;
        stable = 0;
        promotion = {};
        return true;
    }
};
} // namespace D3D12::Present
