// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace D3D12::Present {
// DXGI's last displayed present ID and its refresh index, not CPU Present returns.
// Present IDs can skip when the compositor drops frames: these deltas alone do
// not prove that every submitted image was scanned out.
class DisplayStatistics {
public:
    void Observe(std::uint32_t present, std::uint32_t refresh) {
        if (have_previous) {
            const std::uint32_t present_delta = present - previous_present;
            const std::uint32_t refresh_delta = refresh - previous_refresh;
            if (present_delta == 0 && refresh_delta == 0)
                return; // Repeated/stale sample is not another display event.
            if (present_delta == 0 || refresh_delta == 0 || present_delta > 0x7fffffffu ||
                refresh_delta > 0x7fffffffu) {
                Unavailable(); // Counter reset/disjoint, allowing normal UINT wrap.
            } else {
                presents += present_delta;
                refreshes += refresh_delta;
                ++samples;
            }
        }
        previous_present = present;
        previous_refresh = refresh;
        have_previous = true;
    }
    void Unavailable() {
        have_previous = false;
        ++errors;
    }
    bool Valid() const {
        return samples != 0 && errors == 0;
    }
    double FramesPerSecond(double seconds) const {
        return Valid() && seconds > 0 ? static_cast<double>(presents) / seconds : -1.0;
    }
    void ClearWindow() {
        presents = refreshes = samples = errors = 0;
    }
    std::uint64_t presents{}, refreshes{}, samples{}, errors{};

private:
    std::uint32_t previous_present{}, previous_refresh{};
    bool have_previous{};
};
} // namespace D3D12::Present
