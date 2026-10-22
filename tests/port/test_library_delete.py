# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable deletion boundaries and real PFS0 replacement decisions, plus UWP contracts."""

import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIBRARY = (ROOT / "src/eden_uwp/ui/library.cpp").read_text()
SCREEN = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()

DRIVER = r'''
#include <cassert>
#include <fstream>
#include <iterator>
#include "frontend_common/library_files.h"
namespace fs = std::filesystem;
using namespace LibraryFiles;
int main(int argc, char** argv) {
    assert(argc == 3);
    const std::vector<fs::path> roots{"/local/games", "/usb/NXbox/games"};
    assert(Allowed("/local/games/title/game.nsp", roots));
    assert(Allowed("/USB/nxbox/GAMES/game.nsp", roots));
    for (const auto& path : {"/local/saves/game.nsp", "/local/games-other/game.nsp",
                             "/local/games/../saves/game.nsp", "/usb/games/game.nsp",
                             "relative/game.nsp", "/local/games", "/local/games/file.nsp:stream"})
        assert(!Allowed(path, roots));
    assert(Sibling("/usb/NXbox/games/Game.nsp", "/usb/NXbox/games/game.nsz.partial"));
    assert(Sibling("/local/games/a.nsp", "/local/games/a.nsp.partial"));
    assert(!Sibling("/local/games/a.nsp", "/local/games/ab.nsp"));
    assert(!Sibling("/local/games/a.nsp", "/local/games/a.json"));
    assert(!Sibling("/local/games/a.nsp", "/local/saves/a.nsz"));
    assert(!Sibling("/local/games/a.nsp", "/local/games/a.nsp.partial.partial"));
    assert(Related(0x0100f2c0115b6000ull, 0x0100f2c0115b6800ull));
    assert(Related(0x0100f2c0115b6000ull, 0x0100f2c0115b7001ull));
    assert(!Related(0x0100f2c0115b6000ull, 0x0100f2c0115b8000ull));
    assert(!Related(0x0100f2c0115b6000ull, 0x0100f2c0115b6000ull));
    assert(ParseTitle("0100F2C0115B6000") == 0x0100f2c0115b6000ull);
    for (const auto* id : {"", "invalid", "0100F2C0115B600Z", "0100F2C0115B60000",
                           " 100F2C0115B6000", "0000000000000000"})
        assert(!ParseTitle(id));
    for (const std::uint64_t size : {8ull, 100ull}) {
        assert(!TruncatedPfs0(size, [](std::uint64_t, std::size_t) {
            return std::vector<unsigned char>{};
        }).has_value());
    }
    assert(TruncatedPfs0(8, [](std::uint64_t, std::size_t size) {
        return std::vector<unsigned char>(size);
    }) == std::optional<bool>{true});
    assert(FilenameTitle("Update [0100F2C0115B6800].nsp.partial") == 0x0100f2c0115b6800ull);
    assert(!FilenameTitle("Update [0100F2C0115B680Z].nsp"));
    assert(!FilenameTitle("Update 0100F2C0115B6800.nsp"));
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
        const auto inspect = [&](const auto& data) {
            return TruncatedPfs0(data.size(), [&](std::uint64_t offset, std::size_t size) {
                if (offset > data.size()) return std::vector<unsigned char>{};
                return std::vector<unsigned char>(data.begin() + offset,
                    data.begin() + std::min<std::uint64_t>(data.size(), offset + size));
            });
        };
        // Replacement is authorized only by a positively detected truncation.
        assert(inspect(bytes) == std::optional<bool>{i == 2});
        if (i == 1) {
            auto corrupt = bytes;
            corrupt[0] = 'X';
            assert(!inspect(corrupt).has_value());
            corrupt = bytes;
            for (int j = 16; j < 24; ++j) corrupt[j] = 0xff;
            assert(inspect(corrupt) == std::optional<bool>{true});
            corrupt.resize(20); // Header survives, entry table does not.
            assert(inspect(corrupt) == std::optional<bool>{true});
        }
    }
}
'''


