// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <memory>
#include "eden_uwp/ftp/ftp_server.h"
#include "eden_uwp/ui/anim.h"
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/renderer.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
class FilesScreen {
public:
    FilesScreen(Renderer& renderer, Input& input, const std::filesystem::path& local_state,
                Ftp::Server& ftp);
    ~FilesScreen();
    void Tick(Clock::time_point now);
    // Returns true when focus should return to the top navigation.
    bool HandleInput(Clock::time_point now);
    void Enter();
    void Suspend();
    void Draw(Clock::time_point now, bool navigation_focused);
    std::vector<Hint> Hints() const;
    bool Busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace EdenXbox::Ui
