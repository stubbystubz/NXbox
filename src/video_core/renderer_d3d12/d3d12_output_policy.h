// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace D3D12 {
struct OutputExtent {
    std::uint32_t width, height;
};

constexpr bool IsSeriesS(std::string_view device_form) {
    return device_form == "Xbox Series S";
}

constexpr OutputExtent ChooseOutputExtent(OutputExtent requested, bool series_s,
                                          std::string_view environment = {}) {
    requested.width = std::max(1u, requested.width);
    requested.height = std::max(1u, requested.height);
    if (!series_s || environment == "0" || (requested.width <= 1920 && requested.height <= 1080))
        return requested;
    // Preserve the window aspect ratio, including portrait and ultrawide windows.
    if (std::uint64_t{requested.width} * 1080 >= std::uint64_t{requested.height} * 1920)
        return {1920, std::max(1u, static_cast<std::uint32_t>(std::uint64_t{requested.height} *
                                                              1920 / requested.width))};
    return {std::max(1u, static_cast<std::uint32_t>(std::uint64_t{requested.width} * 1080 /
                                                    requested.height)),
            1080};
}
} // namespace D3D12
