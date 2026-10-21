// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/applet_boot.h"
#include "eden_uwp/ui/library_screen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

#include "common/scope_exit.h"
#include "common/fps_overlay.h"
#include "eden_uwp/fps_settings.h"
#include "common/scm_rev.h"
#include "eden_uwp/renderer_settings.h"
#include "eden_uwp/ui/settings_model.h"
#include "eden_uwp/accounts.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/firmware.h"
#include "eden_uwp/ftp/ftp_server.h"
#include "eden_uwp/home_ui_settings.h"
#include "eden_uwp/multiplayer_screen.h"
#include "eden_uwp/multiplayer_strings.h"
#include "eden_uwp/nextendo_online.h"
#include "eden_uwp/save_sync.h"
#include "eden_uwp/title_settings.h"
#include "eden_uwp/ui/accounts_screen.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/art.h"
#include "eden_uwp/ui/art_colors.h"
#include "eden_uwp/ui/cheats_screen.h"
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/library.h"
#include "eden_uwp/ui/mods_screen.h"
#include "eden_uwp/ui/moru_screen.h"
#include "eden_uwp/ui/nextendo_screen.h"
#include "eden_uwp/ui/renderer.h"
#include "eden_uwp/ui/savesync_ui.h"
#include "eden_uwp/ui/sources_screen.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/updater.h"
#include "eden_uwp/ui/usb_import_screen.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {

using D2D1::Point2F;
using D2D1::RectF;
using winrt::Windows::ApplicationModel::Core::CoreApplication;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

// Layout, in canvas units (the canvas is 1920x1080), from the approved preview.
constexpr float kMargin = 96.0f; // left and right edge of the content

// The hero lives on the top 760 px: the gradient and the scrim are laid out on that box, and the
// rail sits on plain black below it. Text on the left, the game's icon on the right.
constexpr float kHeroHeight = 760.0f;
constexpr float kHeroTextWidth = 1100.0f;
constexpr float kTitleBottom = 540.0f;
constexpr float kTitleHeight = 200.0f;
constexpr float kMetaTop = 560.0f;
constexpr float kMetaHeight = 24.0f;
constexpr float kMetaGap = 22.0f; // between a run of the meta line and the dot after it
constexpr float kMetaDot = 5.0f;
constexpr float kPillTop = 622.0f;
constexpr float kArtSize = 520.0f;
constexpr float kArtTop = kPillTop + kPillHeight - kArtSize; // its bottom lines up with the pills
constexpr float kArtRadius = 32.0f;

// The progress bar of a conversion, on the row of the hero pills.
constexpr float kBarWidth = 560.0f;
constexpr float kBarHeight = 14.0f;

// The rail of tiles, the last one being "add games".
constexpr float kSectionTop = 708.0f;
constexpr float kSectionHeight = 30.0f;
constexpr float kRailTop = 778.0f;
constexpr float kTileSize = 196.0f;
constexpr float kTileRadius = 22.0f;
constexpr float kTileGap = 30.0f;
constexpr float kTilePitch = kTileSize + kTileGap;
constexpr float kLiftScale = 0.08f;
constexpr float kLiftRise = 16.0f;
constexpr float kLiftTilt = -1.2f; // degrees
constexpr float kNameOffset = 22.0f; // from the bottom of the tile to the name under it
constexpr float kNameWidth = 520.0f;
constexpr float kNameHeight = 28.0f;
constexpr int kSkeletonTiles = 6;

// The details sheet.
constexpr float kSheetLeft = 420.0f;
constexpr float kSheetTop = 250.0f;
constexpr float kSheetWidth = 1080.0f;
constexpr float kSheetHeight = 500.0f;
constexpr float kSheetPadding = 56.0f;
constexpr float kSheetRowTop = 170.0f; // from the top of the sheet
constexpr float kSheetRowHeight = 72.0f;
constexpr float kSheetLabelWidth = 280.0f;

constexpr auto kToastDuration = std::chrono::milliseconds(2600);
constexpr auto kLaunchFade = std::chrono::milliseconds(180);

// Focus moves vertically through three layers, like the Xbox dashboard.
enum class Layer {
    Rail,    // the game rail: the default focus; A launches the focused game
    Actions, // the hero pills of the focused game
    Nav,     // the tabs at the top
};

enum class Tab { Library, Shop, Settings };

constexpr int kTabCount = 3;
constexpr int kActionPlay = 0;
constexpr int kActionMods = 1;
constexpr int kActionDetails = 2;
constexpr int kActionResolution = 3;
constexpr int kActionCount = 4;

constexpr std::size_t Idx(int index) {
    return static_cast<std::size_t>(index);
}

// The three colors of the hero gradient, top to bottom.
struct Palette {
    D2D1_COLOR_F top;
    D2D1_COLOR_F middle;
    D2D1_COLOR_F bottom;
};

constexpr Palette kNeutralPalette = {Theme::Rgb(0x26262B), Theme::Rgb(0x151518),
                                     Theme::Rgb(0x0A0A0C)};
constexpr Palette kBlackPalette = {Theme::Rgb(0x000000), Theme::Rgb(0x000000),
                                   Theme::Rgb(0x000000)};

// What the hero shows: a gradient and the icon of one game (an index into the games, or -1).
struct Look {
    Palette palette = kBlackPalette;
    int art = -1; // the game whose icon is drawn, when its icon decoded
    int key = -1; // the game this look belongs to, -1 for the others
};

// A game's icon on the GPU and the colors taken from it, loaded the first time it is needed.
struct Visual {
    bool loaded = false;
    Image image;
    Palette palette = kNeutralPalette;
    Image hero;
    Image background;
    bool background_checked = false;
    Tween background_blend{0.0f, kEaseHero};
};

Palette MixPalette(const Palette& from, const Palette& to, float t) {
    return {Mix(from.top, to.top, t), Mix(from.middle, to.middle, t),
            Mix(from.bottom, to.bottom, t)};
}

Palette ExtractPalette(const Pixels& pixels) {
    const auto colors = Art::Dominant(pixels.bgra, pixels.width, pixels.height);
    const auto convert = [](Art::Color c) { return D2D1::ColorF(c.r, c.g, c.b, 1.0f); };
    return {convert(colors[0]), convert(colors[1]), convert(colors[2])};
}

// A size as a number and its unit ("14,2" "GB"), so the number can be emphasized.
struct SizeText {
    std::wstring amount;
    const wchar_t* unit;
};

SizeText FormatSize(std::uint64_t bytes) {
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    const bool gigabytes = static_cast<double>(bytes) >= kGiB;
    wchar_t buffer[32];
    swprintf_s(buffer, std::size(buffer), gigabytes ? L"%.1f" : L"%.0f",
               static_cast<double>(bytes) / (gigabytes ? kGiB : kMiB));
    std::wstring amount = buffer;
    if (CurrentLanguage() == Language::Portuguese) {
        std::replace(amount.begin(), amount.end(), L'.', L',');
    }
    return {amount, gigabytes ? L"GB" : L"MB"};
}

// "NSP" or "XCI".
std::wstring FormatName(const GameEntry& game) {
    std::wstring name = game.path.extension().wstring();
    if (!name.empty() && name.front() == L'.') {
        name.erase(0, 1);
    }
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t letter) { return static_cast<wchar_t>(std::towupper(letter)); });
    return name;
}

class LibraryScreen {
public:
    LibraryScreen(Renderer& renderer, const CoreWindow& window,
                  const std::filesystem::path& local_state,
                  std::atomic<bool>* return_home = nullptr,
                  std::atomic<bool>* guest_home_request = nullptr)
        : renderer_(renderer), window_(window), input_(window), local_state_(local_state) {
        PurgeLegacyArt(local_state_);
        internet_art_ = !std::filesystem::exists(local_state_ / "internet-art.disabled");
        if (internet_art_)
            backgrounds_ = std::make_unique<BackgroundArtSource>(local_state_);
        // Headless transfers: an ftp-autostart file in LocalState starts the FTP server with the
        // library, so games can be copied to the removable drive without a controller.
        if (std::filesystem::exists(local_state_ / "ftp-autostart"))
            ftp_.Start(local_state_);
        return_home_ = return_home;
        guest_home_request_ = guest_home_request;
        input_.SetCancelFlag(return_home_);
        official_interface_ = UseOfficialSwitchInterface(local_state_);
        show_fps_ = ReadShowFps(local_state_);
        renderer_backend_ = RendererSettings::Read(local_state_);
        shader_preload_ = RendererSettings::ReadShaderPreload(local_state_);
        async_shaders_ = RendererSettings::ReadAsyncShaders(local_state_);
        fsr_ = RendererSettings::ReadFsr(local_state_);
        xbox_button_icons_ = RendererSettings::ReadXboxButtonIcons(local_state_);
        button_layout_ = RendererSettings::ReadButtonLayout(local_state_);
        std::ifstream resolution_settings(local_state_ / "eden_settings.txt");
        settings_resolution_ = TitleSettings::ReadResolution(resolution_settings);
        if (return_home_) {
            tab_ = Tab::Settings;
            layer_ = Layer::Rail;
            Toast(Tr(Text::HomeComboPrompt), Clock::now());
            input_.ConsumeUntilRelease();
        }
        NextendoOnline::Initialize(local_state_);
        firmware_status_ = GetFirmwareStatus();
        sync_account_ = GetSyncAccount();
        accounts_multi_ = Accounts::Load(local_state_).multi;
        sources_count_ = CountSources(local_state_);
        usb_mode_ = LoadUsbMode(local_state_);
        usb_detection_ = std::make_unique<UsbDetection>(local_state_);
        if (!return_home_)
            StartScan();
    }

    // Draws and handles input until the player launches a game or leaves. Returns the chosen
    // game's path (UTF-8), or an empty string.
    std::string Run() {
        const auto closed_token = window_.Closed([this](const auto&, const auto&) {
            closed_ = true;
            if (return_home_)
                return_home_->store(true);
        });
        // The system wants the GPU memory trimmed when the app is suspended.
        const auto suspending_token = CoreApplication::Suspending([this](const auto&, const auto&) {
            ftp_.Stop();
            renderer_.Trim();
        });
        SCOPE_EXIT {
            window_.Closed(closed_token);
            CoreApplication::Suspending(suspending_token);
        };
        Diagnostic("UI library screen open");
        while (!closed_ && !leaving_ && !(return_home_ && return_home_->load())) {
            const Clock::time_point frame_start = Clock::now();
            window_.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            const Clock::time_point now = Clock::now();
            Update(now);
            renderer_.BeginFrame();
            Draw(now);
            renderer_.EndFrame();
            if (launching_ && now - launch_started_ >= kLaunchFade) {
                break;
            }
            // Present1(1, 0) paces the loop on the display. Only an early return, as for an
            // occluded window, needs a sleep so that the loop never spins.
            const Clock::duration spent = Clock::now() - frame_start;
            if (spent < std::chrono::milliseconds(8)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16) - spent);
            }
        }
        return launching_ ? result_ : std::string();
    }

    // The game that was launched, or null when none was.
    const GameEntry* Chosen() const {
        return launching_ && launched_ >= 0 && launched_ < GameCount() ? &Game(launched_) : nullptr;
    }

