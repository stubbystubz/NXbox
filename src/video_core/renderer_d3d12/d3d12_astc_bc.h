// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace D3D12 {
// Recompressed ASTC must not enter the Xbox native BC array/cube sampler path.
// Restrict new BC storage to ordinary 2D sampled assets; volumes also stay RGBA.
constexpr bool AstcBcEligible(bool is_2d, std::uint32_t width, std::uint32_t height,
                              std::uint32_t depth, std::int32_t layers, std::uint32_t samples) {
    // MakeTexture rounds BC base dimensions to blocks. That changes normalized UVs
    // and the entire mip geometry for a nonaligned ASTC extent; RGBA keeps it exact.
    return is_2d && width != 0 && height != 0 && width % 4 == 0 && height % 4 == 0 && depth == 1 &&
           layers == 1 && samples == 1;
}

inline std::string AstcBcClasses(std::uint32_t width, std::uint32_t height, std::int32_t levels,
                                 std::int32_t layers, std::string_view format,
                                 std::uint64_t gpu = 0) {
    std::string classes{format};
    classes += " size=" + std::to_string(width) + "x" + std::to_string(height);
    std::array<char, 16> address{};
    const auto converted = std::to_chars(address.data(), address.data() + address.size(), gpu, 16);
    classes += " gpu=";
    classes.append(address.data(), converted.ptr);
    classes += layers > 1 ? " array" : " single";
    if (layers > 1 && layers % 6 == 0 && width == height)
        classes += " cube";
    if (format.ends_with("SRGB"))
        classes += " srgb";
    // ASTC has no metadata distinguishing opacity from data carried in alpha.
    // This conservative class includes every alpha-capable ASTC image.
    classes += " alpha";
    for (std::int32_t level = 0; level < levels; ++level) {
        if (width % 4 || height % 4) {
            classes += " partial"; // Partial BC block, including the mip tail.
            break;
        }
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    return classes;
}

// Comma-separated, case-sensitive substrings, ORed. Empty tokens never match.
inline bool AstcBcSkip(std::string_view rule, std::string_view classes) {
    while (!rule.empty()) {
        const auto separator = rule.find(',');
        const auto token = rule.substr(0, separator);
        if (!token.empty() && (token == "all" || classes.find(token) != std::string_view::npos))
            return true;
        if (separator == std::string_view::npos)
            break;
        rule.remove_prefix(separator + 1);
    }
    return false;
}
} // namespace D3D12
