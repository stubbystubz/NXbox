// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "eden_uwp/title_settings.h"
namespace EdenXbox {
inline constexpr std::string_view kShowFpsKey = "show_fps";
inline constexpr bool kShowFpsDefault = false;
inline bool ReadShowFps(const std::filesystem::path& local_state) {
    std::ifstream in(local_state / "eden_settings.txt");
    bool enabled = kShowFpsDefault;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == kShowFpsKey)
            enabled = entry->value == "true";
    }
    return enabled;
}
inline bool SaveShowFps(const std::filesystem::path& local_state, bool enabled) {
    const auto file = local_state / "eden_settings.txt";
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error)
        return false;
    std::ifstream in(file);
    if (exists && !in)
        return false;
    std::string content;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (!entry || entry->label != kShowFpsKey)
            content += line + '\n';
    }
    if (in.bad())
        return false;
    std::ofstream out(file, std::ios::trunc);
    out << content << kShowFpsKey << '=' << (enabled ? "true" : "false") << '\n';
    out.close();
    return !out.fail();
}
} // namespace EdenXbox
