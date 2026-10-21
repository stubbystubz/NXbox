// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>

namespace D3D12::Present {
inline constexpr unsigned MaxFrameSlots = 3;
inline bool FastEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_D3D12_PRESENT_FAST");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
inline unsigned FrameSlotCount() {
    return FastEnabled() ? MaxFrameSlots : 2;
}
} // namespace D3D12::Present