class LibraryDeletion(unittest.TestCase):
    def test_portable_boundaries_and_real_truncated_pfs0(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "check.cpp"
            source.write_text(DRIVER)
            exe = directory / "check"
            names = b"program.nca\0tail.tik\0"
            header = struct.pack("<4sIII", b"PFS0", 2, len(names), 0)
            entries = struct.pack("<QQII", 0, 32, 0, 0) + struct.pack("<QQII", 32, 8, 12, 0)
            fixture = header + entries + names + b"x" * 40
            complete = directory / "complete.nsp"
            truncated = directory / "truncated.nsp"
            complete.write_bytes(fixture)
            truncated.write_bytes(fixture[:-4])
            subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"), str(source), "-o", str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe), str(complete), str(truncated)], check=True, capture_output=True)

    def test_delete_guard_precedes_mutation_and_broker_fallbacks(self):
        delete = LIBRARY.split("bool DeleteLibraryFiles(")[1].split("LibraryScan::LibraryScan")[0]
        self.assertLess(delete.index("SafeDeletePath(file.path, roots)"), delete.index("RemoveLibraryFile(file.path, reason)"))
        self.assertIn("if (!file.selected)", delete)
        self.assertIn("LIBRARY_DELETE path=", delete)
        remove = LIBRARY.split("bool RemoveLibraryFile(")[1].split("} // namespace")[0]
        self.assertLess(remove.index("fs::remove(path, ec)"), remove.index("DeleteFileFromAppW"))
        self.assertLess(remove.index("DeleteFileFromAppW"), remove.index("StorageFile::GetFileFromPathAsync"))
        self.assertIn("DeleteAsync(StorageDeleteOption::PermanentDelete)", remove)
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", LIBRARY)
        self.assertIn("FILE_ATTRIBUTE_DIRECTORY", LIBRARY)
        self.assertNotIn("remove_all", LIBRARY)
        self.assertIn("!sibling, sibling", LIBRARY)  # extras are optional and start unselected

    def test_replace_requires_positive_truncation_and_validates_temporary_first(self):
        convert = LIBRARY.split("std::string ConvertNsz(")[1].split("} // namespace")[0]
        self.assertLess(convert.index("ConvertNszToNsp"), convert.index("TruncatedPackage(vfs, partial)"))
        self.assertLess(convert.index("TruncatedPackage(vfs, nsp_path)"), convert.index("PublishDownload(partial, nsp_path, error)"))
        self.assertIn("NSZ_CONVERT replaced_truncated=", convert)
        self.assertIn("TruncatedPackage(probe, existing) != std::optional<bool>{true}", LIBRARY)
        load = LIBRARY.split("std::optional<GameEntry> LoadGame(")[1].split("std::string ConvertNsz(")[0]
        self.assertLess(load.index("if (cache_valid)"), load.index("TruncatedPackage(vfs, file)"))
        launch = SCREEN.split("void Launch(Clock::time_point now)")[1].split("void OpenResolution()")[0]
        self.assertLess(launch.index("game.incomplete"), launch.index("launching_ = true"))

    def test_ui_reviews_named_files_defaults_cancel_and_refreshes(self):
        self.assertIn("delete_confirm_ = false; // Cancel is the default", SCREEN)
        self.assertIn("if (file.optional)", SCREEN)
        self.assertIn("file.path.filename().wstring()", SCREEN)
        self.assertIn("const auto size = FormatSize(file.size)", SCREEN)
        adopt = SCREEN.split("void AdoptDelete(")[1].split("void OpenDelete()")[0]
        self.assertIn("StartScan(false);", adopt)
        opening = SCREEN.split("void OpenDelete()")[1].split("void HandleInput(")[0]
        self.assertLess(opening.index("files_->Busy()"), opening.index("delete_stage_ = 1"))
        self.assertIn("std::async(std::launch::async", SCREEN)
        self.assertIn("wait_for(std::chrono::seconds(0))", adopt)
        for lang in ("en", "pt_br"):
            strings = (ROOT / f"src/eden_uwp/ui/lang/{lang}.inc").read_text()
            for key in ("DeleteGame", "DeleteChoose", "DeleteConfirm", "DeleteFailed", "IncompleteFile"):
                self.assertIn("Text::" + key, strings)


if __name__ == "__main__":
    unittest.main()
