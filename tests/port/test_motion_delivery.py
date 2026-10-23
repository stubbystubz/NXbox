# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise Xbox mapping through the real engine, poller, converter, and HID filter."""

import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_motion_delivery():
    # Compile the production mapping verbatim without requiring WinRT on the host.
    gamepad = (ROOT / "src/eden_uwp/gamepad.h").read_text()
    mapping = re.search(r"Common::ParamPackage motion\{.*?;", gamepad, re.DOTALL)
    assert mapping is not None
    with tempfile.TemporaryDirectory(prefix="nxbox-motion-delivery-") as temporary:
        directory = Path(temporary)
        (directory / "motion_mapping.h").write_text(mapping.group())
        binary = directory / "motion_delivery"
        include_paths = [ROOT / "src", directory]
        # These are the same fmt/Boost headers used by the native host build.
        prefix = Path(os.environ.get("HOMEBREW_PREFIX", "/opt/homebrew"))
        if (prefix / "include").is_dir():
            include_paths.append(prefix / "include")
        sources = [
            "tests/port/motion_delivery.cpp",
            "src/input_common/input_engine.cpp",
            "src/input_common/input_poller.cpp",
            "src/hid_core/frontend/input_converter.cpp",
            "src/hid_core/frontend/motion_input.cpp",
            "src/common/param_package.cpp",
            "src/common/string_util.cpp",
            "src/common/uuid.cpp",
            "src/common/random.cpp",
        ]
        command = [
            "clang++",
            "-std=c++20",
            "-DFMT_HEADER_ONLY",
            "-include",
            "array",
            "-include",
            "algorithm",
            "-include",
            "cstddef",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-Wno-deprecated-declarations",
        ]
        for include_path in include_paths:
            command.extend(["-I", str(include_path)])
        # Shadowing and narrowing are errors in the new driver and production filter.
        subprocess.run(
            command
            + ["-Wshadow", "-Wconversion", "-fsyntax-only"]
            + [str(ROOT / sources[0]), str(ROOT / "src/hid_core/frontend/motion_input.cpp")],
            check=True,
        )
        command.extend(str(ROOT / source) for source in sources)
        command.extend(["-o", str(binary)])
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
