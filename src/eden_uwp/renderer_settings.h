// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include "eden_uwp/button_layout.h"
#include "eden_uwp/gamepad_slots.h"
#include "eden_uwp/title_settings.h"

namespace EdenXbox::RendererSettings {
// Values of Eden's RendererBackend: OpenGL_GLSL = 0, D3D12 = 5.
inline constexpr int kOpenGL = 0;
inline constexpr int kDirectX12 = 5;
inline int Read(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    int backend = kDirectX12;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && (entry->label == "renderer_backend" || entry->label == "backend")) {
            // Mesa also supports the legacy GLASM and SPIR-V settings.
            backend = entry->value == "0" || entry->value == "3" || entry->value == "4"
                          ? kOpenGL
                          : kDirectX12;
        }
    }
    return backend;
}
inline bool ReadFrameGeneration(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    bool enabled = false;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "frame_generation")
            enabled = entry->value == "1";
    }
    return enabled;
}
inline bool SaveFrameGeneration(const std::filesystem::path& local, bool enabled) {
    const auto file = local / "eden_settings.txt";
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
        if (!entry || entry->label != "frame_generation")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "frame_generation=" << (enabled ? 1 : 0) << '\n';
    out.close();
    return !out.fail();
}
inline bool ReadFramePacing(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    bool enabled = false;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "frame_pacing")
            enabled = entry->value == "1";
    }
    return enabled;
}
inline bool SaveFramePacing(const std::filesystem::path& local, bool enabled) {
    const auto file = local / "eden_settings.txt";
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
        if (!entry || entry->label != "frame_pacing")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "frame_pacing=" << (enabled ? 1 : 0) << '\n';
    out.close();
    return !out.fail();
}
inline bool ReadShaderPreload(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    bool enabled = true;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "shader_preload")
            enabled = entry->value != "0";
    }
    return enabled;
}
inline bool SaveShaderPreload(const std::filesystem::path& local, bool enabled) {
    const auto file = local / "eden_settings.txt";
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
        if (!entry || entry->label != "shader_preload")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "shader_preload=" << (enabled ? 1 : 0) << '\n';
    out.close();
    return !out.fail();
}
inline bool ReadAsyncShaders(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    bool enabled = true;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "async_shaders")
            enabled = entry->value != "0";
    }
    return enabled;
}
inline bool SaveAsyncShaders(const std::filesystem::path& local, bool enabled) {
    const auto file = local / "eden_settings.txt";
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
        if (!entry || entry->label != "async_shaders")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "async_shaders=" << (enabled ? 1 : 0) << '\n';
    out.close();
    return !out.fail();
}
inline bool ReadFsr(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    bool enabled = true;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "upscaler")
            enabled = entry->value != "bilinear";
    }
    return enabled;
}
inline bool SaveFsr(const std::filesystem::path& local, bool enabled) {
    const auto file = local / "eden_settings.txt";
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
        if (!entry || entry->label != "upscaler")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "upscaler=" << (enabled ? "fsr1" : "bilinear") << '\n';
    out.close();
    return !out.fail();
}
// Invalid text keeps the default; numeric values are clamped to the supported range.
inline int ParseMaxPlayers(const std::string& text) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0')
        return 4;
    return value < 1 ? 1 : value > 4 ? 4 : static_cast<int>(value);
}
inline int ReadMaxPlayers(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    int value = 4;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "max_players")
            value = ParseMaxPlayers(entry->value);
    }
    if (const char* override_value = std::getenv("NXBOX_MAX_PLAYERS"))
        value = ParseMaxPlayers(override_value);
    return value;
}
inline bool SaveMaxPlayers(const std::filesystem::path& local, int value) {
    const auto file = local / "eden_settings.txt";
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
        if (!entry || entry->label != "max_players")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "max_players=" << ClampMaxPlayers(value) << '\n';
    out.close();
    return !out.fail();
}
inline bool ReadXboxButtonIcons(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    bool enabled = true;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "xbox_button_icons")
            enabled = entry->value != "0";
    }
    return enabled;
}
inline bool SaveXboxButtonIcons(const std::filesystem::path& local, bool enabled) {
    const auto file = local / "eden_settings.txt";
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
        if (!entry || entry->label != "xbox_button_icons")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "xbox_button_icons=" << (enabled ? 1 : 0) << '\n';
    out.close();
    return !out.fail();
}
// Presence is separate from the default so existing installs see the chooser once.
inline bool HasButtonLayout(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "button_layout")
            return true;
    }
    return false;
}
inline ButtonLayout ReadButtonLayout(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    ButtonLayout layout = ButtonLayout::Name;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "button_layout")
            layout = entry->value == "position" ? ButtonLayout::Position : ButtonLayout::Name;
    }
    return layout;
}
inline bool SaveButtonLayout(const std::filesystem::path& local, ButtonLayout layout) {
    const auto file = local / "eden_settings.txt";
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
        if (!entry || entry->label != "button_layout")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "button_layout=" << (layout == ButtonLayout::Position ? "position" : "name")
        << '\n';
    out.close();
    return !out.fail();
}
inline bool Save(const std::filesystem::path& local, int backend) {
    if (backend != kDirectX12 && backend != kOpenGL)
        return false;
    const auto file = local / "eden_settings.txt";
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
        if (!entry || (entry->label != "renderer_backend" && entry->label != "backend"))
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "renderer_backend=" << backend << '\n';
    out.close();
    return !out.fail();
}
} // namespace EdenXbox::RendererSettings
