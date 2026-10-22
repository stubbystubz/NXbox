// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <utility>
#include <vector>
#include <cstdint>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>

#include "common/settings.h"
#include "eden_uwp/applet_boot.h"
#include "eden_uwp/button_layout.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/gamepad_devices.h"
#include "eden_uwp/home_combo.h"
#include "eden_uwp/input_script.h"
#include "eden_uwp/pad_style.h"
#include "eden_uwp/recorder_policy.h"
#include "eden_uwp/stick_motion.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/input_engine.h"
#include "input_common/input_poller.h"

namespace EdenXbox {
// Engines the desktop frontends provide (keyboard, TAS, cameras...). The Xbox frontend has none of
// them; registering explicit null devices keeps behavior the same without logging an error for
// every device created.
template <typename Device>
class NullFactory final : public Common::Input::Factory<Device> {
public:
    std::unique_ptr<Device> Create(const Common::ParamPackage&) override {
        return std::make_unique<Device>();
    }
};

inline constexpr std::array UnsupportedEngines{"keyboard", "mouse",           "touch",
                                               "tas",      "virtual_gamepad", "virtual_amiibo",
                                               "camera",   "joycon",          "cemuhookudp"};

inline void RegisterUnsupportedEngines() {
    for (const auto* name : UnsupportedEngines) {
        Common::Input::RegisterInputFactory(
            name, std::make_shared<NullFactory<Common::Input::InputDevice>>());
        Common::Input::RegisterOutputFactory(
            name, std::make_shared<NullFactory<Common::Input::OutputDevice>>());
    }
}

inline void UnregisterUnsupportedEngines() {
    for (const auto* name : UnsupportedEngines) {
        Common::Input::UnregisterInputFactory(name);
        Common::Input::UnregisterOutputFactory(name);
    }
}

class XboxGamepad final : public InputCommon::InputEngine {
public:
    XboxGamepad() : InputEngine("nxbox") {
        for (std::size_t slot = 0; slot < 4; ++slot) {
            const PadIdentifier pad{.port = slot};
            PreSetController(pad);
            PreSetMotion(pad, 0);
            BasicMotion level{};
            level.accel_z = -1.0f;
            SetMotion(pad, 0, level);
            for (int i = 0; i < Settings::NativeButton::NumButtons; ++i)
                PreSetButton(pad, i);
            for (int i = 0; i < 4; ++i)
                PreSetAxis(pad, i);
        }
    }

    static void Configure(const std::shared_ptr<XboxGamepad>& engine) {
        Common::Input::RegisterInputFactory("nxbox",
                                            std::make_shared<InputCommon::InputFactory>(engine));
        Common::Input::RegisterOutputFactory("nxbox",
                                             std::make_shared<InputCommon::OutputFactory>(engine));
        for (std::size_t slot = 0; slot < 4; ++slot) {
            auto& player = Settings::values.players.GetValue()[slot];
            player.connected = slot == 0;
            player.controller_type = Settings::ControllerType::ProController;
            for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
                Common::ParamPackage parameters{
                    {"engine", "nxbox"}, {"port", std::to_string(slot)}, {"pad", "0"}};
                parameters.Set("button", i);
                player.buttons[i] = parameters.Serialize();
            }
            // Both motion mappings use this player's sensor, including styles selected by
            // AutoStyle. Keep HID's standard gyro deadband so its automatic bias estimate cannot
            // drift at rest.
            Common::ParamPackage motion{{"engine", "nxbox"},
                                        {"port", std::to_string(slot)},
                                        {"pad", "0"},
                                        {"motion", "0"},
                                        {"threshold", "1"}};
            for (auto& mapping : player.motions)
                mapping = motion.Serialize();
            for (int i = 0; i < 2; ++i) {
                Common::ParamPackage parameters{
                    {"engine", "nxbox"}, {"port", std::to_string(slot)}, {"pad", "0"}};
                parameters.Set("axis_x", i * 2);
                parameters.Set("axis_y", i * 2 + 1);
                parameters.Set("deadzone", 0.15f);
                player.analogs[i] = parameters.Serialize();
            }
        }
    }