private:
    void LaunchFirmwareApplet(Clock::time_point now) {
        launched_ = -1;
        launching_ = true;
        launch_started_ = now;
        input_.ConsumeUntilRelease();
    }
    // ---- State ----

    int GameCount() const {
        return static_cast<int>(games_.size());
    }
    const GameEntry& Game(int index) const {
        return games_[Idx(index)];
    }
    int RailCount() const {
        return scan_ ? 0 : GameCount() + 1; // the games and the "add games" tile
    }
    bool HasGame() const {
        return tab_ == Tab::Library && !scan_ && selected_ >= 0 && selected_ < GameCount();
    }
    bool OnAddTile() const {
        return tab_ == Tab::Library && !scan_ && selected_ == GameCount();
    }
    std::string SelectedTitleId() const {
        return HasGame() ? Game(selected_).title_id : std::string();
    }

    // Lists LocalState\games again, on a worker thread. The focus stays on the same game when it
    // is still there.
    void StartScan(bool convert_nsz = true) {
        keep_title_id_ = SelectedTitleId();
        games_.clear();
        tinfoil_index_ = -1;
        visuals_.clear();

        lift_.clear();
        name_.clear();
        hero_from_ = Look{};
        hero_to_ = Look{};
        hero_key_ = -2;
        selected_ = 0;
        keys_missing_ = false;
        scan_ = std::make_unique<LibraryScan>(local_state_, convert_nsz);
        Diagnostic("UI library scan started");
    }

    void AdoptScan() {
        if (!scan_ || !scan_->Finished()) {
            return;
        }
        games_ = scan_->Take();
        // Use the library model, never a filename guess or another disk scan.
        tinfoil_index_ = -1;
        for (int i = 0; i < GameCount(); ++i) {
            if (Game(i).title_id == "050000BADDAD0000") {
                tinfoil_index_ = i;
                break;
            }
        }
        keys_missing_ = scan_->KeysMissing();
        const auto& failures = scan_->FailedConversions();
        if (!failures.empty()) {
            const auto& first = failures.front();
            std::wstring message =
                Tr(first.kind == NszFailure::Kind::Damaged ? Text::ConversionDamaged
                                                           : Text::ConversionFailed);
            std::wstring name = first.name;
            if (failures.size() > 1)
                name += L" (+" + std::to_wstring(failures.size() - 1) + L")";
            message.replace(message.find(L"%s"), 2, name);
            Toast(message, Clock::now());
        }
        scan_.reset();
        visuals_.assign(games_.size(), Visual{});
        lift_.assign(games_.size() + 1, Tween(0.0f));
        name_.assign(games_.size() + 1, Tween(0.0f));
        selected_ = 0;
        for (int i = 0; i < GameCount(); ++i) {
            if (!keep_title_id_.empty() && Game(i).title_id == keep_title_id_) {
                selected_ = i;
            }
        }
        scroll_ = Tween(ScrollGoal(selected_));
        name_[Idx(selected_)] = Tween(1.0f);
        Diagnostic("UI library ready games=" + std::to_string(games_.size()));
    }

    Visual& EnsureVisual(int index) {
        Visual& visual = visuals_[Idx(index)];
        if (visual.loaded) {
            return visual;
        }
        visual.loaded = true;
        const GameEntry& game = Game(index);
        if (game.icon.empty()) {
            return visual;
        }
        Pixels pixels;
        auto decoded_file = game.icon;
        decoded_file.replace_extension(".bmp");
        if (!renderer_.DecodeImage(ReadFileBytes(decoded_file), pixels)) {
            if (!renderer_.DecodeImage(ReadFileBytes(game.icon), pixels)) {
                Diagnostic("UI library icon unusable for " + game.title_id);
                return visual;
            }
            CacheBitmap(decoded_file, pixels);
        }
        if (!renderer_.UploadImage(pixels, visual.image))
            return visual;
        visual.palette = ExtractPalette(pixels);
        // New provenance/versioned filenames ensure old store images can never be loaded.
        const auto hero_file =
            game.icon.parent_path() / (game.icon.stem().string() + ".hero-v1.bmp");
        Pixels hero;
        if (!renderer_.DecodeImage(ReadFileBytes(hero_file), hero)) {
            hero = Pixels{192, 76, Art::Hero(pixels.bgra, pixels.width, pixels.height)};
            CacheBitmap(hero_file, hero);
        }
        renderer_.UploadImage(hero, visual.hero);
        return visual;
    }

    Look LookFor(int key) {
        if (key < 0) {
            return Look{};
        }
        if (key >= GameCount()) {
            return Look{kNeutralPalette, -1, -1}; // the "add games" tile
        }
        const Visual& visual = EnsureVisual(key);
        return Look{visual.palette, visual.image.bitmap ? key : -1, key};
    }

    // ---- Input ----

    void Update(Clock::time_point now) {
        AdoptMove(now);
        AdoptDelete(now);
        AdoptScan();
        if (!scan_ && ftp_changes_ != ftp_.Changes()) {
            ftp_changes_ = ftp_.Changes();
            StartScan();
        }
        if (shop_) {
            shop_->SetTinfoilAvailable(tinfoil_index_ >= 0);
            shop_->Tick(now, false);
        }
        if (!launching_) {
            HandleInput(now);
        }
        // Input gets first refusal: an A press that starts a game must beat detection.
        if (!launching_ && !leaving_ && !closed_ && !scan_ && !details_open_ && !resolution_open_ &&
            !update_open_ && tab_ == Tab::Library && usb_detection_->Ready()) {
            const InputTransition transition(input_);
            closed_ = usb_detection_->Run(renderer_, window_, input_);
            usb_mode_ = LoadUsbMode(local_state_);
            if (!closed_)
                StartScan();
        }
        Refresh(now);
    }

    void AdoptDelete(Clock::time_point now) {
        if (delete_stage_ == 1 && delete_candidates_.valid() &&
            delete_candidates_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                delete_files_ = delete_candidates_.get();
            } catch (const std::exception& failure) {
                Diagnostic("LIBRARY_DELETE list failed reason=" + std::string(failure.what()));
                delete_files_.clear();
            }
            if (delete_files_.empty()) {
                delete_stage_ = 0;
                Toast(Tr(Text::DeleteFailed), now);
            } else {
                delete_stage_ = 2;
            }
        }
        if (delete_stage_ == 4 && delete_result_.valid() &&
            delete_result_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            bool ok = false;
            try {
                ok = delete_result_.get();
            } catch (const std::exception& failure) {
                Diagnostic("LIBRARY_DELETE failed reason=" + std::string(failure.what()));
            }
            delete_stage_ = 0;
            details_open_ = false;
            delete_files_.clear();
            Toast(Tr(ok ? Text::DeleteDone : Text::DeleteFailed), now);
            StartScan(false);
        }
    }

    void OpenDelete() {
        // The shop keeps downloading across tabs. Do not delete a live .partial file or
        // race a transfer publishing a replacement under the path the player reviewed.
        if (shop_ && shop_->HasDownloads()) {
            Toast(Tr(Text::DeleteDownloadBusy), Clock::now());
            input_.ConsumeUntilRelease();
            return;
        }
        delete_stage_ = 1;
        delete_row_ = 0;
        delete_confirm_ = false;
        const auto game = Game(selected_);
        const auto local = local_state_;
        delete_candidates_ = std::async(
            std::launch::async, [game, local] { return LibraryDeleteCandidates(game, local); });
        input_.ConsumeUntilRelease();
    }

    void AdoptMove(Clock::time_point now) {
        if (!move_active_ || !move_result_.valid() ||
            move_result_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        auto result = LibraryTransfer::Result::Failed;
        try {
            result = move_result_.get();
        } catch (...) {
            Diagnostic("LIBRARY_MOVE failed");
        }
        move_active_ = false;
        details_open_ = false;
        const auto message = result == LibraryTransfer::Result::Success   ? Text::MoveDone
                             : result == LibraryTransfer::Result::NoSpace ? Text::MoveNoSpace
                             : result == LibraryTransfer::Result::NoDrive ? Text::MoveNoDrive
                             : result == LibraryTransfer::Result::NotInternal
                                 ? Text::MoveNotInternal
                             : result == LibraryTransfer::Result::Conflict ? Text::MoveConflict
                                                                           : Text::MoveFailed;
        Toast(Tr(message), now);
        StartScan(false);
    }

    void OpenMove(Clock::time_point now) {
        if (scan_ || (shop_ && shop_->HasDownloads()) || ftp_.Running()) {
            Toast(Tr(Text::MoveBusy), now);
            return;
        }
        move_progress_.done = 0;
        move_progress_.total = 0;
        move_active_ = true;
        const auto game = Game(selected_);
        const auto local = local_state_;
        move_result_ = std::async(std::launch::async, [this, game, local] {
            return MoveLibraryGame(game, local, move_progress_);
        });
        input_.ConsumeUntilRelease();
    }

    void HandleInput(Clock::time_point now) {
        if (move_active_)
            return;
        if (delete_stage_) {
            if (delete_stage_ == 1 || delete_stage_ == 4)
                return;
            if (input_.Pressed(Button::B)) {
                delete_stage_ = delete_stage_ == 3 ? 2 : 0;
                delete_confirm_ = false;
            } else if (input_.Pressed(Button::Up)) {
                delete_row_ = std::max(0, delete_row_ - 1);
            } else if (input_.Pressed(Button::Down)) {
                delete_row_ = std::min(static_cast<int>(delete_files_.size()) - 1, delete_row_ + 1);
            } else if (delete_stage_ == 2 && input_.Pressed(Button::A)) {
                auto& file = delete_files_[Idx(delete_row_)];
                if (file.optional)
                    file.selected = !file.selected;
            } else if (delete_stage_ == 2 && input_.Pressed(Button::X)) {
                delete_stage_ = 3;
                delete_row_ = 0;
                delete_confirm_ = false; // Cancel is the default in the final confirmation.
            } else if (delete_stage_ == 3 &&
                       (input_.Pressed(Button::Left) || input_.Pressed(Button::Right))) {
                delete_confirm_ = !delete_confirm_;
            } else if (delete_stage_ == 3 && input_.Pressed(Button::A)) {
                if (!delete_confirm_) {
                    delete_stage_ = 2;
                } else {
                    delete_stage_ = 4;
                    const auto files = delete_files_;
                    const auto local = local_state_;
                    delete_result_ = std::async(std::launch::async, [files, local] {
                        return DeleteLibraryFiles(files, local);
                    });
                }
            }
            input_.ConsumeUntilRelease();
            return;
        }
        if (update_open_) {
            const auto state = updater_.State();
            if (state == UpdateState::Failed || state == UpdateState::MissingPortal) {
                if (input_.Pressed(Button::B)) {
                    update_open_ = false;
                    input_.ConsumeUntilRelease();
                } else if (input_.Pressed(Button::A))
                    updater_.StartInstall();
            }
            // Never start a game or another download while replacing the running package.
            return;
        }
        if (resolution_open_) {
            if (input_.Pressed(Button::B)) {
                resolution_open_ = false;
                input_.ConsumeUntilRelease();
                return;
            }
            if (input_.Pressed(Button::Left) || input_.Pressed(Button::Up)) {
                resolution_choice_ = std::max(0, resolution_choice_ - 1);
                resolution_save_failed_ = false;
            }
            if (input_.Pressed(Button::Right) || input_.Pressed(Button::Down)) {
                resolution_choice_ =
                    std::min(static_cast<int>(TitleSettings::kResolutionValues.size()) - 1,
                             resolution_choice_ + 1);
                resolution_save_failed_ = false;
            }
            if (input_.Pressed(Button::A)) {
                const auto file = resolution_global_
                                      ? local_state_ / "eden_settings.txt"
                                      : TitleSettings::File(local_state_, Game(selected_).title_id);
                const int value = TitleSettings::kResolutionValues[Idx(resolution_choice_)];
                resolution_save_failed_ = !TitleSettings::SaveResolution(file, value);
                Diagnostic("UI resolution " +
                           (resolution_global_ ? std::string("global") : Game(selected_).title_id) +
                           "=" + std::to_string(value) +
                           (resolution_save_failed_ ? " failed" : " saved"));
                if (!resolution_save_failed_) {
                    if (resolution_global_)
                        settings_resolution_ = value;
                    resolution_open_ = false;
                    input_.ConsumeUntilRelease();
                }
            }
            return;
        }
        if (details_open_) {
            if (input_.Pressed(Button::X)) {
                OpenMove(now);
            } else if (input_.Pressed(Button::A)) {
                OpenDelete();
            } else if (input_.Pressed(Button::B)) {
                details_open_ = false;
                input_.ConsumeUntilRelease();
            }
            return;
        }
        const bool shop_typing = tab_ == Tab::Shop && shop_ && shop_->Typing();
        if (!shop_typing && input_.Pressed(Button::LeftShoulder)) {
            SetTab(static_cast<Tab>((static_cast<int>(tab_) + kTabCount - 1) % kTabCount));
            return;
        }
        if (!shop_typing && input_.Pressed(Button::RightShoulder)) {
            SetTab(static_cast<Tab>((static_cast<int>(tab_) + 1) % kTabCount));
            return;
        }
        if (tab_ == Tab::Shop && layer_ != Layer::Nav) {
            shop_->Tick(now, true);
            if (shop_->TakeTinfoilRequest() && tinfoil_index_ >= 0 &&
                tinfoil_index_ < GameCount()) {
                selected_ = tinfoil_index_;
                Launch(now);
                return;
            }
            if (shop_->TakeSourcesRequest()) {
                SetTab(Tab::Settings);
                settings_category_ = 1;
                settings_row_ = 0;
                settings_rows_focused_ = true;
                layer_ = Layer::Rail;
                OpenSources();
            }
            if (shop_->TakeNavigationRequest())
                layer_ = Layer::Nav;
            return;
        }
        if (input_.Pressed(Button::Left)) {
            MoveHorizontal(-1);
        }
        if (input_.Pressed(Button::Right)) {
            MoveHorizontal(1);
        }
        if (input_.Pressed(Button::Up)) {
            MoveVertical(-1);
        }
        if (input_.Pressed(Button::Down)) {
            MoveVertical(1);
        }
        // X and Y are shortcuts for the focused game, from any layer.
        if (input_.Pressed(Button::X) && tab_ == Tab::Library && HasGame() && !update_focused_) {
            OpenMods();
            return;
        }
        if (input_.Pressed(Button::Y) && tab_ == Tab::Library && HasGame() && !update_focused_) {
            details_open_ = true;
            input_.ConsumeUntilRelease();
            return;
        }
        if (input_.Pressed(Button::A)) {
            Activate(now);
            return;
        }
        if (input_.Pressed(Button::B)) {
            if (tab_ == Tab::Settings && layer_ == Layer::Rail) {
                if (renderer_open_) {
                    renderer_open_ = false;
                } else if (settings_rows_focused_) {
                    settings_rows_focused_ = false;
                } else {
                    layer_ = Layer::Nav;
                }
                return;
            }
            Diagnostic("UI library quit");
            leaving_ = true;
        }
    }

    void SetTab(Tab tab) {
        if (return_home_ || tab_ == tab)
            return;
        update_focused_ = false;
        renderer_open_ = false;
        tab_ = tab;
        if (tab_ == Tab::Shop) {
            if (!shop_)
                shop_ = std::make_unique<ShopScreen>(renderer_, window_, input_, local_state_);
            layer_ = Layer::Nav;
        } else if (tab_ == Tab::Settings) {
            layer_ = Layer::Nav;
        } else if (shop_) {
            StartScan();
        }
    }

    void MoveHorizontal(int direction) {
        if (return_home_ && tab_ != Tab::Settings)
            return;
        switch (layer_) {
        case Layer::Rail:
            if (tab_ == Tab::Settings) {
                if (renderer_open_) {
                    renderer_open_ = false;
                }
                settings_rows_focused_ = direction > 0;
                break;
            }
            if (!scan_) {
                selected_ = std::clamp(selected_ + direction, 0, GameCount());
            }
            break;
        case Layer::Actions:
            action_ = std::clamp(action_ + direction, 0, kActionCount - 1);
            break;
        case Layer::Nav:
            if (return_home_)
                break;
            if (update_focused_) {
                if (direction < 0)
                    update_focused_ = false;
                break;
            }
            if (direction > 0 && tab_ == Tab::Settings && HasUpdate()) {
                update_focused_ = true;
                break;
            }
            // Focus on a tab selects it.
            SetTab(static_cast<Tab>(std::clamp(static_cast<int>(tab_) + direction, 0,
                                               kTabCount - 1)));
            break;
        }
    }

    void MoveVertical(int direction) {
        update_focused_ = false;
        if (tab_ == Tab::Settings && layer_ == Layer::Rail) {
            if (renderer_open_) {
                renderer_choice_ = std::clamp(renderer_choice_ + direction, 0, 2);
            } else if (settings_rows_focused_) {
                settings_row_ = std::clamp(settings_row_ + direction, 0,
                                           kSettingsCategories[Idx(settings_category_)].count - 1);
            } else if (direction < 0 && settings_category_ == 0 && !return_home_) {
                layer_ = Layer::Nav;
            } else {
                settings_category_ = std::clamp(settings_category_ + direction, 0, 5);
                settings_row_ = 0;
            }
            return;
        }
        if (direction < 0) {
            if (layer_ == Layer::Rail) {
                layer_ = HasGame() ? Layer::Actions : Layer::Nav;
            } else if (layer_ == Layer::Actions) {
                layer_ = Layer::Nav;
            }
        } else if (layer_ == Layer::Actions) {
            layer_ = Layer::Rail;
        } else if (layer_ == Layer::Nav) {
            if (tab_ == Tab::Settings) {
                layer_ = Layer::Rail;
            } else {
                layer_ = HasGame() ? Layer::Actions : Layer::Rail;
            }
        }
    }

    void Activate(Clock::time_point now) {
        if (tab_ == Tab::Shop && layer_ == Layer::Nav) {
            layer_ = Layer::Rail;
            return;
        }
        if (layer_ == Layer::Nav && update_focused_ && HasUpdate()) {
            update_open_ = true;
            input_.ConsumeUntilRelease();
            updater_.StartInstall();
            return;
        }
        switch (layer_) {
        case Layer::Rail:
            if (tab_ == Tab::Settings) {
                if (!settings_rows_focused_)
                    settings_rows_focused_ = true;
                else
                    ActivateSetting(now);
            } else if (HasGame()) {
                Launch(now);
            } else if (OnAddTile()) {
                OpenUsbImport();
            }
            break;
        case Layer::Actions:
            if (action_ == kActionPlay) {
                Launch(now);
            } else if (action_ == kActionMods) {
                OpenMods();
            } else if (action_ == kActionDetails) {
                details_open_ = true;
                input_.ConsumeUntilRelease();
            } else if (action_ == kActionResolution) {
                resolution_global_ = false;
                OpenResolution();
            }
            break;
        case Layer::Nav:
            if (tab_ == Tab::Settings)
                layer_ = Layer::Rail;
            break; // focus on a tab already selected it
        }
    }

    void Launch(Clock::time_point now) {
        const GameEntry& game = Game(selected_);
        if (game.incomplete) {
            details_open_ = true;
            Toast(Tr(LibraryContent::Label(game.problem)), now);
            input_.ConsumeUntilRelease();
            return;
        }
        result_ = winrt::to_string(game.path.wstring());
        Diagnostic("UI library launch " + game.title_id + " " + result_);
        launched_ = selected_;
        launching_ = true;
        launch_started_ = now;
    }

    void OpenResolution() {
        if (!resolution_global_ && !HasGame()) {
            return;
        }
        std::ifstream global(local_state_ / "eden_settings.txt");
        int value = TitleSettings::ReadResolution(global);
        if (!resolution_global_) {
            std::ifstream title(TitleSettings::File(local_state_, Game(selected_).title_id));
            value = TitleSettings::ReadResolution(title, value);
        }
        resolution_choice_ = 2;
        for (std::size_t i = 0; i < TitleSettings::kResolutionValues.size(); ++i) {
            if (TitleSettings::kResolutionValues[i] == value) {
                resolution_choice_ = static_cast<int>(i);
            }
        }
        resolution_save_failed_ = false;
        resolution_open_ = true;
        input_.ConsumeUntilRelease();
    }

    // The mod store of the focused game, on this window and this renderer. It runs its own loop
    // and returns when the player leaves with B.
    void OpenMods() {
        const InputTransition transition(input_);
        if (!HasGame()) {
            return;
        }
        const Visual& visual = EnsureVisual(selected_);
        ModsGame mods;
        mods.title_id = Game(selected_).title_id;
        mods.name = Game(selected_).name;
        mods.icon = visual.image.bitmap ? &visual.image : nullptr;
        mods.glow = visual.palette.top;
        Diagnostic("UI library open mods " + mods.title_id);
        if (RunModsScreen(renderer_, window_, input_, mods, local_state_)) {
            closed_ = true;
        }
    }

    // The USB import screen, on this window and this renderer. It runs its own loop and returns
    // when the player leaves with B; whatever it copied or moved is picked up by a new scan.
    void OpenUsbImport() {
        const InputTransition transition(input_);
        Diagnostic("UI library open usb import");
        if (RunUsbImportScreen(renderer_, window_, input_, local_state_)) {
            closed_ = true;
            return;
        }
        if (!return_home_)
            StartScan();
    }

    // The sources screen, on this window and this renderer. It runs its own loop and returns when
    // the player leaves with B; a download it finished is picked up by a new scan.
    void OpenSources() {
        const InputTransition transition(input_);
        Diagnostic("UI library open sources");
        if (RunSourcesScreen(renderer_, window_, input_, local_state_)) {
            closed_ = true;
            return;
        }
        sources_count_ = CountSources(local_state_);
        if (shop_)
            shop_->ReloadSources();
        if (!return_home_)
            StartScan();
    }

    // Settings > Credits: who made what NXbox builds on (cheats_screen.cpp).
    void OpenCredits() {
        const InputTransition transition(input_);
        Diagnostic("UI library open credits");
        if (RunCreditsScreen(renderer_, window_, input_)) {
            closed_ = true;
        }
    }

    Setting SelectedSetting() const {
        return kSettingsCategories[Idx(settings_category_)].rows[Idx(settings_row_)];
    }

    // A on a settings row.
    void ActivateSetting(Clock::time_point now) {
        if (renderer_open_) {
            if (!RendererChoiceEnabled(renderer_choice_))
                return;
            const int backend =
                renderer_choice_ == 0 ? RendererSettings::kDirectX12 : RendererSettings::kOpenGL;
            if (RendererSettings::Save(local_state_, backend)) {
                renderer_backend_ = backend;
                renderer_open_ = false;
                Toast(Tr(Text::RendererNextLaunch), now);
            } else {
                Toast(Tr(Text::ResolutionSaveFailed), now);
            }
            return;
        }
        if (SelectedSetting() == Setting::Renderer) {
            renderer_choice_ = renderer_backend_ == RendererSettings::kOpenGL ? 1 : 0;
            renderer_open_ = true;
            return;
        }
        if (SelectedSetting() == Setting::ButtonLayout) {
            const auto next =
                button_layout_ == ButtonLayout::Name ? ButtonLayout::Position : ButtonLayout::Name;
            if (RendererSettings::SaveButtonLayout(local_state_, next))
                button_layout_ = next;
            else
                Toast(Tr(Text::ButtonLayoutSaveFailed), now);
            return;
        }
        if (SelectedSetting() == Setting::XboxButtonIcons) {
            if (RendererSettings::SaveXboxButtonIcons(local_state_, !xbox_button_icons_))
                xbox_button_icons_ = !xbox_button_icons_;
            else
                Toast(Tr(Text::ResolutionSaveFailed), now);
            return;
        }
        if (SelectedSetting() == Setting::ShaderPreload) {
            if (RendererSettings::SaveShaderPreload(local_state_, !shader_preload_))
                shader_preload_ = !shader_preload_;
            else
                Toast(Tr(Text::ResolutionSaveFailed), now);
            return;
        }
        if (SelectedSetting() == Setting::AsyncShaders) {
            if (RendererSettings::SaveAsyncShaders(local_state_, !async_shaders_))
                async_shaders_ = !async_shaders_;
            else
                Toast(Tr(Text::ResolutionSaveFailed), now);
            return;
        }
        if (SelectedSetting() == Setting::Upscaler) {
            if (RendererSettings::SaveFsr(local_state_, !fsr_))
                fsr_ = !fsr_;
            else
                Toast(Tr(Text::ResolutionSaveFailed), now);
            return;
        }
        if (SelectedSetting() == Setting::Resolution) {
            resolution_global_ = true;
            OpenResolution();
            return;
        }
        if (SelectedSetting() == Setting::Version)
            return;
        if (SelectedSetting() == Setting::FtpServer) {
            if (ftp_.Running())
                ftp_.Stop();
            else if (!ftp_.Start(local_state_))
                Toast(Tr(Text::FtpStartFailed), now);
            return;
        }
        if (SelectedSetting() == Setting::ShowFps) {
            if (SaveShowFps(local_state_, !show_fps_)) {
                show_fps_ = !show_fps_;
                Common::FpsOverlay::SetPreference(show_fps_);
            } else {
                Toast(Tr(Text::ResolutionSaveFailed), now);
            }
            return;
        }
        if (SelectedSetting() == Setting::InternetArt) {
            const auto file = local_state_ / "internet-art.disabled";
            std::error_code ec;
            bool saved = true;
            if (internet_art_)
                saved = Art::WriteBytes(file, {1});
            else {
                std::filesystem::remove(file, ec);
                saved = !ec;
            }
            if (!saved) {
                Toast(Tr(Text::ResolutionSaveFailed), now);
                return;
            }
            internet_art_ = !internet_art_;
            if (internet_art_)
                backgrounds_ = std::make_unique<BackgroundArtSource>(local_state_);
            else
                backgrounds_.reset();
            return;
        }
        if (SelectedSetting() == Setting::Moru) {
            if (return_home_) {
                return_home_->store(true); // Release the guest database before editing.
                return;
            }
            const InputTransition transition(input_);
            if (RunMoruScreen(renderer_, window_, input_, nullptr, &result_))
                closed_ = true;
            if (!result_.empty())
                LaunchFirmwareApplet(now);
            return;
        }
        if (SelectedSetting() == Setting::Accounts ||
            SelectedSetting() == Setting::ProfilePicture) {
            if (return_home_) {
                return_home_->store(true); // Edit accounts only after the guest releases its saves.
                return;
            }
            const InputTransition transition(input_);
            if (RunAccountsScreen(renderer_, window_, input_, local_state_,
                                  SelectedSetting() == Setting::ProfilePicture, &result_))
                closed_ = true;
            if (!result_.empty())
                LaunchFirmwareApplet(now);
            accounts_multi_ = Accounts::Load(local_state_).multi;
            return;
        }
        if (SelectedSetting() == Setting::BackToNXbox) {
            if (SaveOfficialSwitchInterface(local_state_, false)) {
                official_interface_ = false;
                if (return_home_)
                    return_home_->store(true);
            } else
                Toast(Tr(Text::ResolutionSaveFailed), now);
            return;
        }
        if (SelectedSetting() == Setting::OfficialInterface) {
            if (firmware_status_.home_available &&
                SaveOfficialSwitchInterface(local_state_, !official_interface_)) {
                official_interface_ = !official_interface_;
            } else if (firmware_status_.home_available)
                Toast(Tr(Text::ResolutionSaveFailed), now);
            return;
        }
        if (SelectedSetting() == Setting::SystemMenu) {
            if (!firmware_status_.home_available)
                return;
            if (guest_home_request_) {
                guest_home_request_->store(true);
                leaving_ = true;
                return;
            }
            result_ = std::string(SwitchHomeBootTarget);
            LaunchFirmwareApplet(now);
            return;
        }
        if (SelectedSetting() == Setting::Multiplayer) {
            const InputTransition transition(input_);
            if (RunMultiplayerScreen(renderer_, window_, input_)) {
                closed_ = true;
            }
            return;
        }
        if (SelectedSetting() == Setting::Nextendo) {
            const InputTransition transition(input_);
            closed_ = RunNextendoScreen(renderer_, window_, input_);
            return;
        }
        if (SelectedSetting() == Setting::Credits) {
            OpenCredits();
            return;
        }
        if (SelectedSetting() == Setting::Usb) {
            const auto next = usb_mode_ == UsbMode::Ask        ? UsbMode::Copy
                              : usb_mode_ == UsbMode::Copy     ? UsbMode::External
                              : usb_mode_ == UsbMode::External ? UsbMode::Off
                                                               : UsbMode::Ask;
            if (SaveUsbMode(local_state_, next)) {
                usb_mode_ = next;
            } else {
                Toast(Tr(Text::UsbSaveFailed), now);
            }
            return;
        }
        if (SelectedSetting() == Setting::Sources) {
            OpenSources();
            return;
        }
        if (SelectedSetting() != Setting::SaveSync)
            return;
        switch (sync_account_) {
        case SyncAccount::SignedIn:
            SaveSync::SignOut();
            Diagnostic("UI settings savesync signed out");
            sync_account_ = GetSyncAccount();
            Toast(Tr(Text::SyncStateOff), now);
            break;
        case SyncAccount::NotConfigured:
        case SyncAccount::SignedOut: {
            bool window_closed = false;
            const InputTransition transition(input_);
            const SignInOutcome outcome = RunSignIn(renderer_, window_, input_, window_closed);
            Diagnostic("UI settings savesync sign-in outcome " +
                       std::to_string(static_cast<int>(outcome)));
            if (window_closed) {
                closed_ = true;
            }
            sync_account_ = GetSyncAccount();
            break;
        }
        }
    }

    void Toast(const std::wstring& text, Clock::time_point now) {
        toast_ = text;
        toast_until_ = now + kToastDuration;
    }

    // ---- Animation targets ----

    // Keeps focus on a layer that exists, and points every animation at where it should be.
    void Refresh(Clock::time_point now) {
        if (tab_ == Tab::Settings) {
            if (layer_ == Layer::Actions) {
                layer_ = Layer::Rail; // the settings have no hero pills
            }
        } else if (layer_ == Layer::Actions && !HasGame()) {
            layer_ = Layer::Rail;
        }
        selected_ = std::clamp(selected_, 0, std::max(RailCount() - 1, 0));

        for (int i = 0; i < static_cast<int>(lift_.size()); ++i) {
            // Only the focused tile lifts; the selected game keeps its name under its tile.
            const bool selected = tab_ == Tab::Library && i == selected_;
            const float lift_target = selected && layer_ == Layer::Rail ? 1.0f : 0.0f;
            Tween& lift = lift_[Idx(i)];
            if (lift.Target() != lift_target) {
                lift.To(lift_target, now);
            }
            const float name_target = selected ? 1.0f : 0.0f;
            Tween& name = name_[Idx(i)];
            if (name.Target() != name_target) {
                name.To(name_target, now);
            }
        }
        const float goal = ScrollGoal(selected_);
        if (goal != scroll_.Target()) {
            scroll_.To(goal, now, kDurationPanel);
        }
        RefreshHero(now);
    }

    // The focused tile always stands at the left margin, where the approved layout puts it, and
    // the rail scrolls under it. That also keeps its name clear of the hint bar.
    float ScrollGoal(int index) const {
        return static_cast<float>(index) * kTilePitch;
    }

    // The hero fades to the look of whatever is selected whenever that changes.
    void RefreshHero(Clock::time_point now) {
        const int key = tab_ == Tab::Library && !scan_ ? selected_ : -1;
        if (key == hero_key_) {
            return;
        }
        hero_key_ = key;
        // What is on screen right now is where the new fade starts from.
        const float shown = hero_blend_.Value(now);
        Look from;
        from.palette = MixPalette(hero_from_.palette, hero_to_.palette, shown);
        from.art = shown < 0.5f ? hero_from_.art : hero_to_.art;
        from.key = shown < 0.5f ? hero_from_.key : hero_to_.key;
        hero_from_ = from;
        hero_to_ = LookFor(key);
        // Only the two transitioning heroes need full-size GPU backgrounds.
        for (std::size_t i = 0; i < visuals_.size(); ++i) {
            if (static_cast<int>(i) == hero_from_.key || static_cast<int>(i) == hero_to_.key)
                continue;
            auto& visual = visuals_[i];
            visual.background = {};
            visual.background_checked = false;
            visual.background_blend = Tween(0.0f, kEaseHero);
        }
        hero_blend_ = Tween(0.0f, kEaseHero);
        hero_blend_.To(1.0f, now, kDurationScreen);
    }

    // ---- Drawing ----

    void Draw(Clock::time_point now) {
        if (return_home_) {
            renderer_.FillRounded(RectF(kMargin, 180.0f, kCanvasWidth - kMargin, 1010.0f), 24.0f,
                                  Theme::kSheet);
            DrawSettings();
            DrawToast(now);
            if (resolution_open_)
                DrawResolution();
            DrawHints(renderer_, CurrentHints());
            return;
        }
        if (tab_ == Tab::Shop && shop_) {
            shop_->Draw(now, layer_ == Layer::Nav);
            return;
        }
        if (tab_ == Tab::Library)
            DrawBackdrop(now);
        else
            renderer_.FillRounded(RectF(0.0f, 0.0f, kCanvasWidth, kCanvasHeight), 0.0f,
                                  Theme::kBackground);
        if (tab_ == Tab::Library) {
            DrawHeroBackground(now);
        }
        DrawScrim();
        if (tab_ == Tab::Library) {
            DrawHeroArt(now);
            DrawHeroText(now);
            DrawRail(now);
        } else {
            DrawSettings();
        }
        DrawTabs(renderer_, static_cast<int>(tab_), layer_ == Layer::Nav && !update_focused_);
        DrawClock(renderer_);
        if (HasUpdate()) {
            DrawUpdatePill();
            // Keep feedback from Settings visible without covering the update pill.
            renderer_.SetLocalTransform(D2D1::Matrix3x2F::Translation(0.0f, 100.0f));
            DrawToast(now);
            renderer_.ClearLocalTransform();
        } else {
            DrawToast(now);
        }
        if (delete_stage_) {
            DrawDelete();
        } else if (details_open_) {
            DrawDetails();
        }
        if (resolution_open_) {
            DrawResolution();
        }
        if (update_open_)
            DrawUpdateSheet();
        DrawHints(renderer_, CurrentHints()); // above the sheet's veil: the sheet's own hint says how to close it
        if (launching_) {
            const float progress =
                std::chrono::duration<float>(now - launch_started_).count() /
                std::chrono::duration<float>(kLaunchFade).count();
            renderer_.FillRounded(RectF(0.0f, 0.0f, kCanvasWidth, kCanvasHeight), 0.0f,
                                  Theme::Rgb(0x000000, std::clamp(progress, 0.0f, 1.0f)));
        }
    }

    float TextWidth(const std::wstring& text, Font font) const {
        return renderer_.MeasureString(text, font).width;
    }

    // The gradient also gives games without control icons a deliberate empty state.
    void DrawBackdrop(Clock::time_point now) {
        const Palette palette =
            MixPalette(hero_from_.palette, hero_to_.palette, hero_blend_.Value(now));
        // Three stops with the middle one pulled forward (0 / 48 / 72%): a two-stop ramp is the
        // default gradient of every tool.
        const D2D1_GRADIENT_STOP ramp[3] = {{Art::kGradientStops[0], palette.top},
                                            {Art::kGradientStops[1], palette.middle},
                                            {Art::kGradientStops[2], palette.bottom}};
        renderer_.FillGradient(RectF(0.0f, 0.0f, kCanvasWidth, kHeroHeight), 0.0f, ramp, 3,
                               Point2F(0.0f, 0.0f), Point2F(0.0f, kHeroHeight));
    }

    // Separate from cached artwork: the left side carries text. Even pure white artwork
    // under the 82% black veil has luminance <= .028, giving secondary text WCAG AA.
    void DrawScrim() {
        const auto clear = WithOpacity(Theme::kBackground, 0.0f);
        const D2D1_GRADIENT_STOP bottom[] = {{0.0f, clear},
                                             {0.5f, clear},
                                             {Art::kBottomScrimStart, clear},
                                             {1.0f, Theme::kBackground}};
        const auto rect = RectF(0.0f, 0.0f, kCanvasWidth, kHeroHeight);
        renderer_.FillGradient(rect, 0, bottom, 4, Point2F(0, 0), Point2F(0, kHeroHeight));
        // A linear fall-off leaves a visible vertical seam where the flat veil ends, so the
        // fade follows smoothstep (no slope jump at either end), sampled into stops.
        constexpr int kFadeStops = 9;
        D2D1_GRADIENT_STOP left[kFadeStops + 1];
        left[0] = {0.0f, WithOpacity(Theme::kBackground, Art::kLeftScrimOpacity)};
        for (int i = 0; i < kFadeStops; ++i) {
            const float t = static_cast<float>(i) / (kFadeStops - 1);
            const float eased = t * t * (3.0f - 2.0f * t);
            left[i + 1] = {Art::kLeftScrimEnd + (1.0f - Art::kLeftScrimEnd) * t,
                           WithOpacity(Theme::kBackground, Art::kLeftScrimOpacity * (1.0f - eased))};
        }
        renderer_.FillGradient(rect, 0, left, kFadeStops + 1, Point2F(0, 0),
                               Point2F(kCanvasWidth, 0));
    }

    void DrawHeroBackground(Clock::time_point now) {
        const float blend = hero_blend_.Value(now);
        const auto draw = [&](int key, float opacity) {
            if (key < 0 || key >= GameCount())
                return;
            auto& visual = EnsureVisual(key);
            renderer_.DrawImage(visual.hero, RectF(0, 0, kCanvasWidth, kHeroHeight), 0, opacity);
            if (!internet_art_ || !backgrounds_)
                return;
            const auto& game = Game(key);
            if (game.incomplete)
                return;
            if (!visual.background_checked) {
                // NACP slot 0 is American English; slot 1 is British English.
                std::string english;
                for (std::size_t i = 0; i < std::min(std::size_t(2), game.nacp_names.size()); ++i)
                    if (!game.nacp_names[i].empty()) {
                        english = game.nacp_names[i];
                        break;
                    }
                backgrounds_->Request(game.title_id, winrt::to_string(game.name), english);
                std::filesystem::path file;
                if (backgrounds_->Get(game.title_id, file) == ArtState::Ready) {
                    visual.background_checked = true;
                    Pixels pixels;
                    if (renderer_.DecodeImage(ReadFileBytes(file), pixels) &&
                        renderer_.UploadImage(pixels, visual.background))
                        visual.background_blend.To(1.0f, now, kDurationScreen);
                }
            }
            renderer_.DrawImage(visual.background, RectF(0, 0, kCanvasWidth, kHeroHeight), 0,
                                opacity * visual.background_blend.Value(now));
        };
        const bool has_next = hero_to_.key >= 0 && hero_to_.key < GameCount() &&
                              EnsureVisual(hero_to_.key).hero.bitmap;
        draw(hero_from_.key, has_next ? 1.0f : 1.0f - blend);
        draw(hero_to_.key, blend);
    }

    void DrawHeroArt(Clock::time_point now) {
        const float blend = hero_blend_.Value(now);
        const D2D1_RECT_F rect = RectF(kCanvasWidth - kMargin - kArtSize, kArtTop,
                                       kCanvasWidth - kMargin, kArtTop + kArtSize);
        const auto draw = [&](int art, float opacity) {
            if (art >= 0 && opacity > 0.001f) {
                renderer_.DrawImage(EnsureVisual(art).image, rect, kArtRadius, opacity);
            }
        };
        // The old icon stays underneath while the new one fades in over it.
        if (hero_from_.art == hero_to_.art) {
            draw(hero_to_.art, 1.0f);
        } else {
            draw(hero_from_.art, hero_to_.art >= 0 ? 1.0f : 1.0f - blend);
            draw(hero_to_.art, blend);
        }
        if (hero_to_.art >= 0) {
            renderer_.StrokeRounded(Inflate(rect, -0.5f), kArtRadius - 0.5f, 1.0f,
                                    Theme::kHairline);
        }
    }

    void DrawTitle(const std::wstring& title, float opacity = 1.0f) {
        if (opacity <= 0.001f) {
            return;
        }
        const Font font = renderer_.MeasureString(title, Font::Title, kHeroTextWidth).lines > 2
                              ? Font::TitleSmall
                              : Font::Title;
        renderer_.DrawString(title, font,
                             RectF(kMargin, kTitleBottom - kTitleHeight, kMargin + kHeroTextWidth,
                                   kTitleBottom),
                             WithOpacity(Theme::kText, opacity), HAlign::Left, VAlign::Bottom);
    }

    // A title with a paragraph under it, for the states without a game.
    void DrawMessage(const std::wstring& title, const std::wstring& body) {
        DrawTitle(title);
        renderer_.DrawString(body, Font::Body,
                             RectF(kMargin, kMetaTop, kMargin + 900.0f, kSectionTop - 16.0f),
                             Theme::kTextSecondary);
    }

    void DrawHeroText(Clock::time_point now) {
        if (scan_) {
            const LibraryScan::Conversion conversion = scan_->CurrentConversion();
            if (conversion.active) {
                DrawConversion(conversion);
            } else {
                DrawMessage(Tr(Text::ScanningTitle), std::to_wstring(scan_->Done()) + L" / " +
                                                         std::to_wstring(scan_->Total()));
            }
        } else if (HasGame()) {
            DrawGameHero(Game(selected_));
        } else if (GameCount() == 0 && keys_missing_) {
            // The folder has games, but nothing could be opened: say why, not "no games".
            DrawMessage(Tr(Text::MissingKeysTitle), Tr(Text::MissingKeysBody));
        } else {
            DrawMessage(GameCount() == 0 ? Tr(Text::EmptyTitle) : Tr(Text::AddGamesTitle),
                        Tr(Text::AddGamesBody));
        }
    }

    // A package being converted from .nsz to .nsp: its name, what is going on, a progress bar and
    // the percentage. Leaving with B cancels the conversion.
    void DrawConversion(const LibraryScan::Conversion& conversion) {
        DrawTitle(conversion.name);
        float x = DrawMetaRun(kMargin, Tr(Text::ConvertingTitle), Font::MetaMono,
                              Theme::kTextSecondary);
        if (conversion.count > 1) {
            x = DrawMetaDot(x);
            DrawMetaRun(x,
                        std::to_wstring(conversion.index) + L" / " +
                            std::to_wstring(conversion.count),
                        Font::MetaMono, Theme::kTextSecondary);
        }

        const double fraction =
            conversion.total > 0 ? std::min(static_cast<double>(conversion.done) /
                                                static_cast<double>(conversion.total),
                                            1.0)
                                 : 0.0;
        const float top = kPillTop + (kPillHeight - kBarHeight) / 2.0f;
        const D2D1_RECT_F track = RectF(kMargin, top, kMargin + kBarWidth, top + kBarHeight);
        renderer_.FillRounded(track, kBarHeight / 2.0f, Theme::kSurfaceStrong);
        if (fraction > 0.0) {
            // The ribbon runs across the filled part, so it always ends in red.
            const D2D1_RECT_F fill =
                RectF(track.left, track.top,
                      track.left + std::max(static_cast<float>(fraction) * kBarWidth, kBarHeight),
                      track.bottom);
            renderer_.FillGradient(fill, kBarHeight / 2.0f, kRibbonStops, 4,
                                   Point2F(fill.left, top), Point2F(fill.right, top), 0.9f);
        }
        renderer_.DrawString(std::to_wstring(static_cast<int>(fraction * 100.0)) + L"%",
                             Font::MetaStrong,
                             RectF(track.right + 24.0f, top - 12.0f, track.right + 200.0f,
                                   top + kBarHeight + 12.0f),
                             Theme::kText, HAlign::Left, VAlign::Middle);
    }

    // One run of the meta line, in the monospaced face. Returns where the next one starts.
    float DrawMetaRun(float x, const std::wstring& text, Font font, const D2D1_COLOR_F& color) {
        const float width = TextWidth(text, font);
        renderer_.DrawString(text, font,
                             RectF(x, kMetaTop, x + width + 8.0f, kMetaTop + kMetaHeight), color,
                             HAlign::Left, VAlign::Middle);
        return x + width;
    }

    // The dot between two runs, with the gap on both sides. Returns where the next run starts.
    float DrawMetaDot(float x) {
        renderer_.FillDisc(Point2F(x + kMetaGap + kMetaDot / 2.0f, kMetaTop + kMetaHeight / 2.0f),
                           kMetaDot / 2.0f, Theme::kTextTertiary);
        return x + 2.0f * kMetaGap + kMetaDot;
    }

    void DrawGameHero(const GameEntry& game) {
        DrawTitle(game.name);

        // Format, size and title ID in one monospaced line; the number of the size is the one
        // thing set in the brighter, heavier face.
        const SizeText size = FormatSize(game.size);
        float x = DrawMetaRun(kMargin, FormatName(game), Font::MetaMono, Theme::kTextSecondary);
        x = DrawMetaDot(x);
        x = DrawMetaRun(x, size.amount, Font::MetaStrong, Theme::kText);
        x = DrawMetaRun(x, std::wstring(L" ") + size.unit, Font::MetaMono, Theme::kTextSecondary);
        x = DrawMetaDot(x);
        DrawMetaRun(x, Widen(game.title_id), Font::MetaMono, Theme::kTextSecondary);

        DrawActions();
    }

    // The focused game's actions.
    void DrawActions() {
        const std::array<const wchar_t*, kActionCount> labels = {
            Tr(Text::ActionPlay), Tr(Text::ActionMods), Tr(Text::ActionDetails),
            Tr(Text::ResolutionTitle)};
        float x = kMargin;
        for (int i = 0; i < kActionCount; ++i) {
            const bool primary = i == kActionPlay;
            const std::wstring label = labels[Idx(i)];
            const float width = PillWidth(renderer_, label, primary);
            DrawPill(renderer_, RectF(x, kPillTop, x + width, kPillTop + kPillHeight), label,
                     primary, layer_ == Layer::Actions && action_ == i);
            x += width + kPillGap;
        }
    }

    void DrawRail(Clock::time_point now) {
        renderer_.DrawString(Tr(Text::RailTitle), Font::Section,
                             RectF(kMargin, kSectionTop, kMargin + 600.0f,
                                   kSectionTop + kSectionHeight),
                             Theme::kTextSecondary);
        std::wstring firmware_text = firmware_status_.version.empty()
                                         ? std::wstring(Tr(Text::FirmwareMissing))
                                         : std::wstring(Tr(Text::FirmwareInstalled)) + L" " +
                                               Widen(firmware_status_.version);
        if (firmware_status_.conversion_required) {
            firmware_text = Tr(Text::FirmwareConvertDump);
        } else if (firmware_status_.install_failed) {
            firmware_text = firmware_status_.version.empty()
                                ? std::wstring(Tr(Text::FirmwareInstallFailed))
                                : firmware_text + L" · " + Tr(Text::FirmwareInstallFailed);
        }
        renderer_.DrawString(firmware_text, Font::MetaMono,
                             RectF(kMargin + 620.0f, kSectionTop, kCanvasWidth - kMargin,
                                   kSectionTop + kSectionHeight),
                             Theme::kTextSecondary);
        if (scan_) {
            DrawSkeletonRail(now);
            return;
        }
        const float scroll = scroll_.Value(now);
        // Tiles that scrolled past the left margin are hidden, so nothing sticks out under it.
        renderer_.PushClip(RectF(kMargin - 30.0f, 0.0f, kCanvasWidth, kCanvasHeight));
        // Resting tiles first, then the lifted ones, so a lifted tile is never covered.
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < RailCount(); ++i) {
                const float x = kMargin + static_cast<float>(i) * kTilePitch - scroll;
                if (x + kTileSize < 0.0f || x > kCanvasWidth) {
                    continue;
                }
                const float lift = lift_[Idx(i)].Value(now);
                if ((lift >= 0.001f) == (pass == 1)) {
                    DrawTile(i, x, lift, name_[Idx(i)].Value(now));
                }
            }
        }
        renderer_.PopClip();
    }

    // Placeholders for the tiles while the folder is being read, so the screen does not jump.
    void DrawSkeletonRail(Clock::time_point now) {
        const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
        const float phase = static_cast<float>(std::fmod(seconds, 1.8) / 1.8);
        const D2D1_COLOR_F fill =
            WithOpacity(Theme::kSurface, 0.775f + 0.225f * std::sin(phase * 6.2831853f));
        for (int i = 0; i < kSkeletonTiles; ++i) {
            const float x = kMargin + static_cast<float>(i) * kTilePitch;
            renderer_.FillRounded(RectF(x, kRailTop, x + kTileSize, kRailTop + kTileSize),
                                  kTileRadius, fill);
        }
    }

    // One tile of the rail. A focused tile lifts, tilts and gets the ring, all at once, and the
    // game's name under it goes along.
    void DrawTile(int index, float x, float lift, float name_opacity) {
        const D2D1_RECT_F rect = RectF(x, kRailTop, x + kTileSize, kRailTop + kTileSize);
        const D2D1_POINT_2F center = Point2F(x + kTileSize / 2.0f, kRailTop + kTileSize / 2.0f);
        const float scale = 1.0f + kLiftScale * lift;
        renderer_.SetLocalTransform(D2D1::Matrix3x2F::Scale(scale, scale, center) *
                                    D2D1::Matrix3x2F::Rotation(kLiftTilt * lift, center) *
                                    D2D1::Matrix3x2F::Translation(0.0f, -kLiftRise * lift));
        if (index == GameCount()) {
            DrawAddTile(rect);
        } else {
            DrawGameTile(index, rect);
            renderer_.DrawString(
                Game(index).name, Font::Caption,
                RectF(x, rect.bottom + kNameOffset, x + kNameWidth,
                      rect.bottom + kNameOffset + kNameHeight),
                WithOpacity(Theme::kText, name_opacity), HAlign::Left, VAlign::Top);
        }
        if (layer_ == Layer::Rail && index == selected_) {
            DrawRing(renderer_, rect, kTileRadius, lift);
        }
        renderer_.ClearLocalTransform();
    }

    void DrawGameTile(int index, const D2D1_RECT_F& rect) {
        const Visual& visual = EnsureVisual(index);
        if (visual.image.brush) {
            renderer_.DrawImage(visual.image, rect, kTileRadius, 1.0f);
            return;
        }
        // No icon in the package: the first letter of the name on a translucent surface.
        renderer_.FillRounded(rect, kTileRadius, Theme::kSurface);
        renderer_.StrokeRounded(Inflate(rect, -0.5f), kTileRadius - 0.5f, 1.0f, Theme::kHairline);
        renderer_.DrawString(Game(index).name.substr(0, 1), Font::TitleSmall, rect,
                             Theme::kTextTertiary, HAlign::Center, VAlign::Middle);
    }

    // A translucent surface with a plus sign and its two-line label.
    void DrawAddTile(const D2D1_RECT_F& rect) {
        renderer_.FillRounded(rect, kTileRadius, Theme::kSurface);
        renderer_.StrokeRounded(Inflate(rect, -0.5f), kTileRadius - 0.5f, 1.0f, Theme::kHairline);
        // The plus (46 px box, strokes of 3 px) over the label (two lines of 23.4 px), 14 px apart,
        // the pair centered in the tile.
        constexpr float kPlusBox = 46.0f;
        constexpr float kPlusArm = 13.4f; // half of the 26.8 px stroke length
        constexpr float kPlusStroke = 3.0f;
        constexpr float kLabelGap = 14.0f;
        constexpr float kLabelHeight = 46.8f;
        const float cx = (rect.left + rect.right) / 2.0f;
        const float top =
            (rect.top + rect.bottom) / 2.0f - (kPlusBox + kLabelGap + kLabelHeight) / 2.0f;
        const float cy = top + kPlusBox / 2.0f;
        const D2D1_COLOR_F plus = WithOpacity(Theme::kTextSecondary, 0.6f);
        renderer_.FillRounded(RectF(cx - kPlusArm, cy - kPlusStroke / 2.0f, cx + kPlusArm,
                                    cy + kPlusStroke / 2.0f),
                              kPlusStroke / 2.0f, plus);
        renderer_.FillRounded(RectF(cx - kPlusStroke / 2.0f, cy - kPlusArm,
                                    cx + kPlusStroke / 2.0f, cy + kPlusArm),
                              kPlusStroke / 2.0f, plus);
        const float label_top = top + kPlusBox + kLabelGap;
        renderer_.DrawString(Tr(Text::AddGamesTile), Font::TileLabel,
                             RectF(rect.left, label_top, rect.right, label_top + kLabelHeight),
                             Theme::kTextSecondary, HAlign::Center, VAlign::Top);
    }

    // A short message in a glass capsule in the top row, fading in and out.
    void DrawToast(Clock::time_point now) {
        if (toast_.empty() || now >= toast_until_) {
            return;
        }
        const float remaining = std::chrono::duration<float>(toast_until_ - now).count();
        const float elapsed = std::chrono::duration<float>(kToastDuration).count() - remaining;
        DrawToastCapsule(renderer_, toast_,
                         std::clamp(std::min(elapsed / 0.15f, remaining / 0.22f), 0.0f, 1.0f));
    }

    bool HasUpdate() const {
        const auto state = updater_.State();
        return state != UpdateState::Checking && state != UpdateState::None;
    }

    void DrawUpdatePill() {
        const std::wstring label = Tr(Text::UpdateAvailable);
        const float width = PillWidth(renderer_, label, false);
        const float right = kCanvasWidth - kMargin - 190.0f;
        DrawChoicePill(renderer_,
                       RectF(right - width, kTopCenter - kPillHeight / 2.0f, right,
                             kTopCenter + kPillHeight / 2.0f),
                       label, false, layer_ == Layer::Nav && update_focused_);
    }

    void DrawUpdateSheet() {
        const auto state = updater_.State();
        Text message = Text::UpdateDownloading;
        if (state == UpdateState::Installing)
            message = Text::UpdateInstalling;
        else if (state == UpdateState::Restarting)
            message = Text::UpdateRestarting;
        else if (state == UpdateState::Failed)
            message = Text::UpdateFailed;
        else if (state == UpdateState::MissingPortal)
            message = Text::UpdateMissingPortal;
        DrawSheet(renderer_,
                  RectF(kSheetLeft, kSheetTop, kSheetLeft + kSheetWidth, kSheetTop + kSheetHeight));
        renderer_.DrawString(Tr(Text::UpdateAvailable), Font::Heading,
                             RectF(476.0f, 290.0f, 1444.0f, 380.0f), Theme::kText);
        renderer_.DrawString(Tr(message), Font::Body, RectF(476.0f, 400.0f, 1444.0f, 610.0f),
                             Theme::kTextSecondary);
        if (state == UpdateState::Downloading) {
            DrawProgressBar(renderer_, RectF(476.0f, 650.0f, 1250.0f, 664.0f), updater_.Progress());
            renderer_.DrawString(
                std::to_wstring(static_cast<int>(updater_.Progress() * 100)) + L"%", Font::MetaMono,
                RectF(1280.0f, 630.0f, 1444.0f, 686.0f), Theme::kText);
        }
    }

    std::vector<Hint> CurrentHints() const {
        if (move_active_)
            return {};
        if (update_open_) {
            const auto state = updater_.State();
            if (state == UpdateState::Failed || state == UpdateState::MissingPortal) {
                return {{Theme::kButtonA, L"A", Tr(Text::UpdateAction)},
                        {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
            }
            return {};
        }
        if (layer_ == Layer::Nav && update_focused_) {
            return {{Theme::kButtonA, L"A", Tr(Text::UpdateAction)},
                    {Theme::kButtonB, L"B", Tr(Text::HintQuit)}};
        }
        if (resolution_open_) {
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        }
        if (delete_stage_) {
            if (delete_stage_ == 1 || delete_stage_ == 4)
                return {};
            if (delete_stage_ == 3)
                return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                        {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonX, L"X", Tr(Text::DeleteReview)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        }
        if (details_open_) {
            return {{Theme::kButtonA, L"A", Tr(Text::DeleteGame)},
                    {Theme::kButtonX, L"X", Tr(Text::MoveToHd)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        }
        std::vector<Hint> hints;
        if (return_home_)
            hints.push_back({Theme::kTextSecondary, L"R", Tr(Text::HintStickMotion)});
        if (tab_ == Tab::Settings) {
            if (layer_ == Layer::Rail &&
                (!renderer_open_ || RendererChoiceEnabled(renderer_choice_)))
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSelect)});
            hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintBack)});
            return hints;
        } else if (HasGame()) {
            if (layer_ == Layer::Rail) {
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::ActionPlay)});
            } else if (layer_ == Layer::Actions) {
                hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintSelect)});
            }
            hints.push_back({Theme::kButtonX, L"X", Tr(Text::ActionMods)});
            hints.push_back({Theme::kButtonY, L"Y", Tr(Text::ActionDetails)});
        } else if (OnAddTile() && layer_ == Layer::Rail) {
            hints.push_back({Theme::kButtonA, L"A", Tr(Text::HintImport)});
        }
        hints.push_back({Theme::kButtonB, L"B", Tr(Text::HintQuit)});
        return hints;
    }

    // Settings shares the Shop sidebar and plain rows, with a separate focus for each pane.
    void DrawSettings() {
        renderer_.DrawString(Tr(Text::SettingsTitle), Font::Heading,
                             RectF(kMargin + 28.0f, 210.0f, kCanvasWidth - kMargin, 310.0f),
                             Theme::kText);
        struct Row {
            const wchar_t* name;
            const wchar_t* state;
            D2D1_COLOR_F state_color;
            const wchar_t* hint;
        };
        Row sync_row = {Tr(Text::SyncRowTitle), Tr(Text::SyncStateOff), Theme::kTextSecondary,
                        Tr(Text::SyncRowOffHint)};
        switch (sync_account_) {
        case SyncAccount::SignedIn:
            sync_row.state = Tr(Text::SyncStateOn);
            sync_row.state_color = Theme::kSuccessText;
            sync_row.hint = Tr(Text::SyncRowOnHint);
            break;
        case SyncAccount::NotConfigured:
            sync_row.state = Tr(Text::SyncStateNotConfigured);
            sync_row.state_color = Theme::kTextTertiary;
            sync_row.hint = Tr(Text::SyncRowMissingHint);
            break;
        case SyncAccount::SignedOut:
            break;
        }
        Row sources_row = {Tr(Text::SourcesRowTitle), Tr(Text::SourcesRowState),
                           Theme::kTextTertiary, Tr(Text::SourcesRowHint)};
        std::wstring sources_state;
        if (sources_count_ > 0) {
            sources_state = std::to_wstring(sources_count_) + L" " +
                            Tr(sources_count_ == 1 ? Text::SourcesUnitOne : Text::SourcesUnitMany);
            sources_row.state = sources_state.c_str();
            sources_row.state_color = Theme::kSuccessText;
        }
        const Row usb_row = {Tr(Text::UsbSetting), UsbModeLabel(usb_mode_), Theme::kTextSecondary,
                             Tr(Text::UsbSettingHint)};
        const Row credits_row = {Tr(Text::CreditsRowTitle), Tr(Text::CreditsRowState),
                                 Theme::kTextSecondary, Tr(Text::CreditsRowHint)};
        const Row nextendo_row = {
            Tr(Text::NextendoTitle),
            Tr(Service::Nextendo::IsEnabled() ? Text::NextendoOn : Text::NextendoOff),
            Theme::kTextSecondary, Tr(Text::NextendoRowHint)};
        static const std::wstring build =
            winrt::to_hstring(Common::g_build_version).c_str() + std::wstring(L" · ") +
            winrt::to_hstring(std::string(Common::g_scm_rev).substr(0, 8)).c_str();
        const int resolution = settings_resolution_;
        const Text resolution_label = resolution == 1   ? Text::Resolution0_5X
                                      : resolution == 2 ? Text::Resolution0_75X
                                      : resolution == 5 ? Text::Resolution1_5X
                                      : resolution == 6 ? Text::Resolution2X
                                      : resolution == 7 ? Text::Resolution3X
                                                        : Text::Resolution1X;
        const bool ftp_running = ftp_.Running();
        const auto ftp_address = ftp_running ? ftp_.Address() : Tr(Text::NextendoOff);
        const auto ftp_credentials = ftp_running
                                         ? std::wstring(Tr(Text::FtpUser)) + L" nxbox · " +
                                               Tr(Text::FtpPassword) + L" " + ftp_.Password()
                                         : std::wstring(Tr(Text::FtpHint));
        const std::array<Row, static_cast<std::size_t>(Setting::Count)> rows = {
            {sync_row,
             sources_row,
             usb_row,
             credits_row,
             nextendo_row,
             {Tr(Text::SettingsMultiplayer), Tr(Text::HintSelect), Theme::kTextSecondary,
              Tr(Text::SettingsMultiplayerHint)},
             {Tr(Text::InternetArtwork), Tr(internet_art_ ? Text::NextendoOn : Text::NextendoOff),
              Theme::kTextSecondary, Tr(Text::InternetArtworkHint)},
             {Tr(Text::SystemMenuTitle), Tr(Text::SystemMenuBoot), Theme::kTextSecondary,
              Tr(Text::SystemMenuHint)},
             {Tr(Text::OfficialSwitchInterface),
              Tr(official_interface_ ? Text::NextendoOn : Text::NextendoOff), Theme::kTextSecondary,
              Tr(firmware_status_.home_available ? Text::SystemMenuHint : Text::RequiresFirmware)},
             {Tr(Text::ProfilePicture), Tr(Text::HintSelect), Theme::kTextSecondary,
              Tr(Text::ProfilePictureHint)},
             {Tr(Text::MultipleAccounts),
              Tr(accounts_multi_ ? Text::NextendoOn : Text::NextendoOff), Theme::kTextSecondary,
              Tr(Text::MultipleAccountsHint)},
             {Tr(Text::MoruTitle), Tr(Text::HintSelect), Theme::kTextSecondary,
              Tr(Text::MoruSettingsHint)},
             {Tr(Text::BackToNXboxInterface), Tr(Text::HintSelect), Theme::kTextSecondary,
              Tr(Text::BackToNXboxHint)},
             {Tr(Text::RendererTitle),
              Tr(renderer_backend_ == RendererSettings::kOpenGL ? Text::RendererOpenGL
                                                                : Text::RendererDirectX),
              Theme::kTextSecondary, Tr(Text::RendererNextLaunch)},
             {Tr(Text::ShaderPreloadTitle),
              Tr(shader_preload_ ? Text::NextendoOn : Text::NextendoOff), Theme::kTextSecondary,
              Tr(Text::ShaderPreloadHint)},
             {Tr(Text::AsyncShadersTitle),
              Tr(async_shaders_ ? Text::NextendoOn : Text::NextendoOff), Theme::kTextSecondary,
              Tr(Text::AsyncShadersHint)},
             {Tr(Text::XboxButtonIconsTitle),
              Tr(xbox_button_icons_ ? Text::NextendoOn : Text::NextendoOff), Theme::kTextSecondary,
              Tr(Text::XboxButtonIconsHint)},
             {Tr(Text::ButtonLayoutTitle),
              Tr(button_layout_ == ButtonLayout::Position ? Text::ButtonLayoutPositionShort
                                                          : Text::ButtonLayoutNameShort),
              Theme::kTextSecondary, Tr(Text::ButtonLayoutHint)},
             {Tr(Text::ResolutionTitle), Tr(resolution_label), Theme::kTextSecondary,
              Tr(Text::ResolutionMemoryHint)},
             {Tr(Text::UpscalerTitle), Tr(fsr_ ? Text::UpscalerFsr : Text::UpscalerBilinear),
              Theme::kTextSecondary, Tr(Text::UpscalerHint)},
             {Tr(Text::ShowFps), Tr(show_fps_ ? Text::NextendoOn : Text::NextendoOff),
              Theme::kTextSecondary, Tr(Text::ShowFpsHint)},
             {Tr(Text::SettingsVersion), build.c_str(), Theme::kTextSecondary,
              Tr(Text::SettingsBuildHint)},
             {Tr(Text::FtpServer), ftp_address.c_str(), Theme::kTextSecondary,
              ftp_credentials.c_str()}}};
        constexpr float kRowTop = 340.0f;
        const float kRowPitch = kSettingsCategories[Idx(settings_category_)].count > 6   ? 92.0f
                                : kSettingsCategories[Idx(settings_category_)].count > 5 ? 106.0f
                                                                                         : 128.0f;
        constexpr float kRight = kCanvasWidth - kMargin;
        constexpr float kRowsLeft = 560.0f;
        for (std::size_t category = 0; category < kSettingsCategories.size(); ++category) {
            const float y = kRowTop + static_cast<float>(category) * 88.0f;
            const auto rect = RectF(kMargin, y, 510.0f, y + 72.0f);
            if (static_cast<int>(category) == settings_category_) {
                renderer_.FillRounded(rect, 36.0f, Theme::kSurfaceStrong);
                if (layer_ == Layer::Rail && !settings_rows_focused_)
                    DrawRing(renderer_, rect, 36.0f + kRingGap, 1.0f);
            }
            renderer_.DrawString(Tr(kSettingsCategories[category].title), Font::RowTitle,
                                 RectF(kMargin + 28.0f, y, 490.0f, y + 72.0f), Theme::kText,
                                 HAlign::Left, VAlign::Middle);
        }
        const std::array<Row, 3> renderer_rows{{
            {Tr(Text::RendererDirectX), Tr(Text::RendererRecommended), Theme::kTextSecondary,
             Tr(Text::RendererNextLaunch)},
            {Tr(Text::RendererOpenGL), Tr(Text::RendererCompatibility), Theme::kTextSecondary,
             Tr(Text::RendererNextLaunch)},
            {Tr(Text::RendererVulkan), L"", Theme::kTextTertiary, Tr(Text::RendererVulkanHint)},
        }};
        const int row_count =
            renderer_open_ ? 3 : kSettingsCategories[Idx(settings_category_)].count;
        const int first_row = renderer_open_ ? 0 : SettingsFirstVisibleRow(settings_row_);
        for (int i = first_row; i < std::min(row_count, first_row + 7); ++i) {
            const auto setting = kSettingsCategories[Idx(settings_category_)].rows[Idx(i)];
            const Row& item =
                renderer_open_ ? renderer_rows[Idx(i)] : rows[static_cast<std::size_t>(setting)];
            const float y = kRowTop + static_cast<float>(i - first_row) * kRowPitch;
            const auto rect = RectF(kRowsLeft, y, kRight, y + kRowPitch - 16.0f);
            const bool disabled = renderer_open_ ? !RendererChoiceEnabled(i)
                                                 : setting == Setting::Version ||
                                                       ((setting == Setting::OfficialInterface ||
                                                         setting == Setting::SystemMenu) &&
                                                        !firmware_status_.home_available);
            const bool focused = layer_ == Layer::Rail && settings_rows_focused_ &&
                                 (renderer_open_ ? renderer_choice_ : settings_row_) == i;
            if (focused)
                renderer_.FillRounded(rect, (kRowPitch - 16.0f) / 2.0f, Theme::kSurfaceStrong);
            const bool layout_row = !renderer_open_ && setting == Setting::ButtonLayout;
            const float text_left = kRowsLeft + (layout_row ? 124.0f : 28.0f);
            if (layout_row)
                DrawXboxButtonDiamond(renderer_, Point2F(kRowsLeft + 60.0f, y + 44.0f), 25.0f,
                                      12.0f, Font::Glyph, button_layout_);
            renderer_.DrawString(
                item.name, Font::RowTitle,
                RectF(text_left, y + 12.0f,
                      renderer_open_ && i == 2 ? kRight - 28.0f : kRowsLeft + 620.0f, y + 54.0f),
                disabled ? Theme::kTextTertiary : Theme::kText);
            renderer_.DrawString(item.state, Font::RowSub,
                                 RectF(kRowsLeft + 630.0f, y + 12.0f, kRight - 28.0f, y + 54.0f),
                                 disabled ? Theme::kTextTertiary : item.state_color, HAlign::Right,
                                 VAlign::Middle);
            renderer_.DrawString(item.hint, Font::RowSub,
                                 RectF(text_left, y + 54.0f, kRight - 28.0f, y + kRowPitch - 20.0f),
                                 Theme::kTextSecondary);
            if (focused)
                DrawRing(renderer_, rect, (kRowPitch - 16.0f) / 2.0f + kRingGap, 1.0f);
        }
        if (firmware_status_.version.empty() &&
            kSettingsCategories[Idx(settings_category_)].title == Text::SettingsSystem) {
            float folder_y = kRowTop + static_cast<float>(row_count) * kRowPitch + 8.0f;
            renderer_.DrawString(Tr(Text::FirmwareSearchedFolders), Font::RowSub,
                                 RectF(kRowsLeft, folder_y, kRight, folder_y + 32.0f),
                                 Theme::kText);
            for (const auto& folder : firmware_status_.searched_folders) {
                folder_y += 32.0f;
                renderer_.DrawString(folder.wstring(), Font::MetaMono,
                                     RectF(kRowsLeft, folder_y, kRight, folder_y + 32.0f),
                                     Theme::kTextSecondary);
            }
        }
    }

    // What the package says about the focused game, in a sheet over the dimmed screen.
    void DrawDetails() {
        const GameEntry& game = Game(selected_);
        renderer_.FillRounded(RectF(0.0f, 0.0f, kCanvasWidth, kCanvasHeight), 0.0f, Theme::kVeil);
        const D2D1_RECT_F sheet = RectF(kSheetLeft, kSheetTop, kSheetLeft + kSheetWidth,
                                        kSheetTop + kSheetHeight + 100.0f);
        renderer_.FillRounded(sheet, 32.0f, Theme::kSheet);
        renderer_.StrokeRounded(Inflate(sheet, -0.5f), 31.5f, 1.0f, Theme::kHairline);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(
            game.name, Font::Heading,
            RectF(left, sheet.top + 40.0f, right, sheet.top + kSheetRowTop - 16.0f), Theme::kText,
            HAlign::Left, VAlign::Middle);

        const std::filesystem::path relative = game.path.lexically_relative(local_state_);
        const std::wstring file = relative.empty() ? game.path.wstring() : relative.wstring();
        const SizeText size = FormatSize(game.size);
        struct Row {
            const wchar_t* label;
            std::wstring value;
            Font font;
        };
        const std::array<Row, 4> rows = {{
            {Tr(Text::DetailsTitleId), Widen(game.title_id), Font::MetaMono},
            {Tr(Text::DetailsFormat), FormatName(game), Font::MetaMono},
            {Tr(Text::DetailsSize), size.amount + L" " + size.unit, Font::MetaMono},
            {Tr(Text::DetailsFile), file, Font::Meta},
        }};
        float y = sheet.top + kSheetRowTop;
        for (const Row& row : rows) {
            renderer_.FillRounded(RectF(left, y, right, y + 1.0f), 0.0f, Theme::kHairline);
            renderer_.DrawString(row.label, Font::Meta,
                                 RectF(left, y, left + kSheetLabelWidth, y + kSheetRowHeight),
                                 Theme::kTextSecondary, HAlign::Left, VAlign::Middle);
            renderer_.DrawString(row.value, row.font,
                                 RectF(left + kSheetLabelWidth, y, right, y + kSheetRowHeight),
                                 Theme::kText, HAlign::Left, VAlign::Middle);
            y += kSheetRowHeight;
        }
        DrawChoicePill(renderer_, RectF(left, y + 16.0f, left + 310.0f, y + 16.0f + kPillHeight),
                       Tr(Text::DeleteGame), false, !move_active_);
        DrawChoicePill(renderer_, RectF(left + 340.0f, y + 16.0f, right, y + 16.0f + kPillHeight),
                       Tr(Text::MoveToHd), false, move_active_);
        if (move_active_) {
            const auto total = move_progress_.total.load();
            const auto done = move_progress_.done.load();
            const auto percent = total ? static_cast<int>(100.0 * static_cast<double>(done) /
                                                          static_cast<double>(total))
                                       : 0;
            renderer_.DrawString(
                std::wstring(Tr(Text::MoveCopying)) + L" " + std::to_wstring(percent) + L"%",
                Font::MetaMono, RectF(left, y + 90.0f, right, y + 130.0f), Theme::kText);
        }
    }

    void DrawDelete() {
        const auto sheet = RectF(250.0f, 120.0f, 1670.0f, 950.0f);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(Tr(Text::DeleteGame), Font::Heading,
                             RectF(left, 145.0f, right, 205.0f), Theme::kText);
        const Text message = delete_stage_ == 1   ? Text::DeleteListing
                             : delete_stage_ == 4 ? Text::DeleteBusy
                             : delete_stage_ == 3 ? Text::DeleteConfirm
                                                  : Text::DeleteChoose;
        renderer_.DrawString(Tr(message), Font::Body, RectF(left, 215.0f, right, 285.0f),
                             Theme::kTextSecondary);
        if (delete_stage_ == 1 || delete_stage_ == 4)
            return;
        std::uint64_t bytes = 0;
        for (const auto& file : delete_files_)
            if (file.selected)
                bytes += file.size;
        const auto total = FormatSize(bytes);
        renderer_.DrawString(total.amount + L" " + total.unit, Font::MetaMono,
                             RectF(left, 290.0f, right, 335.0f), Theme::kText);
        const int first = (delete_row_ / 6) * 6;
        float y = 345.0f;
        for (int i = first; i < std::min(first + 6, static_cast<int>(delete_files_.size())); ++i) {
            const auto& file = delete_files_[Idx(i)];
            const auto size = FormatSize(file.size);
            const auto label =
                std::wstring(file.selected ? L"[+] " : L"[ ] ") + file.path.filename().wstring();
            renderer_.DrawString(label, Font::Meta, RectF(left, y, right - 210.0f, y + 32.0f),
                                 i == delete_row_ ? Theme::kText : Theme::kTextSecondary);
            renderer_.DrawString(size.amount + L" " + size.unit, Font::MetaMono,
                                 RectF(right - 200.0f, y, right, y + 32.0f), Theme::kText);
            renderer_.DrawString(file.path.parent_path().wstring(), Font::RowSub,
                                 RectF(left, y + 32.0f, right, y + 64.0f), Theme::kTextSecondary);
            y += 70.0f;
        }
        renderer_.DrawString(
            std::to_wstring(delete_row_ + 1) + L" / " + std::to_wstring(delete_files_.size()),
            Font::MetaMono, RectF(left, 780.0f, right, 825.0f), Theme::kTextSecondary);
        if (delete_stage_ == 3) {
            DrawChoicePill(renderer_, RectF(left, 850.0f, left + 260.0f, 910.0f),
                           Tr(Text::DeleteCancel), false, !delete_confirm_);
            DrawChoicePill(renderer_, RectF(left + 290.0f, 850.0f, left + 590.0f, 910.0f),
                           Tr(Text::DeleteGame), false, delete_confirm_);
        } else {
            renderer_.DrawString(Tr(Text::DeleteReviewHint), Font::Body,
                                 RectF(left, 850.0f, right, 920.0f), Theme::kText);
        }
    }

    void DrawResolution() {
        const D2D1_RECT_F sheet =
            RectF(kSheetLeft, kSheetTop, kSheetLeft + kSheetWidth, kSheetTop + kSheetHeight);
        DrawSheet(renderer_, sheet);
        const float left = sheet.left + kSheetPadding;
        const float right = sheet.right - kSheetPadding;
        renderer_.DrawString(Tr(Text::ResolutionTitle), Font::Heading,
                             RectF(left, sheet.top + 40.0f, right, sheet.top + 110.0f),
                             Theme::kText);
        renderer_.DrawString(resolution_global_ ? Tr(Text::SettingsGlobalResolution)
                                                : Game(selected_).name.c_str(),
                             Font::Meta, RectF(left, sheet.top + 115.0f, right, sheet.top + 155.0f),
                             Theme::kTextSecondary);
        const std::array<Text, 6> labels = {Text::Resolution0_5X, Text::Resolution0_75X,
                                            Text::Resolution1X,   Text::Resolution1_5X,
                                            Text::Resolution2X,   Text::Resolution3X};
        float x = left;
        for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
            const std::wstring label = Tr(labels[Idx(i)]);
            const float width = PillWidth(renderer_, label, false);
            DrawChoicePill(
                renderer_,
                RectF(x, sheet.top + 200.0f, x + width, sheet.top + 200.0f + kPillHeight), label,
                false, resolution_choice_ == i);
            x += width + kPillGap;
        }
        if (TitleSettings::kResolutionValues[Idx(resolution_choice_)] >= 6) {
            renderer_.DrawString(Tr(Text::ResolutionMemoryHint), Font::Body,
                                 RectF(left, sheet.top + 300.0f, right, sheet.top + 380.0f),
                                 Theme::kTextSecondary);
        }
        if (resolution_save_failed_) {
            renderer_.DrawString(Tr(Text::ResolutionSaveFailed), Font::Meta,
                                 RectF(left, sheet.top + 400.0f, right, sheet.bottom - 24.0f),
                                 Theme::kTextSecondary);
        }
    }

    // ---- Members ----

    Renderer& renderer_;
    CoreWindow window_;
    Input input_;
    std::filesystem::path local_state_;
    std::unique_ptr<ShopScreen> shop_;
    Ftp::Server ftp_;
    LibraryMoveProgress move_progress_;
    std::future<LibraryTransfer::Result> move_result_;
    bool move_active_ = false;
    std::uint64_t ftp_changes_ = 0;
    bool internet_art_ = true;
    std::unique_ptr<BackgroundArtSource> backgrounds_;
    int sources_count_ = 0; // sources saved by the Sources screen, shown on its Settings row
    Updater updater_;
    bool update_focused_ = false;
    bool update_open_ = false;

    UsbMode usb_mode_ = UsbMode::Unset;
    std::unique_ptr<UsbDetection> usb_detection_;
    FirmwareStatus firmware_status_;
    std::unique_ptr<LibraryScan> scan_; // set while the folder is being read
    std::string keep_title_id_;         // the game to focus again when the scan ends
    bool keys_missing_ = false;         // the last scan found games it could not open for want of keys
    std::vector<GameEntry> games_;
    std::vector<Visual> visuals_;
    std::vector<Tween> lift_; // per rail item: how lifted it is (0..1)
    std::vector<Tween> name_; // per rail item: how visible the name under it is (0..1)
    Tween scroll_;

    Tab tab_ = Tab::Library;
    Layer layer_ = Layer::Rail;
    int tinfoil_index_ = -1;
    int selected_ = 0; // rail index; GameCount() is the "add games" tile
    int action_ = kActionPlay;
    std::atomic<bool>* return_home_ = nullptr;
    std::atomic<bool>* guest_home_request_ = nullptr;
    bool official_interface_ = false;
    bool accounts_multi_ = false;
    int settings_row_ = 0;
    int settings_category_ = 0;
    bool settings_rows_focused_ = false;
    bool renderer_open_ = false;
    int renderer_choice_ = 0;
    bool resolution_global_ = false;
    bool shader_preload_ = true;
    bool async_shaders_ = true;
    bool fsr_ = true;
    bool xbox_button_icons_ = true;
    ButtonLayout button_layout_ = ButtonLayout::Name;
    int renderer_backend_ = RendererSettings::kDirectX12;
    int settings_resolution_ = TitleSettings::kDefaultResolution;
    SyncAccount sync_account_ = SyncAccount::SignedOut;
    int launched_ = -1;
    bool details_open_ = false;
    int delete_stage_ = 0; // 1: listing, 2: choices, 3: confirmation, 4: deleting
    int delete_row_ = 0;
    bool delete_confirm_ = false;
    std::vector<LibraryDeleteFile> delete_files_;
    std::future<std::vector<LibraryDeleteFile>> delete_candidates_;
    std::future<bool> delete_result_;
    bool resolution_open_ = false;
    bool resolution_save_failed_ = false;
    int resolution_choice_ = 0;

    Look hero_from_;
    Look hero_to_;
    Tween hero_blend_{1.0f};
    int hero_key_ = -2;

    std::wstring toast_;
    Clock::time_point toast_until_{};

    bool show_fps_ = false;
    bool closed_ = false;
    bool leaving_ = false;
    bool launching_ = false;
    Clock::time_point launch_started_{};
    std::string result_;
};

} // namespace

