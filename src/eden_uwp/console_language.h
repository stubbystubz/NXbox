// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include "common/settings_enums.h"

namespace EdenXbox::ConsoleLanguage {
using Settings::Language;
inline constexpr std::array<std::string_view, 20> kNames{"Japanese",
                                                         "EnglishAmerican",
                                                         "French",
                                                         "German",
                                                         "Italian",
                                                         "Spanish",
                                                         "Chinese",
                                                         "Korean",
                                                         "Dutch",
                                                         "Portuguese",
                                                         "Russian",
                                                         "Taiwanese",
                                                         "EnglishBritish",
                                                         "FrenchCanadian",
                                                         "SpanishLatin",
                                                         "ChineseSimplified",
                                                         "ChineseTraditional",
                                                         "PortugueseBrazilian",
                                                         "Polish",
                                                         "Thai"};

inline Language FromTag(std::string_view tag) {
    std::string normalized(tag);
    for (char& c : normalized) {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    }
    tag = normalized;
    const auto base = tag.substr(0, tag.find('-'));
    std::string_view region, script;
    for (auto pos = tag.find('-'); pos != std::string_view::npos;) {
        const auto end = tag.find('-', pos + 1);
        const auto part = tag.substr(pos + 1, end == std::string_view::npos ? end : end - pos - 1);
        if (part.size() == 1)
            break; // Ignore extensions and private-use subtags.
        if (part.size() == 4)
            script = part;
        if (part.size() == 2 || part.size() == 3)
            region = part;
        pos = end;
    }
    if (base == "en")
        return region.empty() || region == "us" || region == "ca" || region == "ph"
                   ? Language::EnglishAmerican
                   : Language::EnglishBritish;
    if (base == "es")
        return !region.empty() && region != "es" ? Language::SpanishLatin : Language::Spanish;
    if (base == "pt")
        return region == "br" ? Language::PortugueseBrazilian : Language::Portuguese;
    if (base == "fr")
        return region == "ca" ? Language::FrenchCanadian : Language::French;
    if (base == "zh") {
        if (script == "hant")
            return Language::ChineseTraditional;
        if (script == "hans")
            return Language::ChineseSimplified;
        return region == "tw" || region == "hk" || region == "mo" ? Language::ChineseTraditional
                                                                  : Language::ChineseSimplified;
    }
    if (base == "ja")
        return Language::Japanese;
    if (base == "de")
        return Language::German;
    if (base == "it")
        return Language::Italian;
    if (base == "ko")
        return Language::Korean;
    if (base == "nl")
        return Language::Dutch;
    if (base == "ru")
        return Language::Russian;
    if (base == "pl")
        return Language::Polish;
    if (base == "th")
        return Language::Thai;
    return Language::EnglishAmerican;
}

inline std::optional<Language> Parse(std::string_view value) {
    unsigned index = 0;
    if (value.empty() || value.size() > 2)
        return std::nullopt;
    for (const char digit : value) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        index = index * 10 + digit - '0';
    }
    if (index >= kNames.size())
        return std::nullopt;
    return static_cast<Language>(index);
}
struct Selection {
    Language language;
    std::string_view source;
};
inline Selection Resolve(std::string_view tag, std::optional<Language> global,
                         std::optional<Language> title) {
    if (title)
        return {*title, "title"};
    if (global)
        return {*global, "setting"};
    return {FromTag(tag), "xbox"};
}
} // namespace EdenXbox::ConsoleLanguage