    void SetButtonLayout(ButtonLayout layout) {
        button_layout = layout;
    }

    void SetMaxPlayers(int value) {
        max_players = ClampMaxPlayers(value);
    }

    void Poll(Core::HID::HIDCore& hid, bool suppress_guest = false) {
        const auto devices = SnapshotGamepads(max_players);
        for (std::size_t slot = 0; slot < 4; ++slot) {
            auto& controller = *hid.GetEmulatedControllerByIndex(slot);
            const bool present = devices[slot].has_value();
            if (devices[slot] != previous_devices[slot])
                stick_motions[slot] = {};
            if (present || slot == 0)
                AutoStyle(controller, hid.GetSupportedStyleTag(), slot);
            const bool allowed =
                slot < static_cast<std::size_t>(max_players) &&
                hid.IsSupportedPlayer(slot, controller.GetNpadStyleIndex() ==
                                                Core::HID::NpadStyleIndex::Handheld);
            // Keep the keyboard/remote-backed player 1 alive without a pad: disconnecting it
            // repeatedly reopens support applets in P5R/BotW. Guest ID/style limits still win.
            if (!allowed || (!present && slot != 0)) {
                controller.Disconnect();
                stick_motions[slot] = {};
            } else {
                if (!controller.IsControllerSupported())
                    controller.Disconnect();
                else if (!controller.IsConnected())
                    controller.Connect();
            }
            const auto reading = ReadGamepad(devices[slot]);
            PollPlayer(slot, reading, suppress_guest && PlayerOneControls(slot));
        }
        previous_devices = devices;
    }

