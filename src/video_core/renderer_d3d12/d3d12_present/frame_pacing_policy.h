// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace D3D12::Present {
inline bool FramePacingEnabled(bool preference, const char* environment) {
    if (environment && std::string_view(environment) == "0")
        return false;
    if (environment && std::string_view(environment) == "1")
        return true;
    return preference;
}
// Real arrivals only. Reject stalls immediately, and require a full stable window.
// Probe without pacing periodically so presentation backpressure cannot latch 30 fps.
struct FramePacingPolicy {
    static constexpr unsigned Window = 16;
    std::array<double, Window> intervals{};
    unsigned count{}, cursor{}, paced_frames{};
    bool previous_paced{};
    double median_seconds{};

    unsigned Observe(double seconds, bool bypass) {
        if (bypass || !std::isfinite(seconds) || seconds < 0.022 || seconds > 0.040) {
            count = cursor = paced_frames = 0;
            previous_paced = false;
            return 1;
        }
        if (!previous_paced) {
            intervals[cursor++ % Window] = seconds;
            count = std::min(count + 1, Window);
        }
        previous_paced = false;
        if (count < Window)
            return 1;
        auto sorted = intervals;
        std::sort(sorted.begin(), sorted.end());
        median_seconds = (sorted[Window / 2 - 1] + sorted[Window / 2]) * 0.5;
        // A bimodal or widely jittering producer should not acquire a false cadence.
        if (sorted.back() - sorted.front() > 0.006)
            return 1;
        if (++paced_frames >= 30) {
            paced_frames = 0;
            count = cursor = 0;
            return 1;
        }
        return 2;
    }
    void Completed(bool paced) {
        previous_paced = paced;
    }
};
// Injected callback keeps the exact disabled command path testable on any host.
// A rejected nonblocking present has not consumed the backbuffer. Retry that same
// real frame using the original interval/flags; never drop it or spin/sleep.
template <typename Submit, typename Status>
auto SubmitPaced(unsigned interval, Submit&& submit, Status busy, Status unsupported) {
    if (interval == 2) {
        const auto result = submit(2u, true);
        if (result != busy && result != unsupported)
            return result;
    }
    return submit(1u, false);
}
struct PresentGapSamples {
    std::array<double, 256> milliseconds{};
    unsigned count{}, cursor{};
    void Add(double seconds) {
        if (std::isfinite(seconds) && seconds > 0) {
            milliseconds[cursor++ % milliseconds.size()] = seconds * 1000;
            count = std::min(count + 1, static_cast<unsigned>(milliseconds.size()));
        }
    }
    double Percentile(unsigned percentile) const {
        if (!count)
            return 0;
        auto sorted = milliseconds;
        std::sort(sorted.begin(), sorted.begin() + count);
        return sorted[(count - 1) * percentile / 100];
    }
    void Clear() {
        count = cursor = 0;
    }
};
} // namespace D3D12::Present
