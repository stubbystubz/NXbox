# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Games kept in LocalState\\games\\<folder>\\ must show up in the library."""

from pathlib import Path

UI = Path(__file__).resolve().parents[2] / "src/eden_uwp/ui"


def test_library_scan_enters_subfolders():
    scan = (UI / "library.cpp").read_text().split("std::vector<GameEntry> LibraryScan::Scan()")[1]
    games_loop = scan.split("ListExternalGames()")[0]
    assert "recursive_directory_iterator" in games_loop
    assert "skip_permission_denied" in games_loop