    void PollPlayer(std::size_t slot, const winrt::Windows::Gaming::Input::GamepadReading& reading,
                    bool suppress_guest) {
        using namespace winrt::Windows::Gaming::Input;
        const PadIdentifier pad{.port = slot};
        const bool primary = PlayerOneControls(slot);
        // Remote keys describe physical Xbox buttons; scripts describe guest Switch buttons.
        const auto physical_keys = primary ? key_state.load(std::memory_order_acquire) : 0u;
        const auto scripted_keys = primary ? ScriptedKeys() : 0u;
        const auto keys = physical_keys | scripted_keys;
        const bool view = (reading.Buttons & GamepadButtons::View) != GamepadButtons::None ||
                          (keys & (1u << Settings::NativeButton::Minus)) != 0;
        const bool menu = (reading.Buttons & GamepadButtons::Menu) != GamepadButtons::None ||
                          (keys & (1u << Settings::NativeButton::Plus)) != 0;
        const auto now = std::chrono::steady_clock::now();
        const bool combo_blocked = primary && (home_combo.held || (view && menu));
        if (primary) {
            home_pressed = home_edge.Update(view, menu);
            home_combo_action = UpdateHomeCombo(home_combo, view, menu, now);
        }
        const bool right_click =
            (reading.Buttons & GamepadButtons::RightThumbstick) != GamepadButtons::None ||
            (keys & (1u << Settings::NativeButton::RStick)) != 0;
        if (primary && record_shortcut.Update(view, right_click, menu || combo_blocked) &&
            !suppress_guest) {
            Recording::toggles.fetch_add(1);
        }
        const auto motion = stick_motions[slot].Update(
            view, static_cast<float>(reading.RightThumbstickX),
            static_cast<float>(reading.RightThumbstickY), now, suppress_guest || combo_blocked);
        if (primary && view && right_click)
            stick_motions[slot].CancelTap();
        BasicMotion sensors{};
        sensors.accel_x = motion.accel[0];
        sensors.accel_y = motion.accel[1];
        sensors.accel_z = motion.accel[2];
        sensors.gyro_x = motion.gyro[0];
        sensors.gyro_y = motion.gyro[1];
        sensors.gyro_z = motion.gyro[2];
        sensors.delta_timestamp = motion.delta_timestamp;
        SetMotion(pad, 0, sensors);
        // Remap only guest input; frontend controls retain Xbox conventions.
        constexpr std::array mappings{GamepadButtons::A,
                                      GamepadButtons::B,
                                      GamepadButtons::X,
                                      GamepadButtons::Y,
                                      GamepadButtons::LeftThumbstick,
                                      GamepadButtons::RightThumbstick,
                                      GamepadButtons::LeftShoulder,
                                      GamepadButtons::RightShoulder,
                                      GamepadButtons::None,
                                      GamepadButtons::None,
                                      GamepadButtons::Menu,
                                      GamepadButtons::View,
                                      GamepadButtons::DPadLeft,
                                      GamepadButtons::DPadUp,
                                      GamepadButtons::DPadRight,
                                      GamepadButtons::DPadDown};
        for (int i = 0; i < static_cast<int>(mappings.size()); ++i) {
            const int physical = FaceButtonIndex(i, button_layout);
            bool pressed = (reading.Buttons & mappings[physical]) != GamepadButtons::None;
            if (i == Settings::NativeButton::ZL)
                pressed = reading.LeftTrigger > 0.5;
            if (i == Settings::NativeButton::ZR)
                pressed = reading.RightTrigger > 0.5;
            pressed = pressed || (physical_keys & (1u << physical)) != 0 ||
                      (scripted_keys & (1u << i)) != 0;
            if (i == Settings::NativeButton::Minus)
                pressed = motion.minus_pressed;
            if (i == Settings::NativeButton::RStick &&
                (motion.suppress_right_stick || (primary && record_shortcut.consume_stick)))
                pressed = false;
            if (combo_blocked &&
                (i == Settings::NativeButton::Minus || i == Settings::NativeButton::Plus)) {
                pressed = false;
            }
            SetButton(pad, i, pressed && !suppress_guest);
        }
        if (suppress_guest) {
            for (int i = 0; i < 4; ++i)
                SetAxis(pad, i, 0.0f);
            return;
        }
        const auto key_axis = [keys](int positive, int negative, double value) {
            if (keys & (1u << positive))
                return 1.0f;
            if (keys & (1u << negative))
                return -1.0f;
            return static_cast<float>(value);
        };
        SetAxis(pad, 0, key_axis(KeyLeftStickRight, KeyLeftStickLeft, reading.LeftThumbstickX));
        SetAxis(pad, 1, key_axis(KeyLeftStickUp, KeyLeftStickDown, reading.LeftThumbstickY));
        SetAxis(pad, 2,
                motion.suppress_right_stick
                    ? 0.0f
                    : key_axis(KeyRightStickRight, KeyRightStickLeft, reading.RightThumbstickX));
        SetAxis(pad, 3,
                motion.suppress_right_stick
                    ? 0.0f
                    : key_axis(KeyRightStickUp, KeyRightStickDown, reading.RightThumbstickY));
    }

    /// Gamepad virtual keys from the CoreWindow. Device Portal remote input only arrives this way,
    /// and returns true when the key belongs to the gamepad so the caller can mark it handled.
    bool OnKey(winrt::Windows::System::VirtualKey key, bool down) {
        const int bit = KeyBit(static_cast<int>(key));
        if (bit < 0)
            return false;
        // The Xbox raises these Gamepad* keys for every connected pad, and key_state drives
        // player 1, so with a second pad its buttons also moved player 1. Pads are read
        // directly per slot; key events only stand in for a remote when one pad (or none) is
        // connected. Still handled, so B and Menu never become system navigation.
        if (winrt::Windows::Gaming::Input::Gamepad::Gamepads().Size() > 1) {
            key_state.store(0, std::memory_order_release);
            return true;
        }
        key_seen.store(true, std::memory_order_release);
        if (down)
            key_state.fetch_or(1u << bit, std::memory_order_acq_rel);
        else
            key_state.fetch_and(~(1u << bit), std::memory_order_acq_rel);
        return true;
    }

