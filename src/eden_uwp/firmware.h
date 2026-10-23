// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "eden_uwp/setup_policy.h"

namespace EdenXbox {
struct FirmwareStatus {
    std::string version;
    std::vector<std::filesystem::path> searched_folders;
    bool conversion_required = false;
    bool install_failed = false;
    bool home_available = false;
    bool mii_edit_available = false;
};
// Run on an MTA worker before the library, direct launch, or emulation starts.
void PrepareFirmware(const std::filesystem::path& local_state);
FirmwareStatus GetFirmwareStatus();
std::vector<Setup::Candidate> ScanSetupCandidates(const std::filesystem::path& local_state);
// Empty source preserves automatic startup discovery; manual installs use exactly one source.
void InstallFirmwareSource(const std::filesystem::path& local_state,
                           const std::filesystem::path& source,
                           const std::function<bool(size_t, size_t)>& progress);
bool InstallSetupKeys(const std::filesystem::path& source);
std::string SetupKeysStatus(const std::filesystem::path& local_state);
} // namespace EdenXbox
