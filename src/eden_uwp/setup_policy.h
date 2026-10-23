// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <set>
#include <vector>
#include "frontend_common/firmware_sources.h"

namespace EdenXbox::Setup {
enum class Row { Keys, Firmware, Games };
enum class Phase { Ready, Scanning, Confirm, Installing, Result, Picking, Done };
struct State {
    Phase phase = Phase::Ready;
    void Select() {
        if (phase == Phase::Ready || phase == Phase::Result)
            phase = Phase::Picking;
    }
    void Confirm() {
        if (phase == Phase::Ready || phase == Phase::Result || phase == Phase::Picking)
            phase = Phase::Confirm;
    }
    void Install() {
        if (phase == Phase::Confirm)
            phase = Phase::Installing;
    }
    void Finish() {
        if (phase == Phase::Installing || phase == Phase::Scanning)
            phase = Phase::Result;
    }
    void Back() {
        if (phase == Phase::Confirm || phase == Phase::Picking)
            phase = Phase::Ready;
    }
    bool Continue() {
        if (phase != Phase::Ready && phase != Phase::Result)
            return false;
        phase = Phase::Done;
        return true;
    }
};
inline std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
inline std::string PathText(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
}
inline bool IsKeys(const std::filesystem::path& path) {
    const auto name = Lower(PathText(path.filename()));
    return name == "prod.keys" || name == "title.keys";
}
struct Candidate {
    Row row;
    std::filesystem::path path;
};
// Bounded, read-only discovery. Each loose/chunked NCA set is grouped by its direct parent.
// Any ZIP would also match cheat and mod archives (library\cheats\titles.zip), so only ZIPs whose
// own name or folder says firmware are suggested; any ZIP can still be picked by hand.
inline bool LooksLikeFirmwareZip(const std::filesystem::path& folder, const std::string& name) {
    return name.find("firmware") != std::string::npos ||
           Lower(PathText(folder.filename())).find("firmware") != std::string::npos;
}

inline std::vector<Candidate> Scan(const FirmwareManager::Sources::IO& io,
                                   const std::vector<std::filesystem::path>& roots) {
    std::vector<Candidate> result;
    std::set<std::filesystem::path> seen;
    const auto walk = [&](auto&& self, const std::filesystem::path& path, unsigned depth) -> void {
        if (!seen.insert(path).second)
            return;
        try {
            auto entries = io.list(path);
            std::sort(entries.begin(), entries.end(),
                      [](const auto& a, const auto& b) { return a.path < b.path; });
            bool nca = false, games = false;
            for (const auto& entry : entries) {
                const auto name = Lower(PathText(entry.path.filename()));
                const auto ext = Lower(PathText(entry.path.extension()));
                if (entry.reparse || name.starts_with("._") || name == "__macosx" ||
                    name.starts_with(".firmware_extract") || name.starts_with("firmware_installed"))
                    continue;
                if (ext == ".nca")
                    nca = true;
                else if (!entry.directory && IsKeys(entry.path))
                    result.push_back({Row::Keys, entry.path});
                else if (!entry.directory &&
                         ((ext == ".zip" && LooksLikeFirmwareZip(path, name)) || name == "rawnand.bin" || name == "rawnand.bin.00" ||
                          (Lower(PathText(path.filename())) == "emmc" && name == "00")))
                    result.push_back({Row::Firmware, entry.path});
                else if (!entry.directory && (ext == ".nsp" || ext == ".xci" || ext == ".nsz"))
                    games = true;
                if (entry.directory && ext != ".nca" && depth < 8)
                    self(self, entry.path, depth + 1);
            }
            if (nca)
                result.push_back({Row::Firmware, path});
            if (games)
                result.push_back({Row::Games, path});
        } catch (...) { /* An inaccessible directory must not hide other drives. */
        }
    };
    for (const auto& root : roots)
        walk(walk, root, 0);
    return result;
}
} // namespace EdenXbox::Setup
