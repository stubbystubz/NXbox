# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared downloads and conversions retain brokered removable-drive publication."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UWP = ROOT / "src/eden_uwp"
DOWNLOAD = (UWP / "game_download.cpp").read_text()
LIBRARY = (UWP / "ui/library.cpp").read_text()
LIBRARY_SCREEN = (UWP / "ui/library_screen.cpp").read_text()
COMMON_FS = (ROOT / "src/common/fs/fs.cpp").read_text()


def body(source, signature):
    """The text between the braces of the function that starts at `signature`."""
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1 : end - 1]


def in_order(text, *needles):
    positions = [text.index(needle) for needle in needles]
    return positions == sorted(positions)


class FinishingADownloadOnRemovableStorage(unittest.TestCase):
    def test_publish_tries_plain_then_broker_win32_then_storage_file(self):
        publish = body(DOWNLOAD, "bool PublishDownload(")
        self.assertTrue(
            in_order(
                publish,
                "std::filesystem::rename(partial, complete, ec)",
                "MoveFileFromAppW(partial.c_str(), complete.c_str())",
                "StorageFile::GetFileFromPathAsync(partial.native())",
                "RenameAsync(complete.filename().native()",
            )
        )
        self.assertIn("NameCollisionOption::ReplaceExisting", publish)
        # A file already under the final name is cleared for the Win32 call, which never replaces.
        self.assertIn("FS::RemoveFile(complete)", publish)

    def test_every_step_reports_its_real_error(self):
        publish = body(DOWNLOAD, "bool PublishDownload(")
        self.assertIn("ec.message()", publish)
        self.assertIn("GetLastError()", publish)
        self.assertIn("HRESULT 0x{:08x}", publish)
        self.assertIn("failure.message()", publish)

    def test_a_complete_partial_on_the_other_drive_is_reused_not_downloaded_again(self):
        download = body(DOWNLOAD, "bool DownloadFile(")
        self.assertIn("drives.push_back({DownloadedSize(candidate), FreeSpace(folder)});", download)
        self.assertIn("have = drives[*pick].have;", download)
        self.assertTrue(
            in_order(download, "have = drives[*pick].have;", "if (have == total)", "Transfer(client"))
        # DownloadedSize goes through Common::FS, which has the broker fallback (std::filesystem
        # does not see files on the drive).
        size = body(DOWNLOAD, "std::uint64_t DownloadedSize(")
        self.assertIn("FS::IsFile(file)", size)
        self.assertIn("FS::GetSize(file)", size)

    def test_conversion_and_the_nsz_delete_keep_the_broker_fallback(self):
        convert = body(LIBRARY, "std::string ConvertNsz(")
        self.assertIn("Common::FS::RenameFile(partial, nsp_path)", convert)
        self.assertNotRegex(convert, r"\bfs::(rename|remove)\(")
        compressed = body(LIBRARY, "void LibraryScan::ConvertCompressed(")
        self.assertIn("Common::FS::RemoveFile(nsz)", compressed)
        self.assertNotRegex(compressed, r"\bfs::rename\(")
        rename = body(COMMON_FS, "bool RenameFile(")
        self.assertIn("MoveFileFromAppW", rename)
        remove = body(COMMON_FS, "bool RemoveFile(")
        self.assertIn("DeleteFileFromAppW", remove)


class LibraryFrameProgress(unittest.TestCase):
    def test_the_library_loop_draws_every_frame(self):
        run = body(LIBRARY_SCREEN, "std::string Run()")
        self.assertTrue(in_order(run, "Update(now);", "renderer_.BeginFrame();", "Draw(now);", "renderer_.EndFrame();"))
        self.assertNotRegex(run, r"if\s*\([^)]*\)\s*\{?\s*Draw\(now\)")

