# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads go to the roomiest removable drive and never take the internal drive's last GiB."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class DownloadDriveTests(unittest.TestCase):
    def test_policy(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "download_drive"
            subprocess.run(
                [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"),
                 str(ROOT / "tests/port/download_drive.cpp"), "-o", str(binary)],
                check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_shared_download_uses_the_policy(self):
        shared = (ROOT / "src/eden_uwp/game_download.cpp").read_text()
        for source in (shared,):
            self.assertIn("DownloadDrive::Fits(free, total - have, OnInternalDrive(destination))",
                          source)
            self.assertIn("DownloadDrive::Pick(drives, total)", source)
            # The first-fit loop is gone: no `break` once a drive fits.
            self.assertNotIn("relocated = true", source)
            self.assertNotIn("selected = true", source)
        self.assertIn("file.root_name() == local.root_name()", shared)


if __name__ == "__main__":
    unittest.main()
