// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace EdenXbox {
inline constexpr int ClampMaxPlayers(int value) {
    return std::clamp(value, 1, 4);
}
inline constexpr bool PlayerOneControls(std::size_t slot) {
    return slot == 0;
}

// Remove missing devices before filling holes. Enumeration reorder never moves a live pad.
template <typename Device>
class GamepadSlots {
public:
    void Update(const std::vector<Device>& devices, int max_players = 4) {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (i >= static_cast<std::size_t>(ClampMaxPlayers(max_players)) ||
                (slots[i] && std::find(devices.begin(), devices.end(), *slots[i]) == devices.end()))
                slots[i].reset();
        }
        for (const auto& device : devices) {
            if (std::any_of(slots.begin(), slots.end(),
                            [&](const auto& slot) { return slot && *slot == device; }))
                continue;
            for (int i = 0; i < ClampMaxPlayers(max_players); ++i) {
                if (!slots[i]) {
                    slots[i] = device;
                    break;
                }
            }
        }
    }
    const std::optional<Device>& operator[](std::size_t slot) const {
        return slots[slot];
    }

private:
    std::array<std::optional<Device>, 4> slots;
};
} // namespace EdenXbox
