// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace D3D12::Present {
// 1 is the legacy spelling of 2x. Invalid overrides preserve the preference.
inline unsigned FrameGenerationMode(unsigned preference, const char* environment) {
    if (environment) {
        const std::string_view value(environment);
        if (value == "auto" || value == "3")
            return 3;
        if (value == "0" || value == "off")
            return 0;
        if (value == "1" || value == "2")
            return 2;
        if (value == "4")
            return 4;
    }
    return preference == 3 ? 3 : preference == 4 ? 4 : preference ? 2 : 0;
}
inline unsigned DisplayFrameGenerationMode(unsigned requested, double refresh_hz) {
    return requested == 4 && (!std::isfinite(refresh_hz) || refresh_hz < 119.0) ? 2 : requested;
}
struct GeneratedFrameSchedule {
    unsigned multiplier{};
    unsigned Count() const {
        return multiplier > 1 ? multiplier - 1 : 0;
    }
    float Fraction(unsigned index) const {
        return static_cast<float>(index + 1) / static_cast<float>(multiplier);
    }
    double Timestamp(double previous, double current, unsigned index) const {
        return previous + (current - previous) * Fraction(index);
    }
};
inline bool FrameGenerationEnabled(bool preference, const char* environment) {
    return FrameGenerationMode(preference ? 2 : 0, environment) != 0;
}
inline bool FrameGenerationBudgetMissed(double milliseconds, double game_seconds, unsigned mode) {
    return mode > 1 && std::isfinite(milliseconds) && milliseconds > 0 &&
           milliseconds > game_seconds * 1000 / mode;
}
struct HistoryExtent {
    unsigned width, height;
};
inline HistoryExtent CheaperHistoryExtent(unsigned output_width, unsigned output_height,
                                          unsigned source_width, unsigned source_height) {
    return {source_width ? std::min(output_width, source_width) : output_width,
            source_height ? std::min(output_height, source_height) : output_height};
}
// Measure cadence only after a real-only presentation, so the extra vsync cannot
// make a fast game look like a 30 fps game. Probe once every 30 generated pairs.
struct FrameGenerationCadence {
    double measured_seconds{};
    unsigned generated_pairs{};
    bool previous_generated{};
    double Observe(double arrival_seconds) {
        if (!previous_generated || !std::isfinite(arrival_seconds) || arrival_seconds >= 0.2)
            measured_seconds = arrival_seconds;
        if (generated_pairs >= 30) {
            generated_pairs = 0;
            return 0; // The next arrival measures a real-only interval.
        }
        return measured_seconds;
    }
    void Completed(bool generated) {
        previous_generated = generated;
        if (generated)
            ++generated_pairs;
    }
};
struct FrameGenerationPolicy {
    bool failed{};
    bool locked{};
    bool CanGenerate(bool history, double game_seconds, double mean_difference) const {
        return !failed && history && std::isfinite(game_seconds) &&
               game_seconds > (locked ? 1.0 / 121.0 : 1.0 / 55.0) && game_seconds < 0.2 &&
               std::isfinite(mean_difference) && mean_difference < 0.22;
    }
    // Only the first failure emits a diagnostic; never retry during this session.
    bool Fail() {
        const bool first = !failed;
        failed = true;
        return first;
    }
};
} // namespace D3D12::Present
