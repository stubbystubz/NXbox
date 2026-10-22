// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include "eden_uwp/ui/strings.h"

namespace EdenXbox::Ui {
enum class Setting {
    SaveSync,
    Sources,
    Usb,
    Credits,
    Nextendo,
    Multiplayer,
    InternetArt,
    SystemMenu,
    OfficialInterface,
    ProfilePicture,
    Accounts,
    Moru,
    BackToNXbox,
    Renderer,
    ShaderPreload,
    AsyncShaders,
    XboxButtonIcons,
    ButtonLayout,
    MaxPlayers,
    Resolution,
    Upscaler,
    FrameGeneration,
    FramePacing,
    ShowFps,
    RecordVideo,
    Version,
    FtpServer,
    Count
};
struct SettingsCategory {
    Text title;
    std::array<Setting, 12> rows;
    int count;
};
inline constexpr std::array<SettingsCategory, 6> kSettingsCategories{{
    {Text::SettingsGraphics,
     {Setting::Renderer, Setting::ShaderPreload, Setting::AsyncShaders, Setting::XboxButtonIcons,
      Setting::ButtonLayout, Setting::MaxPlayers, Setting::Resolution, Setting::Upscaler,
      Setting::FrameGeneration, Setting::FramePacing, Setting::ShowFps, Setting::InternetArt},
     12},
    {Text::NavShop, {Setting::Sources, Setting::Usb}, 2},
    {Text::SettingsOnline, {Setting::Nextendo, Setting::Multiplayer, Setting::FtpServer}, 3},
    {Text::AccountSection,
     {Setting::ProfilePicture, Setting::Accounts, Setting::Moru, Setting::SaveSync},
     4},
    {Text::SettingsSystem,
     {Setting::SystemMenu, Setting::OfficialInterface, Setting::BackToNXbox, Setting::RecordVideo},
     4},
    {Text::SettingsAbout, {Setting::Version, Setting::Credits}, 2},
}};
inline constexpr std::array<unsigned, 3> kFrameGenerationModes{0, 2, 4};
inline constexpr std::array<Text, 3> kFrameGenerationChoices{
    Text::FrameGenerationOff, Text::FrameGeneration2x, Text::FrameGeneration4x};
inline constexpr std::array<Text, 3> kUpscalerChoices{Text::UpscalerBilinear, Text::UpscalerFsr,
                                                      Text::UpscalerTemporal};
inline constexpr std::array<const char*, 3> kUpscalerModes{"bilinear", "fsr1", "temporal"};
// Keep seven full-height settings rows visible as focus moves through larger categories.
inline constexpr int SettingsFirstVisibleRow(int selected) {
    return selected > 6 ? selected - 6 : 0;
}
inline constexpr bool RendererChoiceEnabled(int choice) {
    return choice >= 0 && choice < 2;
}
} // namespace EdenXbox::Ui