void RunSessionOverlay(const winrt::Windows::UI::Core::CoreWindow& window,
                       std::atomic<bool>& return_home, std::atomic<bool>& guest_home_request) {
    const std::filesystem::path local_state(std::wstring_view(
        winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
    Renderer renderer;
    renderer.Initialize(window, true);
    LibraryScreen screen(renderer, window, local_state, &return_home, &guest_home_request);
    screen.Run();
}

std::string RunLibrary(const winrt::Windows::UI::Core::CoreWindow& window, ChosenGame* chosen) {
    Diagnostic("UI library begin");
    std::string path;
    try {
        const std::filesystem::path local_state(std::wstring_view(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
        Renderer renderer;
        renderer.Initialize(window);
        bool window_closed = false;
        if (!IsSetupDone(local_state) || !RendererSettings::HasButtonLayout(local_state)) {
            // Existing installs only see the missing layout choice; sync is not repeated.
            Input setup_input(window);
            window_closed = RunFirstRunSetup(renderer, window, setup_input, local_state);
        }
        if (!window_closed) {
            // The screen owns GPU images, so it has to be gone before the renderer is.
            LibraryScreen screen(renderer, window, local_state);
            path = screen.Run();
            if (!path.empty() && chosen != nullptr) {
                if (const GameEntry* game = screen.Chosen()) {
                    chosen->path = path;
                    chosen->title_id = game->title_id;
                    // SwitchSaveSync names the cloud folder after the first NACP name, not after
                    // the name in the UI language.
                    chosen->sync_name = SaveSync::GameNameFromNames(
                        game->nacp_names, std::stoull(game->title_id, nullptr, 16));
                    chosen->display_name = game->name;
                }
            }
        }
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI library failed " + winrt::to_string(error.message()));
        path.clear();
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI library failed ") + error.what());
        path.clear();
    }
    Diagnostic(path.empty() ? "UI library end, no game chosen" : "UI library end");
    return path;
}

} // namespace EdenXbox::Ui
