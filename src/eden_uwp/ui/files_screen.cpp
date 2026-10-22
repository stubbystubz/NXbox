// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <winrt/Windows.Foundation.h>
#include "eden_uwp/file_manager.h"
#include "eden_uwp/ui/files_screen.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/theme.h"

namespace EdenXbox::Ui {
using D2D1::RectF;
struct FilesScreen::Impl {
    Renderer& renderer;
    Input& input;
    std::filesystem::path local;
    Ftp::Server& ftp;
    std::vector<Files::Root> roots;
    std::vector<Files::Entry> entries;
    int root = 0, row = 0, action = 0;
    bool pane = false, menu = false, deleting = false, overwrite = false;
    bool destination = false, moving = false, creating = false;
    Files::Location source;
    unsigned source_root = 0;
    bool source_directory = false, target_directory = false;
    std::wstring relative;
    Files::Sort sort = Files::Sort::Name;
    Files::Progress progress;
    std::atomic<unsigned> ftp_generation{0};
    Tween focus_y{340.0f};
    std::unique_ptr<TextEntry> keyboard;
    Text message = Text::FilesReady;
    struct Result {
        bool ok = true, changed = false, clash = false, directory = false;
        Ui::Text failure = Ui::Text::FilesFailed;
        std::optional<std::vector<Files::Root>> roots;
        std::optional<std::vector<Files::Entry>> entries;
    };
    std::future<Result> work;
    Impl(Renderer& renderer, Input& input, const std::filesystem::path& local, Ftp::Server& ftp)
        : renderer(renderer), input(input), local(local), ftp(ftp), roots{{local, {}}} {}
    bool Busy() const {
        return work.valid();
    }
    bool FtpSelected() const {
        return root == static_cast<int>(roots.size());
    }
    Files::Location Here() const {
        return {roots.at(root), relative};
    }
    Files::Location Selected() const {
        return {roots.at(root), Files::Join(relative, entries.at(row).name)};
    }
    void Start(std::function<Result()> task) {
        progress.cancel = false;
        progress.done = 0;
        progress.total = 0;
        message = Text::FilesWorking;
        work = std::async(std::launch::async, [task = std::move(task)] {
            Result result;
            bool apartment = false;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                apartment = true;
                result = task();
            } catch (...) {
                result.ok = false;
            }
            if (apartment)
                winrt::uninit_apartment();
            return result;
        });
    }
    void RefreshRoots() {
        if (Busy() || destination)
            return;
        Start([this] {
            Result result;
            result.roots = Files::Roots(local);
            return result;
        });
    }
    void Browse() {
        row = 0;
        const auto location = Here();
        entries.clear();
        Start([location] {
            Result result;
            result.entries = Files::List(location);
            return result;
        });
    }
    // Even failed operations can have changed part of a tree. Refresh and notify conservatively.
    void Mutate(std::function<void()> task) {
        const auto location = Here();
        Start([location, task = std::move(task)] {
            Result result;
            result.changed = true;
            try {
                task();
            } catch (...) {
                result.ok = false;
            }
            try {
                result.entries = Files::List(location);
            } catch (...) {
                result.ok = false;
            }
            return result;
        });
    }
    void Transfer(bool replace) {
        const auto target = Here();
        const auto from = source;
        const auto from_root = source_root;
        const auto to_root = static_cast<unsigned>(root);
        const auto directory = source_directory;
        const auto move = moving;
        destination = false;
        overwrite = false;
        Mutate([this, target, from, from_root, to_root, directory, move, replace] {
            const auto plan = Files::PlanTransfer(
                from_root, from.relative, to_root, target.relative, directory, move, replace,
                [&](const auto& path) { return Files::Exists({target.root, path}); });
            if (!plan)
                throw std::runtime_error("invalid transfer destination");
            Files::Transfer(from, {target.root, plan->destination}, plan->move, plan->replace,
                            progress);
        });
    }
    void PrepareTransfer() {
        const auto target = Here();
        const auto filename = std::filesystem::path(source.relative).filename().wstring();
        Start([target, filename] {
            Result result;
            const Files::Location item{target.root, Files::Join(target.relative, filename)};
            result.clash = Files::Exists(item);
            result.directory = result.clash && Files::Directory(item);
            return result;
        });
        preparing = true;
    }
    bool preparing = false;
    void Tick(Clock::time_point now) {
        if (Busy() && work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const auto result = work.get();
            const bool cancelled = progress.cancel.load();
            if (result.changed)
                ftp.NotifyChanges();
            if (result.roots) {
                roots = *result.roots;
                root = 0;
                relative.clear();
                entries.clear();
                if (!roots.empty() && !cancelled)
                    Browse();
            }
            if (result.entries) {
                entries = *result.entries;
                Files::SortEntries(entries, sort);
                row = std::clamp(row, 0, std::max(0, static_cast<int>(entries.size()) - 1));
            }
            message = cancelled   ? Text::FilesCancelled
                      : result.ok ? Text::FilesReady
                                  : result.failure;
            if (preparing) {
                preparing = false;
                if (result.ok && !cancelled) {
                    if (result.clash) {
                        target_directory = result.directory;
                        overwrite = true;
                    } else
                        Transfer(false);
                }
            }
        }
        const float target = 340.0f + static_cast<float>(row % 8) * 72.0f;
        if (focus_y.Target() != target)
            focus_y.To(target, now, kDurationQuick);
    }
    bool Handle(Clock::time_point now) {
        if (Busy()) {
            if (input.Pressed(Button::B))
                progress.cancel = true;
            return false;
        }
        if (keyboard) {
            const auto result = keyboard->Update(input);
            if (result == TextEntry::Result::Editing)
                return false;
            if (result == TextEntry::Result::Confirmed) {
                const auto name = keyboard->Text();
                if (Files::ValidName(name)) {
                    const auto target =
                        Files::Location{roots.at(root), Files::Join(relative, name)};
                    if (creating)
                        Mutate([target] { Files::MakeDirectory(target); });
                    else {
                        const auto from = Selected();
                        Mutate([this, from, target] { Files::Rename(from, target, progress); });
                    }
                } else {
                    message = Text::FilesInvalidName;
                }
            }
            keyboard.reset();
            input.ConsumeUntilRelease();
            return false;
        }
        if (deleting || overwrite) {
            if (input.Pressed(Button::B)) {
                deleting = overwrite = false;
            } else if (input.Pressed(Button::Y) && overwrite) {
                Transfer(false);
            } else if (input.Pressed(Button::A)) {
                if (deleting) {
                    const auto target = Selected();
                    deleting = false;
                    Mutate([this, target] { Files::Remove(target, progress); });
                } else if (!source_directory && !target_directory) {
                    Transfer(true);
                }
            }
            return false;
        }
        if (menu) {
            if (input.Pressed(Button::B))
                menu = false;
            if (input.Pressed(Button::Up))
                action = std::max(entries.empty() ? 4 : 0, action - 1);
            if (input.Pressed(Button::Down))
                action = std::min(4, action + 1);
            if (input.Pressed(Button::A)) {
                menu = false;
                if (action == 4) {
                    creating = true;
                    keyboard = std::make_unique<TextEntry>(Tr(Text::FilesNewFolder), L"");
                } else if (!entries.empty()) {
                    if (action < 2) {
                        source = Selected();
                        source_root = static_cast<unsigned>(root);
                        source_directory = entries[row].directory;
                        moving = action == 1;
                        destination = true;
                        pane = false;
                    } else if (action == 2) {
                        creating = false;
                        keyboard =
                            std::make_unique<TextEntry>(Tr(Text::FilesRename), entries[row].name);
                    } else {
                        deleting = true;
                    }
                }
                input.ConsumeUntilRelease();
            }
            return false;
        }
        if (input.Pressed(Button::B)) {
            if (pane && !relative.empty()) {
                relative = std::filesystem::path(relative).parent_path().generic_wstring();
                Browse();
            } else if (pane) {
                pane = false;
            } else if (destination) {
                destination = false;
            } else {
                return true;
            }
            return false;
        }
        if (input.Pressed(Button::Left))
            pane = false;
        if (input.Pressed(Button::Right))
            pane = true;
        int delta = input.Pressed(Button::Down) ? 1 : input.Pressed(Button::Up) ? -1 : 0;
        if (input.Pressed(Button::LeftShoulder))
            delta = -8;
        if (input.Pressed(Button::RightShoulder))
            delta = 8;
        if (!pane) {
            if (root == 0 && input.Pressed(Button::Up) && !destination)
                return true;
            const auto next =
                std::clamp(root + delta, 0,
                           std::max(0, static_cast<int>(roots.size()) - (destination ? 1 : 0)));
            if (next != root) {
                root = next;
                relative.clear();
                entries.clear();
                if (!FtpSelected())
                    Browse();
            }
        } else {
            row = std::clamp(row + delta, 0, std::max(0, static_cast<int>(entries.size()) - 1));
        }
        if (input.Pressed(Button::A) && !Busy()) {
            if (!pane) {
                pane = true;
            } else if (FtpSelected()) {
                const auto generation = ftp_generation.load();
                Start([this, generation] {
                    Result result;
                    result.failure = Ui::Text::FtpStartFailed;
                    if (ftp.Running())
                        ftp.Stop();
                    else
                        result.ok = ftp.Start(local);
                    // Suspension may stop the server before this worker reaches Start.
                    if (generation != ftp_generation.load() || progress.cancel)
                        ftp.Stop();
                    return result;
                });
            } else if (!entries.empty() && entries[row].directory) {
                relative = Files::Join(relative, entries[row].name);
                Browse();
            }
        }
        if (input.Pressed(Button::X) && pane && !FtpSelected() && !Busy()) {
            if (destination)
                PrepareTransfer();
            else {
                menu = true;
                action = entries.empty() ? 4 : 0;
            }
        }
        if (input.Pressed(Button::Y) && !Busy() && !FtpSelected()) {
            sort = static_cast<Files::Sort>((static_cast<int>(sort) + 1) % 3);
            Files::SortEntries(entries, sort);
            row = 0;
        }
        (void)now;
        return false;
    }
    std::vector<Hint> Hints() const {
        if (keyboard)
            return {};
        if (Busy())
            return {{Theme::kButtonB, L"B", Tr(Text::HintCancel)}};
        if (deleting || overwrite || menu)
            return {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                    {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        return {
            {Theme::kButtonA, L"A", Tr(Text::HintSelect)},
            {Theme::kButtonB, L"B", Tr(Text::HintBack)},
            {Theme::kButtonX, L"X", Tr(destination ? Text::FilesUseFolder : Text::FilesActions)},
            {Theme::kButtonY, L"Y", Tr(Text::FilesSort)},
            {Theme::kTextSecondary, L"LB/RB", Tr(Text::FilesPage)}};
    }
    void DrawText(const std::wstring& text, Font font, float x, float y, float right,
                  D2D1_COLOR_F color = Theme::kText) {
        renderer.DrawString(text, font, RectF(x, y, right, y + 56.0f), color);
    }
    void Draw(Clock::time_point now, bool nav) {
        DrawText(Tr(Ui::Text::NavFiles), Font::Heading, kScreenMargin + 28, 210, 1800);
        const int count = static_cast<int>(roots.size()) + 1;
        const int first = std::max(0, root - 6);
        for (int i = first; i < std::min(count, first + 7); ++i) {
            const float y = 340.0f + (i - first) * 88.0f;
            const auto rect = RectF(kScreenMargin, y, 510, y + 72);
            if (i == root) {
                renderer.FillRounded(rect, 36, Theme::kSurfaceStrong);
                if (!nav && !pane)
                    DrawRing(renderer, rect, 36 + kRingGap, 1);
            }
            DrawText(i == static_cast<int>(roots.size()) ? Tr(Ui::Text::FilesFtp)
                     : i == 0                            ? Tr(Ui::Text::FilesConsole)
                                                         : roots[i].label,
                     Font::RowTitle, kScreenMargin + 28, y + 10, 490);
        }
        if (FtpSelected()) {
            const bool running = ftp.Running();
            DrawChoicePill(renderer, RectF(580, 340, 850, 404),
                           Tr(running ? Ui::Text::NextendoOn : Ui::Text::NextendoOff), running,
                           pane && !nav);
            DrawText(running ? ftp.Address() : std::wstring{}, Font::MetaMono, 580, 440, 1790);
            DrawText(std::wstring(Tr(Ui::Text::FtpUser)) + L" " + Tr(Ui::Text::FtpUsername) +
                         L" · " + Tr(Ui::Text::FtpPassword) + L" " + ftp.Password(),
                     Font::MetaMono, 580, 510, 1790);
            renderer.DrawString(Tr(Ui::Text::FtpHint), Font::Body, RectF(580, 600, 1790, 700),
                                Theme::kTextSecondary);
            if (ftp.ClientConnected())
                DrawText(std::wstring(Tr(Ui::Text::FtpConnected)) + L" " + ftp.LastPeer(),
                         Font::RowSub, 580, 720, 1790, Theme::kTextSecondary);
        } else if (!roots.empty()) {
            const auto breadcrumb =
                (root == 0 ? std::wstring(Tr(Ui::Text::FilesConsole)) : roots[root].label) +
                L" / " + relative;
            DrawText(breadcrumb, Font::RowSub, 580, 270, 1520, Theme::kTextSecondary);
            constexpr Ui::Text sorts[] = {Ui::Text::FilesSortName, Ui::Text::FilesSortSize,
                                          Ui::Text::FilesSortDate};
            DrawText(Tr(sorts[static_cast<unsigned>(sort)]), Font::RowSub, 1550, 270, 1810,
                     Theme::kTextSecondary);
            if (entries.empty()) {
                DrawText(Tr(Busy() ? Ui::Text::FilesWorking : Ui::Text::FilesEmpty), Font::RowTitle,
                         610, 460, 1770, Theme::kTextSecondary);
            }
            const int start = row / 8 * 8;
            if (!nav && pane && !entries.empty()) {
                const float y = focus_y.Value(now);
                renderer.FillRounded(RectF(560, y, 1824, y + 64), 32, Theme::kSurfaceStrong);
                DrawRing(renderer, RectF(560, y, 1824, y + 64), 32 + kRingGap, 1);
            }
            for (int i = start; i < std::min(static_cast<int>(entries.size()), start + 8); ++i) {
                const auto& entry = entries[i];
                const float y = 340.0f + (i - start) * 72.0f;
                // Simple outline glyphs share the text token; rows remain plain text lines.
                renderer.StrokeRounded(RectF(582, y + 19, 610, y + 43), 3, 2,
                                       Theme::kTextSecondary);
                if (entry.directory)
                    renderer.FillRounded(RectF(582, y + 13, 596, y + 21), 2, Theme::kTextSecondary);
                DrawText(entry.name, Font::RowTitle, 632, y + 8, 1290);
                if (!entry.directory) {
                    unsigned unit;
                    const auto number = Files::SizeNumber(
                        entry.size, unit, CurrentLanguage() == Language::Portuguese);
                    constexpr Ui::Text units[] = {Ui::Text::FilesBytes, Ui::Text::FilesKiB,
                                                  Ui::Text::FilesMiB, Ui::Text::FilesGiB,
                                                  Ui::Text::FilesTiB};
                    DrawText(number + L" " + Tr(units[unit]), Font::MetaMono, 1300, y + 10, 1510,
                             Theme::kTextSecondary);
                }
                FILETIME time{static_cast<DWORD>(entry.modified),
                              static_cast<DWORD>(entry.modified >> 32)};
                FILETIME local_time{};
                SYSTEMTIME date{};
                if (FileTimeToLocalFileTime(&time, &local_time) &&
                    FileTimeToSystemTime(&local_time, &date)) {
                    wchar_t formatted[80]{};
                    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &date, nullptr,
                                    formatted, 80, nullptr);
                    DrawText(formatted, Font::MetaMono, 1530, y + 10, 1810, Theme::kTextSecondary);
                }
            }
        }
        if (!FtpSelected() || Busy() || message != Ui::Text::FilesReady)
            DrawText(Tr(destination ? Ui::Text::FilesDestination : message), Font::RowSub, 580, 936,
                     1810, Theme::kTextSecondary);
        if (Busy() && progress.total) {
            DrawProgressBar(renderer, RectF(580, 902, 1810, 912),
                            static_cast<double>(progress.done.load()) / progress.total.load());
        }
        if (menu || deleting || overwrite) {
            DrawSheet(renderer, RectF(540, 240, 1490, 880));
            if (menu) {
                constexpr Ui::Text actions[] = {Ui::Text::FilesCopy, Ui::Text::FilesMove,
                                                Ui::Text::FilesRename, Ui::Text::FilesDelete,
                                                Ui::Text::FilesNewFolder};
                for (int i = 0; i < 5; ++i) {
                    const float y = 310.0f + i * 100.0f;
                    DrawChoicePill(renderer, RectF(610, y, 1420, y + 64), Tr(actions[i]), false,
                                   action == i);
                }
            } else {
                DrawText(
                    Tr(deleting ? Ui::Text::FilesDeleteConfirm : Ui::Text::FilesOverwriteConfirm),
                    Font::RowTitle, 610, 300, 1420);
                const auto from_label = source.root.label.empty()
                                            ? std::wstring(Tr(Ui::Text::FilesConsole))
                                            : source.root.label;
                const auto to_label =
                    root == 0 ? std::wstring(Tr(Ui::Text::FilesConsole)) : roots[root].label;
                DrawText(deleting ? to_label + L" / " + Selected().relative
                                  : from_label + L" / " + source.relative,
                         Font::RowSub, 610, 390, 1420, Theme::kTextSecondary);
                if (overwrite)
                    DrawText(to_label + L" / " + relative, Font::RowSub, 610, 440, 1420,
                             Theme::kTextSecondary);
                renderer.DrawString(Tr(deleting ? Ui::Text::FilesDeleteWarning
                                       : (source_directory || target_directory)
                                           ? Ui::Text::FilesDirectoryClash
                                           : Ui::Text::FilesOverwriteHint),
                                    Font::Body, RectF(610, 510, 1420, 620), Theme::kTextSecondary);
                if (!overwrite || (!source_directory && !target_directory))
                    DrawChoicePill(
                        renderer, RectF(610, 650, 1420, 714),
                        Tr(deleting ? Ui::Text::FilesConfirmDelete : Ui::Text::FilesConfirmReplace),
                        true, true);
                if (overwrite)
                    DrawText(Tr(Ui::Text::FilesKeepBoth), Font::RowSub, 610, 750, 1420);
            }
        }
        if (keyboard)
            keyboard->Draw(renderer, now);
    }
};
FilesScreen::FilesScreen(Renderer& renderer, Input& input, const std::filesystem::path& local,
                         Ftp::Server& ftp)
    : impl_(std::make_unique<Impl>(renderer, input, local, ftp)) {
    impl_->RefreshRoots();
}
FilesScreen::~FilesScreen() {
    impl_->progress.cancel = true;
    if (impl_->work.valid())
        impl_->work.wait();
}
void FilesScreen::Tick(Clock::time_point now) {
    impl_->Tick(now);
}
bool FilesScreen::HandleInput(Clock::time_point now) {
    return impl_->Handle(now);
}
void FilesScreen::Suspend() {
    ++impl_->ftp_generation;
    impl_->progress.cancel = true;
}
void FilesScreen::Enter() {
    impl_->RefreshRoots();
}
void FilesScreen::Draw(Clock::time_point now, bool nav) {
    impl_->Draw(now, nav);
}
std::vector<Hint> FilesScreen::Hints() const {
    return impl_->Hints();
}
bool FilesScreen::Busy() const {
    return impl_->Busy();
}
} // namespace EdenXbox::Ui
