// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <string_view>

namespace D3D12::Present {
inline bool FrameGenerationEnabled(bool preference, const char* environment) {
    if (environment && std::string_view(environment) == "0")
        return false;
    if (environment && std::string_view(environment) == "1")
        return true;
    return preference;
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
    bool CanGenerate(bool history, double game_seconds, double mean_difference) const {
        return !failed && history && std::isfinite(game_seconds) && game_seconds > 1.0 / 55.0 &&
               game_seconds < 0.2 && std::isfinite(mean_difference) && mean_difference < 0.22;
    }
    // Only the first failure emits a diagnostic; never retry during this session.
    bool Fail() {
        const bool first = !failed;
        failed = true;
        return first;
    }
};
} // namespace D3D12::Present
