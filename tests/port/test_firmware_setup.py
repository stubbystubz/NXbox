# SPDX-License-Identifier: GPL-3.0-or-later
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class FirmwareSetupTests(unittest.TestCase):
    def test_candidates_and_transitions(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-setup-") as directory:
            binary = Path(directory) / "firmware-setup"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Wconversion",
                    "-Werror",
                    "-pedantic",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/firmware_setup.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
