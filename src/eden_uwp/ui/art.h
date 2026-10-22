// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include "eden_uwp/ui/art_cache.h"

namespace EdenXbox::Ui {
struct Pixels;
bool CacheBitmap(const std::filesystem::path& file, const Pixels& pixels);

enum class ArtState { Pending, Ready, None };
// Persistent keyless community hero cache.
class BackgroundArtSource {
public:
    explicit BackgroundArtSource(std::filesystem::path local_state);
    ~BackgroundArtSource();
    BackgroundArtSource(const BackgroundArtSource&) = delete;
    BackgroundArtSource& operator=(const BackgroundArtSource&) = delete;
    void Request(const std::string& title_id, const std::string& name, const std::string& english);
    ArtState Get(const std::string& title_id, std::filesystem::path& file) const;
    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};
} // namespace EdenXbox::Ui