    bool ShaderSkipPressed() {
        using namespace winrt::Windows::Gaming::Input;
        bool pressed =
            ((key_state.load(std::memory_order_acquire) | ScriptedKeys()) &
             ((1u << Settings::NativeButton::A) | (1u << Settings::NativeButton::B))) != 0;
        const auto devices = SnapshotGamepads(max_players);
        if (devices[0])
            pressed |= (ReadGamepad(devices[0]).Buttons &
                        (GamepadButtons::A | GamepadButtons::B)) != GamepadButtons::None;
        // Launch confirmation can still be held when preload starts. Require a fresh press.
        if (!pressed)
            shader_skip_armed = true;
        return pressed && shader_skip_armed;
    }

    HomeComboAction HomeCombo() const {
        return home_combo_action;
    }
    bool HomePressed() const {
        return home_pressed;
    }
    void ResetHomeButton() {
        home_edge.Reset();
        home_combo = {};
        home_combo.held = true;
        home_combo_action = HomeComboAction::None;
        home_pressed = false;
    }

private:
    Recording::Shortcut record_shortcut;
    bool shader_skip_armed = false;
    ButtonLayout button_layout = ButtonLayout::Name;
    GamepadSlots<winrt::Windows::Gaming::Input::Gamepad> previous_devices;
    int max_players = 4;
    std::array<StickMotion, 4> stick_motions;
    HomeComboState home_combo;
    HomeComboAction home_combo_action{};
    HomeButtonEdge home_edge;
    bool home_pressed{};
    /// NXBOX_PAD_AUTOSTYLE (default on): when the game changes its supported style set, present the
    /// pad as a type that set allows and signal a fresh connection, so games that look at the
    /// style only on a connect event (Super Mario 3D World's controller screen) see a valid pad.
    void AutoStyle(Core::HID::EmulatedController& controller, Core::HID::NpadStyleTag supported,
                   std::size_t slot) {
        static const bool enabled = [] {
            const char* value = std::getenv("NXBOX_PAD_AUTOSTYLE");
            return value == nullptr || value[0] != '0';
        }();
        const auto raw = static_cast<std::uint32_t>(supported.raw);
        if (!enabled || raw == last_style_raw[slot])
            return;
        last_style_raw[slot] = raw;
        using Core::HID::NpadStyleIndex;
        NpadStyleIndex style;
        // NXBOX_PAD_STYLE=pro|dual|handheld forces the preferred type (tests); it only applies
        // when the game's style set allows it.
        static const std::string forced = [] {
            const char* value = std::getenv("NXBOX_PAD_STYLE");
            return std::string(value != nullptr ? value : "");
        }();
        auto choice = ChoosePadStyle(raw);
        if (forced == "dual" && (raw & (1u << 2)))
            choice = PadChoice::JoyDual;
        else if (forced == "handheld" && (raw & (1u << 1)))
            choice = PadChoice::Handheld;
        switch (choice) {
        case PadChoice::Pro:
            style = NpadStyleIndex::Fullkey;
            break;
        case PadChoice::JoyDual:
            style = NpadStyleIndex::JoyconDual;
            break;
        case PadChoice::Handheld:
            if (slot != 0) {
                controller.Disconnect();
                return;
            }
            style = NpadStyleIndex::Handheld;
            break;
        default:
            return;
        }
        controller.Disconnect();
        controller.SetNpadStyleIndex(style);
        Diagnostic("PAD_AUTOSTYLE supported=0x" + std::to_string(raw) + " style=" +
                   std::to_string(static_cast<int>(style)) + " player=" + std::to_string(slot + 1));
    }

    std::array<std::uint32_t, 4> last_style_raw{};

