// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace Nxbox::SeamDiagnostics {
inline constexpr std::uint32_t PrecisePosition = 1u;
inline constexpr std::uint32_t NoConservativeRaster = 2u;
inline constexpr std::uint32_t DrawTextureTriangle = 4u;

// Shared by the emitter and renderer, including the serialized prewarm identity.
// Read once: change diagnostics before launching the process.
inline std::uint32_t Mode() {
    static const std::uint32_t mode = [] {
        const auto enabled = [](const char* name) {
            const char* value = std::getenv(name);
            return value && std::string_view{value} == "1";
        };
        return (enabled("NXBOX_HLSL_PRECISE_POSITION") ? PrecisePosition : 0u) |
               (enabled("NXBOX_D3D12_NO_CONSERVATIVE_RASTER") ? NoConservativeRaster : 0u) |
               (enabled("NXBOX_D3D12_DRAW_TEXTURE_TRIANGLE") ? DrawTextureTriangle : 0u);
    }();
    return mode;
}
} // namespace Nxbox::SeamDiagnostics
