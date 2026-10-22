// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>
#include <vector>

namespace D3D12::Present {
// Wall timestamps of real guest presents only. Do not subtract presentation waits:
// that would estimate producer capacity rather than delivered native frame rate.
class NativeFrameGenerationGate {
public:
    void ObservePresent(double timestamp, bool real = true) {
        if (!real)
            return;
        if (!std::isfinite(timestamp) || (have_previous && timestamp <= previous)) {
            Reset();
            return;
        }
        if (have_previous && timestamp - previous > 3.0)
            Reset(); // Loading/suspension beyond the floor window must requalify.
        previous = timestamp;
        have_previous = true;
        presents.push_back(timestamp);
        // Keep the endpoint preceding the three-second boundary. Generated
        // presents never enter this history or either floor measurement.
        while (presents.size() > 2 && timestamp - presents[1] >= 3.0)
            presents.pop_front();
        auto recent = presents.begin();
        while (recent + 1 != presents.end() && timestamp - *(recent + 1) >= 1.0)
            ++recent;
        const double duration = timestamp - *recent;
        native_fps = duration > 0 ? static_cast<double>(presents.end() - recent - 1) / duration : 0;
        // Closing has priority even while unqualified: the shorter recovery
        // window must not reopen against a still-failing three-second window.
        const bool below_floor =
            timestamp - presents.front() >= 3.0 - Epsilon && TrimmedRate(3.0) < 27.0 - Epsilon;
        if (below_floor) {
            qualified = false;
        } else if (!qualified && timestamp - presents.front() >= 2.0 - Epsilon &&
                   TrimmedRate(2.0) >= 28.0 - Epsilon) {
            qualified = true;
        }
    }
    double NativeFps() const {
        return native_fps;
    }
    unsigned Multiplier(double refresh) const {
        // Permission for at most one synthetic frame, not an unconditional 2x rate.
        // The vblank schedule gives ready real frames priority and caps output at 60.
        return qualified && native_fps < 50.0 - Epsilon && Interval(refresh) ? 2u : 0u;
    }
    unsigned Interval(double refresh) const {
        // Use the lower tolerance edge for 59.94/119.88, but never exceed 60 Hz.
        return refresh >= 119 && refresh <= 120 ? 2u : refresh >= 59 && refresh <= 60 ? 1u : 0u;
    }

private:
    double TrimmedRate(double window) const {
        std::vector<double> intervals;
        for (auto endpoint = presents.begin() + 1; endpoint != presents.end(); ++endpoint) {
            if (presents.back() - *endpoint < window)
                intervals.push_back(*endpoint - *(endpoint - 1));
        }
        if (intervals.empty())
            return 0;
        // Vblank-quantized arrivals can alternate short/long intervals at a
        // stable native rate. A median jumps between those modes when sample
        // parity changes. Average the middle 80% instead, preserving both modes
        // while rejecting isolated hitches and short bursts symmetrically.
        std::sort(intervals.begin(), intervals.end());
        const auto trim = static_cast<std::vector<double>::difference_type>(intervals.size() / 10);
        const auto begin = intervals.begin() + trim;
        const auto end = intervals.end() - trim;
        return static_cast<double>(end - begin) / std::accumulate(begin, end, 0.0);
    }
    void Reset() {
        presents.clear();
        native_fps = 0;
        have_previous = qualified = false;
    }
    static constexpr double Epsilon = 1e-8;
    std::deque<double> presents;
    double previous{}, native_fps{};
    bool have_previous{}, qualified{};
};
// Reserve positive-interval DXGI presents, one output slot ahead of scanout.
// Preparing/submitting at the scanout deadline is too late: guest work may already
// occupy the GPU. DXGI owns the vblank wait; do not add a second CPU period.
// Synthetic submission has one slot of lookahead and no catch-up credit.
class FrameGenerationVblankSchedule {
public:
    void RealReady() {
        generated_available = false;
    }
    void Presented(double timestamp, double output_hz, bool real, bool candidate = false) {
        period = 1.0 / output_hz;
        next_slot = std::fmax(timestamp, next_slot) + period;
        generated_available = real && candidate;
    }
    bool Generate(double timestamp, bool real_ready) const {
        return generated_available && !real_ready && timestamp >= Deadline();
    }
    double Deadline() const {
        return generated_available ? next_slot - period : 0;
    }

private:
    double next_slot{}, period{};
    bool generated_available{};
};
} // namespace D3D12::Present
