// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace EdenXbox {
enum class PadChoice { None, Pro, JoyDual, Handheld };

/// Picks the controller type the Xbox pad presents for a game's supported NpadStyleSet bits
/// (bit 0 fullkey/Pro, bit 1 handheld, bit 2 joycon dual): Pro if allowed, else JoyDual, else
/// Handheld. None when the game allows none of them (or has not set a style set yet).
/// Pokemon Let's Go Pikachu/Eevee list Pro in their style set but only accept Joy-Cons on the
/// "press a button on the controller" screen, so they would wait there forever.
inline bool PrefersJoycons(std::uint64_t program_id) {
    return program_id == 0x010003F003A34000ULL || program_id == 0x0100187003A36000ULL;
}

inline PadChoice ChoosePadStyle(std::uint32_t supported_raw, bool prefer_joycons = false) {
    if (prefer_joycons && (supported_raw & (1u << 2)))
        return PadChoice::JoyDual;
    if (supported_raw & (1u << 0))
        return PadChoice::Pro;
    if (supported_raw & (1u << 2))
        return PadChoice::JoyDual;
    if (supported_raw & (1u << 1))
        return PadChoice::Handheld;
    return PadChoice::None;
}
} // namespace EdenXbox
