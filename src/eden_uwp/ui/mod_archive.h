// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace EdenXbox::Ui {

// The archive formats the mod store can unpack (through libarchive): .zip (zip64 included),
// .7z and .rar (RAR4 and RAR5). `extension` is lower case with its dot, e.g. ".7z".
bool IsModArchiveExtension(std::string_view extension);

// Unpacks `archive_path` into `destination` (created if missing). The format is detected from the
// content, not the name. Refuses encrypted entries and names that would leave `destination`,
// stops past `max_bytes` of output, and checks `cancel` between blocks. Returns why it failed,
// or an empty string. Free of WinRT, so it builds and is tested off-target as well.
std::string ExtractModArchive(const std::filesystem::path& archive_path,
                              const std::filesystem::path& destination,
                              const std::atomic<bool>& cancel, std::uint64_t max_bytes);

} // namespace EdenXbox::Ui
