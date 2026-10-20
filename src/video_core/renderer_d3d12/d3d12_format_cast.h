// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace D3D12 {
// DXGI values, kept SDK-independent for host regression tests. Equal bit size
// does not imply a view cast or a texture-to-texture copy is legal.
constexpr std::uint32_t FormatFamily(std::uint32_t format) {
    for (const auto [first, last] : std::array<std::array<std::uint32_t, 2>, 22>{
             {{1, 4},   {5, 8},   {9, 14},  {15, 18}, {19, 22}, {23, 25}, {27, 32}, {33, 38},
              {39, 43}, {44, 47}, {48, 52}, {53, 59}, {60, 64}, {70, 72}, {73, 75}, {76, 78},
              {79, 81}, {82, 84}, {94, 96}, {97, 99}, {90, 91}, {92, 93}}}) {
        if (format >= first && format <= last)
            return first;
    }
    if (format == 87)
        return 90;
    if (format == 88)
        return 92;
    return format;
}
constexpr bool IsTypelessFormat(std::uint32_t format) {
    constexpr std::array<std::uint32_t, 22> typeless{1,  5,  9,  15, 19, 23, 27, 33, 39, 44, 48,
                                                     53, 60, 70, 73, 76, 79, 82, 90, 92, 94, 97};
    return std::find(typeless.begin(), typeless.end(), format) != typeless.end();
}
constexpr bool CompatibleViewFormat(std::uint32_t resource, std::uint32_t view,
                                    std::span<const std::uint32_t> castable = {}) {
    if (!resource || !view || IsTypelessFormat(view))
        return false;
    // A nonempty explicit list is authoritative, including same-family views.
    if (!castable.empty())
        return std::find(castable.begin(), castable.end(), view) != castable.end();
    return resource == view || (resource == FormatFamily(view) && view != FormatFamily(view));
}
} // namespace D3D12
