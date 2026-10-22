// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <filesystem>
#include "eden_uwp/files_policy.h"

namespace EdenXbox::Files {
struct Root {
    std::filesystem::path path;
    std::wstring label;
};
struct Location {
    Root root;
    std::wstring relative;
};
struct Progress {
    std::atomic<bool> cancel{false};
    std::atomic<std::uint64_t> done{0}, total{0};
};
std::vector<Root> Roots(const std::filesystem::path& local_state);
std::filesystem::path Resolve(const Location& location);
std::vector<Entry> List(const Location& location);
bool Exists(const Location& location);
bool Directory(const Location& location);
void MakeDirectory(const Location& location);
void Rename(const Location& source, const Location& destination, Progress& progress);
void Remove(const Location& location, Progress& progress);
void Transfer(const Location& source, const Location& destination, bool move, bool replace,
              Progress& progress);
} // namespace EdenXbox::Files