    /// NXBOX_INPUT_SCRIPT events active now, as key_state bits; the clock starts at the first poll.
    std::uint32_t ScriptedKeys() {
        if (!script_loaded) {
            script_loaded = true;
            if (const char* text = std::getenv("NXBOX_INPUT_SCRIPT"); text && *text) {
                script = ParseInputScript(text);
                script_active.assign(script.size(), false);
                script_start = std::chrono::steady_clock::now();
                Diagnostic("INPUT_SCRIPT loaded events=" + std::to_string(script.size()));
            }
        }
        std::uint32_t bits = 0;
        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - script_start).count();
        for (std::size_t i = 0; i < script.size(); ++i) {
            const bool active = ScriptEventActive(script[i], t);
            const int bit = active || script_active[i] ? ScriptBit(script[i].name) : -1;
            if (active != static_cast<bool>(script_active[i])) {
                script_active[i] = active;
                Diagnostic("INPUT_SCRIPT t=" + std::to_string(t) + " btn=" + script[i].name +
                           (active ? " down" : " up") + (bit < 0 ? " (unknown)" : ""));
            }
            if (active && bit >= 0)
                bits |= 1u << bit;
        }
        return bits;
    }

    static int ScriptBit(const std::string& name) {
        namespace B = Settings::NativeButton;
        static const std::pair<const char*, int> table[] = {
            {"A", B::A},         {"B", B::B},           {"X", B::X},           {"Y", B::Y},
            {"L", B::L},         {"R", B::R},           {"ZL", B::ZL},         {"ZR", B::ZR},
            {"PLUS", B::Plus},   {"MINUS", B::Minus},   {"LSTICK", B::LStick}, {"RSTICK", B::RStick},
            {"DUP", B::DUp},     {"DDOWN", B::DDown},   {"DLEFT", B::DLeft},   {"DRIGHT", B::DRight},
            {"LSTICK_UP", KeyLeftStickUp},     {"LSTICK_DOWN", KeyLeftStickDown},
            {"LSTICK_RIGHT", KeyLeftStickRight}, {"LSTICK_LEFT", KeyLeftStickLeft},
            {"RSTICK_UP", KeyRightStickUp},     {"RSTICK_DOWN", KeyRightStickDown},
            {"RSTICK_RIGHT", KeyRightStickRight}, {"RSTICK_LEFT", KeyRightStickLeft}};
        for (const auto& [key, bit] : table)
            if (name == key)
                return bit;
        return -1;
    }

    std::vector<ScriptEvent> script;
    std::vector<char> script_active;
    std::chrono::steady_clock::time_point script_start{};
    bool script_loaded = false;

    // Bits 0-15 follow Settings::NativeButton; the rest are stick directions.
    static constexpr int KeyLeftStickUp = 16;
    static constexpr int KeyLeftStickDown = 17;
    static constexpr int KeyLeftStickRight = 18;
    static constexpr int KeyLeftStickLeft = 19;
    static constexpr int KeyRightStickUp = 20;
    static constexpr int KeyRightStickDown = 21;
    static constexpr int KeyRightStickRight = 22;
    static constexpr int KeyRightStickLeft = 23;

    static int KeyBit(int key) {
        switch (key) {
        case 0xC3:
            return Settings::NativeButton::A;
        case 0xC4:
            return Settings::NativeButton::B;
        case 0xC5:
            return Settings::NativeButton::X;
        case 0xC6:
            return Settings::NativeButton::Y;
        case 0xC7:
            return Settings::NativeButton::R;
        case 0xC8:
            return Settings::NativeButton::L;
        case 0xC9:
            return Settings::NativeButton::ZL;
        case 0xCA:
            return Settings::NativeButton::ZR;
        case 0xCB:
            return Settings::NativeButton::DUp;
        case 0xCC:
            return Settings::NativeButton::DDown;
        case 0xCD:
            return Settings::NativeButton::DLeft;
        case 0xCE:
            return Settings::NativeButton::DRight;
        case 0xCF:
            return Settings::NativeButton::Plus;
        case 0xD0:
            return Settings::NativeButton::Minus;
        case 0xD1:
            return Settings::NativeButton::LStick;
        case 0xD2:
            return Settings::NativeButton::RStick;
        case 0xD3:
            return KeyLeftStickUp;
        case 0xD4:
            return KeyLeftStickDown;
        case 0xD5:
            return KeyLeftStickRight;
        case 0xD6:
            return KeyLeftStickLeft;
        case 0xD7:
            return KeyRightStickUp;
        case 0xD8:
            return KeyRightStickDown;
        case 0xD9:
            return KeyRightStickRight;
        case 0xDA:
            return KeyRightStickLeft;
        default:
            return -1;
        }
    }

    std::atomic<std::uint32_t> key_state{0};
    std::atomic<bool> key_seen{false};
};
} // namespace EdenXbox
