# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host test for the NXBOX_INPUT_SCRIPT parser (std-only header)."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CODE = r"""
#include <cassert>
#include "eden_uwp/input_script.h"
#include "eden_uwp/pad_style.h"
using namespace EdenXbox;
int main() {
    auto e = ParseInputScript("40.0:A:0.15, 45:plus:0.2,55:LSTICK_RIGHT:3.0,bad,x:A:1,5:B,6:A:0");
    assert(e.size() == 4);
    assert(e[0].name == "A" && e[0].start == 40.0 && e[0].hold == 0.15);
    assert(e[1].name == "PLUS" && e[1].start == 45 && e[1].hold == 0.2);
    assert(e[2].name == "LSTICK_RIGHT" && e[2].hold == 3.0);
    assert(e[3].name == "B" && e[3].hold == 0.15);
    assert(!ScriptEventActive(e[0], 39.9) && ScriptEventActive(e[0], 40.1) && !ScriptEventActive(e[0], 40.2));
    assert(ParseInputScript("").empty());
    assert(ChoosePadStyle(0) == PadChoice::None);
    assert(ChoosePadStyle(0x7) == PadChoice::Pro);
    assert(ChoosePadStyle(0x6) == PadChoice::JoyDual);
    assert(ChoosePadStyle(0x2) == PadChoice::Handheld);
    assert(ChoosePadStyle(0x38) == PadChoice::None);
    assert(ChoosePadStyle(0x7, PrefersJoycons(0x010003F003A34000ULL)) == PadChoice::JoyDual);
    assert(ChoosePadStyle(0x7, PrefersJoycons(0x0100187003A36000ULL)) == PadChoice::JoyDual);
    assert(ChoosePadStyle(0x3, true) == PadChoice::Pro);
    assert(ChoosePadStyle(0x7, PrefersJoycons(0x0100000000010000ULL)) == PadChoice::Pro);
}
"""


class InputScriptTests(unittest.TestCase):
    def test_parser(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-script-") as d:
            src, binary = Path(d) / "t.cpp", Path(d) / "t"
            src.write_text(CODE)
            subprocess.run(
                ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"), str(src), "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_remote_right_stick_keys_reach_the_guest(self):
        # Device Portal sends both sticks as gamepad virtual keys (0xD3-0xDA); the right stick
        # used to be dropped, so camera prompts could never be answered from a script.
        pad = (ROOT / "src/eden_uwp/gamepad.h").read_text()
        for key, bit in [("0xD7", "KeyRightStickUp"), ("0xD8", "KeyRightStickDown"),
                         ("0xD9", "KeyRightStickRight"), ("0xDA", "KeyRightStickLeft")]:
            self.assertIn(f"case {key}:\n            return {bit};", pad)
        self.assertIn("key_axis(KeyRightStickRight, KeyRightStickLeft, reading.RightThumbstickX)", pad)
        self.assertIn("key_axis(KeyRightStickUp, KeyRightStickDown, reading.RightThumbstickY)", pad)


if __name__ == "__main__":
    unittest.main()
