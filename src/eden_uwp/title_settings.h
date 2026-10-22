// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eden_uwp/console_language.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace EdenXbox::TitleSettings {

// The numeric values of Eden's ResolutionSetup, not the multipliers themselves.
inline constexpr std::array<int, 6> kResolutionValues{1, 2, 3, 5, 6, 7};
inline constexpr int kDefaultResolution = 3;

struct Entry {
    std::string label;
    std::string value;
};

inline std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

inline std::optional<Entry> ParseLine(std::string_view line) {
    line = Trim(line);
    const auto equals = line.find('=');
    if (line.empty() || line.front() == '#' || equals == std::string_view::npos) {
        return std::nullopt;
    }
    const auto label = Trim(line.substr(0, equals));
    if (label.empty()) {
        return std::nullopt;
    }
    return Entry{std::string(label), std::string(Trim(line.substr(equals + 1)))};
}

inline std::optional<int> ParseResolution(std::string_view value) {
    value = Trim(value);
    // ResolutionSetup currently has thirteen values (0..12).
    int result = 0;
    if (value.empty() || value.size() > 2) {
        return std::nullopt;
    }
    for (const char digit : value) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        result = result * 10 + digit - '0';
    }
    return result <= 12 ? std::optional<int>(result) : std::nullopt;
}

inline int ReadResolution(std::istream& in, int fallback = kDefaultResolution) {
    std::string line;
    while (std::getline(in, line)) {
        const auto entry = ParseLine(line);
        if (entry && entry->label == "resolution_setup") {
            if (const auto value = ParseResolution(entry->value)) {
                fallback = *value;
            }
        }
    }
    return fallback;
}

inline std::optional<Settings::Language> ReadLanguage(std::istream& in) {
    std::optional<Settings::Language> result;
    for (std::string line; std::getline(in, line);) {
        const auto entry = ParseLine(line);
        if (entry && entry->label == "language_index") {
            if (const auto language = ConsoleLanguage::Parse(entry->value))
                result = language;
        }
    }
    return result;
}

inline std::filesystem::path File(const std::filesystem::path& local, std::string_view title_id) {
    if (title_id.size() != 16) {
        return {};
    }
    std::string normalized(title_id);
    for (char& digit : normalized) {
        if (digit >= 'a' && digit <= 'f') {
            digit -= 'a' - 'A';
        }
        if (!((digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F'))) {
            return {};
        }
    }
    return local / "settings" / (normalized + ".txt");
}

// 3 denotes auto; 1 remains the legacy on/2x spelling.
inline std::optional<unsigned> ParseFrameGeneration(std::string_view value) {
    if (value == "off" || value == "0")
        return 0;
    if (value == "1" || value == "2" || value == "2x")
        return 2;
    if (value == "4" || value == "4x")
        return 4;
    if (value == "auto" || value == "3")
        return 3;
    return std::nullopt;
}
inline std::optional<unsigned> ReadFrameGeneration(std::istream& in) {
    std::optional<unsigned> result;
    for (std::string line; std::getline(in, line);) {
        const auto entry = ParseLine(line);
        if (entry && (entry->label == "frame_gen" || entry->label == "frame_generation"))
            if (const auto mode = ParseFrameGeneration(entry->value))
                result = mode;
    }
    return result;
}
struct FrameGenerationDefault {
    std::string_view title;
    unsigned mode;
};
// Series X, 60 Hz: MK8D 28-29 real, 2x measured even 16.68 ms gaps. Odyssey is off: auto
// presented 22 fps against 26.6 without it and added multi-second hitches.
inline constexpr std::array<FrameGenerationDefault, 1> kFrameGenerationDefaults{
    {{"0100152000022000", 2}}};
inline unsigned DefaultFrameGeneration(std::string_view title) {
    for (const auto& entry : kFrameGenerationDefaults)
        if (entry.title == title)
            return entry.mode;
    return 0;
}
inline unsigned ResolveFrameGeneration(std::string_view title, std::optional<unsigned> global,
                                       std::optional<unsigned> per_title) {
    return per_title.value_or(global.value_or(DefaultFrameGeneration(title)));
}
inline unsigned EffectiveFrameGeneration(const std::filesystem::path& local,
                                         std::string_view title) {
    std::ifstream global(local / "eden_settings.txt");
    std::ifstream game(File(local, title));
    return ResolveFrameGeneration(title, ReadFrameGeneration(global), ReadFrameGeneration(game));
}
// nullopt restores inheritance. Preserve every unrelated setting and comment.
inline bool SaveFrameGeneration(const std::filesystem::path& file, std::optional<unsigned> mode) {
    if (file.empty() || (mode && *mode != 0 && *mode != 2 && *mode != 3 && *mode != 4))
        return false;
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error)
        return false;
    std::ifstream in(file);
    if (exists && !in)
        return false;
    std::string content;
    for (std::string line; std::getline(in, line);) {
        const auto entry = ParseLine(line);
        if (!entry || (entry->label != "frame_gen" && entry->label != "frame_generation"))
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
        return false;
    std::ofstream out(file, std::ios::trunc);
    out << content;
    if (mode)
        out << "frame_generation="
            << (*mode == 3   ? "auto"
                : *mode == 4 ? "4"
                : *mode == 2 ? "1"
                             : "0")
            << '\n';
    out.close();
    return !out.fail();
}

inline bool SaveLanguage(const std::filesystem::path& file,
                         std::optional<Settings::Language> mode) {
    if (file.empty() || (mode && static_cast<unsigned>(*mode) >= ConsoleLanguage::kNames.size()))
        return false;
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error)
        return false;
    std::ifstream in(file);
    if (exists && !in)
        return false;
    std::string content;
    for (std::string line; std::getline(in, line);) {
        const auto entry = ParseLine(line);
        if (!entry || entry->label != "language_index")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
        return false;
    std::ofstream out(file, std::ios::trunc);
    out << content;
    if (mode)
        out << "language_index=" << static_cast<unsigned>(*mode) << '\n';
    out.close();
    return !out.fail();
}

// Replace all resolution entries while retaining comments and other settings verbatim.
inline std::string WithResolution(const std::string& content, int value) {
    std::istringstream in(content);
    std::string result;
    std::string line;
    while (std::getline(in, line)) {
        const auto entry = ParseLine(line);
        if (!entry || entry->label != "resolution_setup") {
            result += line + '\n';
        }
    }
    return result + "resolution_setup=" + std::to_string(value) + '\n';
}

inline bool SaveResolution(const std::filesystem::path& file, int value) {
    if (file.empty()) {
        return false;
    }
    bool supported = false;
    for (const int choice : kResolutionValues) {
        supported |= choice == value;
    }
    if (!supported) {
        return false;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) {
        return false;
    }
    std::string content;
    if (exists) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return false;
        }
        content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) {
            return false;
        }
    }
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << WithResolution(content, value);
    out.close();
    return !out.fail();
}

} // namespace EdenXbox::TitleSettings
