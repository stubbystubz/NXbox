// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace D3D12::Present {
// Original 4x7 monospace bitmaps, top row in the least significant nibble.
constexpr std::uint32_t FpsGlyph(char character) {
    switch (character) {
    case '0':
        return 0x6999996;
    case '1':
        return 0xE444464;
    case '2':
        return 0xF124896;
    case '3':
        return 0x7886887;
    case '4':
        return 0x888F999;
    case '5':
        return 0x698711F;
    case '6':
        return 0x6997116;
    case '7':
        return 0x224488F;
    case '8':
        return 0x6996996;
    case '9':
        return 0x688E996;
    case 'F':
        return 0x111711F;
    case 'P':
        return 0x1117997;
    case 'S':
        return 0x788611E;
    case 'm':
        return 0xDDDD700;
    case 's':
        return 0x7861E00;
    case '.':
        return 0x6600000;
    case '-':
        return 0x000F000;
    default:
        return 0;
    }
}
} // namespace D3D12::Present
