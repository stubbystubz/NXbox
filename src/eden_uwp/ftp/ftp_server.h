// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace EdenXbox::Ftp {
// Lifetime owns all sockets. Stop also cancels pending data I/O and removes partial uploads.
class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    bool Start(const std::filesystem::path& local_state);
    void Stop();
    bool Running() const;
    std::wstring Address() const;
    std::wstring Password() const;
    std::uint64_t Changes() const;
    // Local file operations use the same library-rescan signal as FTP uploads.
    void NotifyChanges();
    bool ClientConnected() const;
    std::wstring LastPeer() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace EdenXbox::Ftp
