# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable slot lifecycle, settings, and player-one input isolation."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <fstream>
#include <iterator>
#include "eden_uwp/gamepad_slots.h"
#include "eden_uwp/home_combo.h"
#include "eden_uwp/renderer_settings.h"
using namespace EdenXbox;
int main(int argc, char** argv) {
    assert(argc == 2);
    GamepadSlots<int> slots;
    slots.Update({10, 20, 30, 40, 50});
    for (int i = 0; i < 4; ++i) assert(slots[i] == (i + 1) * 10);
    slots.Update({40, 30, 20, 10});
    for (int i = 0; i < 4; ++i) assert(slots[i] == (i + 1) * 10);
    slots.Update({40, 20});
    assert(!slots[0] && slots[1] == 20 && !slots[2] && slots[3] == 40);
    slots.Update({40, 20, 50, 10});
    assert(slots[0] == 50 && slots[1] == 20 && slots[2] == 10 && slots[3] == 40);
    slots.Update({40, 20, 50, 10}, 2);
    assert(slots[0] == 50 && slots[1] == 20 && !slots[2] && !slots[3]);
    slots.Update({40, 20, 50, 10}, -10);
    assert(slots[0] == 50 && !slots[1] && !slots[2] && !slots[3]);
    slots.Update({40, 20, 50, 10}, 99);
    assert(slots[0] == 50 && slots[1] == 40 && slots[2] == 20 && slots[3] == 10);
    slots.Update({});
    for (int i = 0; i < 4; ++i) assert(!slots[i]);
    slots.Update({20, 10});
    assert(slots[0] == 20 && slots[1] == 10);
    const std::filesystem::path local(argv[1]);
    using namespace RendererSettings;
    assert(ReadMaxPlayers(local) == 4);
    { std::ofstream out(local / "eden_settings.txt");
      out << "# keep\nbutton_layout=position\nmax_players=1\n max_players = 2\r\n"; }
    assert(ReadMaxPlayers(local) == 2);
    for (int value : {-10, 1, 2, 3, 4, 99}) {
        assert(SaveMaxPlayers(local, value));
        assert(ReadMaxPlayers(local) == ClampMaxPlayers(value));
        assert(ReadButtonLayout(local) == ButtonLayout::Position);
        std::ifstream in(local / "eden_settings.txt");
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        assert(text.find("# keep\n") == 0);
        const auto key = text.find("max_players=");
        assert(key != std::string::npos && text.find("max_players=", key + 1) == std::string::npos);
    }
    assert(!SaveMaxPlayers(local / "missing", 2));
    assert(ParseMaxPlayers("bad") == 4 && ParseMaxPlayers("2x") == 4);
    assert(ParseMaxPlayers("9999999999999999999999999999") == 4);
    setenv("NXBOX_MAX_PLAYERS", "1", 1);
    assert(ReadMaxPlayers(local) == 1);
    setenv("NXBOX_MAX_PLAYERS", "3", 1);
    assert(ReadMaxPlayers(local) == 3);
    unsetenv("NXBOX_MAX_PLAYERS");
    HomeComboState home;
    const auto now = std::chrono::steady_clock::time_point{};
    for (int slot = 1; slot < 4; ++slot) {
        if (PlayerOneControls(slot)) UpdateHomeCombo(home, true, true, now);
    }
    assert(!home.held && !home.armed);
    assert(PlayerOneControls(0));
    assert(UpdateHomeCombo(home, true, true, now) == HomeComboAction::Prompt);
    for (int slot = 1; slot < 4; ++slot) {
        if (PlayerOneControls(slot)) UpdateHomeCombo(home, false, false, now);
    }
    assert(home.held);
    UpdateHomeCombo(home, false, false, now);
    assert(UpdateHomeCombo(home, true, true, now + std::chrono::milliseconds(100)) == HomeComboAction::GoHome);
}
"""


class LocalMultiplayerTests(unittest.TestCase):
    def test_slots_settings_and_primary_combo(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-local-players-") as directory:
            source = Path(directory) / "driver.cpp"
            binary = Path(directory) / "driver"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            env = os.environ.copy()
            env.pop("NXBOX_MAX_PLAYERS", None)
            subprocess.run([str(binary), directory], check=True, timeout=10, env=env)

    def test_runtime_routing(self):
        pad = (ROOT / "src/eden_uwp/gamepad.h").read_text()
        self.assertIn("const bool primary = PlayerOneControls(slot)", pad)
        self.assertIn("primary ? key_state.load", pad)
        self.assertIn("primary ? ScriptedKeys() : 0u", pad)
        self.assertIn("suppress_guest && PlayerOneControls(slot)", pad)
        self.assertIn("hid.IsSupportedPlayer(slot,", pad)
        self.assertIn("if (!controller.IsControllerSupported())", pad)
        self.assertIn("stick_motions[slot].Update", pad)
        ui = (ROOT / "src/eden_uwp/ui/input.cpp").read_text()
        self.assertIn("if (pads[0])", ui)
        self.assertNotIn("pads.GetAt", ui)
