// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mutex>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include "eden_uwp/gamepad_slots.h"

namespace EdenXbox {
inline winrt::Windows::Gaming::Input::GamepadReading ReadGamepad(
    const std::optional<winrt::Windows::Gaming::Input::Gamepad>& device) {
    if (device) {
        try {
            return device->GetCurrentReading();
        } catch (const winrt::hresult_error&) {
            // Removal can race a snapshot. Publish neutral input until the next poll.
        }
    }
    return {};
}

// The session and overlay share identity/order; snapshots can be read on either UI thread.
inline GamepadSlots<winrt::Windows::Gaming::Input::Gamepad> SnapshotGamepads(int max_players = 0) {
    static std::mutex mutex;
    static GamepadSlots<winrt::Windows::Gaming::Input::Gamepad> slots;
    std::scoped_lock lock{mutex};
    std::vector<winrt::Windows::Gaming::Input::Gamepad> devices;
    for (const auto& device : winrt::Windows::Gaming::Input::Gamepad::Gamepads())
        devices.push_back(device);
    static int limit = 4;
    if (max_players != 0)
        limit = ClampMaxPlayers(max_players);
    slots.Update(devices, limit);
    return slots;
}
} // namespace EdenXbox
