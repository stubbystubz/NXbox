// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace EdenXbox::Files {
// Root-relative, platform-independent paths. Never accept a Windows device/ADS path.
inline bool ValidName(std::wstring_view name) {
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ')
        return false;
    for (auto c : name)
        if (c < 32 || std::wstring_view(L"/\\:<>\"|?*").find(c) != std::wstring_view::npos)
            return false;
    auto stem = std::wstring(name.substr(0, name.find(L'.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), ::towupper);
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
         stem[3] >= L'1' && stem[3] <= L'9'))
        return false;
    return true;
}
inline std::optional<std::wstring> Normalize(std::wstring_view path) {
    if (!path.empty() && (path.front() == L'/' || path.front() == L'\\'))
        return std::nullopt;
    std::vector<std::wstring> parts;
    for (std::size_t begin = 0; begin < path.size();) {
        auto end = path.find_first_of(L"/\\", begin);
        if (end == std::wstring_view::npos)
            end = path.size();
        const auto part = path.substr(begin, end - begin);
        if (part == L"..") {
            if (parts.empty())
                return std::nullopt;
            parts.pop_back();
        } else if (!part.empty() && part != L".") {
            if (!ValidName(part))
                return std::nullopt;
            parts.emplace_back(part);
        }
        begin = end + 1;
    }
    std::wstring result;
    for (const auto& part : parts) {
        if (!result.empty())
            result += L'/';
        result += part;
    }
    return result;
}
inline std::wstring Join(std::wstring_view parent, std::wstring_view name) {
    return parent.empty() ? std::wstring(name) : std::wstring(parent) + L'/' + std::wstring(name);
}
inline std::wstring Fold(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), ::towlower);
    return value;
}
inline bool Within(std::wstring_view parent, std::wstring_view child) {
    const auto p = Fold(std::wstring(parent));
    const auto c = Fold(std::wstring(child));
    return p.empty() || c == p || (c.starts_with(p) && c.size() > p.size() && c[p.size()] == L'/');
}
struct Entry {
    std::wstring name;
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t modified = 0;
};
enum class Sort { Name, Size, Date };
inline void SortEntries(std::vector<Entry>& entries, Sort sort) {
    std::sort(entries.begin(), entries.end(), [sort](const auto& a, const auto& b) {
        if (a.directory != b.directory)
            return a.directory;
        if (sort == Sort::Size && a.size != b.size)
            return a.size > b.size;
        if (sort == Sort::Date && a.modified != b.modified)
            return a.modified > b.modified;
        const auto an = Fold(a.name), bn = Fold(b.name);
        return an == bn ? a.name < b.name : an < bn;
    });
}
// Units are supplied by the UI's translation table.
inline std::wstring SizeNumber(std::uint64_t bytes, unsigned& unit, bool comma = false) {
    double value = static_cast<double>(bytes);
    unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value;
    auto result = out.str();
    if (comma)
        std::replace(result.begin(), result.end(), L'.', L',');
    return result;
}
inline std::wstring ClashName(std::wstring_view name, bool directory, unsigned number) {
    auto dot = directory ? std::wstring_view::npos : name.find_last_of(L'.');
    if (dot == 0 || dot == std::wstring_view::npos)
        dot = name.size();
    return std::wstring(name.substr(0, dot)) + L" (" + std::to_wstring(number) + L")" +
           std::wstring(name.substr(dot));
}
struct Plan {
    std::wstring destination;
    bool replace = false;
    bool move = false;
};
template <typename Exists>
std::optional<Plan> PlanTransfer(unsigned source_root, std::wstring_view source,
                                 unsigned target_root, std::wstring_view target_folder,
                                 bool directory, bool move, bool overwrite, Exists exists) {
    const auto from = Normalize(source), folder = Normalize(target_folder);
    if (!from || from->empty() || !folder)
        return std::nullopt;
    const auto name = from->substr(from->find_last_of(L'/') + 1);
    auto destination = Join(*folder, name);
    if (source_root == target_root &&
        (Fold(*from) == Fold(destination) || (directory && Within(*from, destination))))
        return std::nullopt;
    const bool clash = exists(destination);
    if (clash && !overwrite) {
        for (unsigned n = 2;; ++n) {
            destination = Join(*folder, ClashName(name, directory, n));
            if (!exists(destination))
                break;
        }
    }
    // Replacing a directory would merge or destroy an unreviewed tree. Keep both instead.
    if (clash && overwrite && directory)
        return std::nullopt;
    return Plan{destination, clash && overwrite, move};
}
} // namespace EdenXbox::Files
