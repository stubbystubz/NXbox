// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>

namespace D3D12::Present {
// DXGI's refresh count and QPC sample can advance on different presents. A
// one-frame quotient therefore aliases 60 Hz to 30/40 Hz or overshoots 60.
// Measure across two seconds and confirm changes over a second whole window.
class DisplayRefreshEstimate {
public:
    void Reset() {
        *this = {};
    }
    void Observe(std::uint32_t refresh_count, double timestamp) {
        if (!std::isfinite(timestamp)) {
            Reset();
            return;
        }
        if (!have_anchor || timestamp < anchor_time || refresh_count < anchor_count) {
            anchor_time = timestamp;
            anchor_count = refresh_count;
            have_anchor = true;
            pending_hz = 0;
            return;
        }
        const double seconds = timestamp - anchor_time;
        if (seconds < 2.0)
            return;
        const double measured = static_cast<double>(refresh_count - anchor_count) / seconds;
        anchor_time = timestamp;
        anchor_count = refresh_count;
        if (measured < 23 || measured > 250) {
            pending_hz = 0;
            return;
        }
        // Absorb counter quantization at the supported nominal modes. Always
        // use the fractional rate, so estimation error cannot schedule above 60.
        const double candidate = std::abs(measured - 60.0) <= 1.0    ? 59.94
                                 : std::abs(measured - 120.0) <= 1.0 ? 119.88
                                                                     : measured;
        if (refresh_hz == 0 || std::abs(candidate - refresh_hz) < 1.0 ||
            (pending_hz > 0 && std::abs(candidate - pending_hz) < 1.0)) {
            refresh_hz = candidate;
            pending_hz = 0;
        } else {
            pending_hz = candidate;
        }
    }
    double Hertz() const {
        return refresh_hz;
    }

private:
    double anchor_time{}, refresh_hz{}, pending_hz{};
    std::uint32_t anchor_count{};
    bool have_anchor{};
};
} // namespace D3D12::Present
