// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ftp/ftp_protocol.h"
#include "eden_uwp/ftp/ftp_server.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_uwp.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/usb_library.h"

namespace EdenXbox::Ftp {
namespace {
using namespace winrt::Windows::Networking;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
constexpr std::uint32_t kBufferSize = 1024 * 1024;

std::tm Modified(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    SYSTEMTIME time{};
    if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        !FileTimeToSystemTime(&attributes.ftLastWriteTime, &time))
        throw std::runtime_error("cannot read modification time");
    std::tm utc{};
    utc.tm_year = time.wYear - 1900;
    utc.tm_mon = time.wMonth - 1;
    utc.tm_mday = time.wDay;
    utc.tm_hour = time.wHour;
    utc.tm_min = time.wMinute;
    utc.tm_sec = time.wSecond;
    return utc;
}
std::string Name(const fs::path& path) {
    return winrt::to_string(path.filename().wstring());
}
void Write(const StreamSocket& socket, std::string_view text) {
    DataWriter writer(socket.OutputStream());
    writer.WriteBytes(winrt::array_view<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        reinterpret_cast<const std::uint8_t*>(text.data() + text.size())));
    writer.StoreAsync().get();
    writer.DetachStream();
}
bool Publish(const fs::path& source, const fs::path& destination) {
    return Common::FS::Exists(destination)
               ? ReplaceFileFromAppW(destination.c_str(), source.c_str(), nullptr, 0, nullptr,
                                     nullptr) != 0
               : MoveFileFromAppW(source.c_str(), destination.c_str()) != 0;
}
// All threads keep their owning Session alive. Closing the sockets cancels blocked WinRT I/O.
struct Session : std::enable_shared_from_this<Session> {
    StreamSocket control{nullptr};
    StreamSocket data{nullptr};
    StreamSocketListener passive{nullptr};
    fs::path root;
    std::vector<fs::path> externals;
    std::string password;
    std::string ip;
    std::string peer;
    std::string cwd;
    std::optional<std::string> rename_from;
    std::shared_ptr<std::atomic<std::uint64_t>> changes;
    std::atomic<bool> stopped{false};
    std::atomic<bool> transferring{false};
    std::atomic<bool> done{false};
    std::mutex sockets_mutex;
    std::mutex reply_mutex;
    std::mutex finish_mutex;
    std::condition_variable ready;
    std::condition_variable finished;
    std::thread transfer;
    bool authenticated = false;
    bool username_ok = false;
    int failures = 0;

    void Reply(std::string_view text) {
        std::lock_guard lock(reply_mutex);
        Write(control, text);
    }
    void CloseData() {
        std::lock_guard lock(sockets_mutex);
        if (passive) {
            passive.Close();
            passive = nullptr;
        }
        if (data) {
            data.Close();
            data = nullptr;
        }
        ready.notify_all();
    }
    void Stop() {
        stopped = true;
        control.Close();
        CloseData();
    }
    void Wait() {
        std::unique_lock lock(finish_mutex);
        finished.wait(lock, [&] { return done.load(); });
    }
    fs::path Resolve(const std::string& relative) const {
        if (relative.empty())
            return root; // Only CWD and listings may use the virtual root.
        const auto slash = relative.find('/');
        const auto alias = relative.substr(0, slash);
        fs::path result;
        if (alias == "interno")
            result = root;
        else if (const auto index = ExternalAliasIndex(alias); index && *index < externals.size())
            result = externals[*index];
        else
            throw std::runtime_error("root unavailable");
        const auto tail = slash == std::string::npos ? std::string{} : relative.substr(slash + 1);
        for (const auto& component : fs::path(winrt::to_hstring(tail).c_str())) {
            result /= component;
            // Reject every reparse point, including directory junctions, not just symlinks.
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (GetFileAttributesExFromAppW(result.c_str(), GetFileExInfoStandard, &attributes) &&
                (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                throw std::runtime_error("reparse point");
        }
        return result;
    }
    void Changed(const std::string& path) {
        if (InGames(path))
            ++*changes;
    }
    void Passive(bool extended) {
        CloseData();
        for (unsigned port = 50000; port <= 50009; ++port) {
            StreamSocketListener candidate;
            const std::weak_ptr<Session> weak = shared_from_this();
            candidate.ConnectionReceived([weak](const auto& sender, const auto& args) {
                const auto owner = weak.lock();
                if (!owner) {
                    args.Socket().Close();
                    return;
                }
                std::lock_guard lock(owner->sockets_mutex);
                const auto incoming = args.Socket();
                if (owner->stopped || owner->passive != sender || owner->data ||
                    winrt::to_string(incoming.Information().RemoteAddress().CanonicalName()) !=
                        owner->peer) {
                    incoming.Close();
                    return;
                }
                owner->data = incoming;
                owner->ready.notify_all();
            });
            try {
                // Any interface, like the control port; PASV still advertises `ip`.
                candidate.BindServiceNameAsync(winrt::to_hstring(port)).get();
            } catch (...) {
                candidate.Close();
                continue;
            }
            {
                std::lock_guard lock(sockets_mutex);
                if (stopped) {
                    candidate.Close();
                    return;
                }
                passive = candidate;
            }
            Reply(extended ? EpsvReply(port) : PasvReply(*PrivateIPv4(ip), port));
            return;
        }
        Reply("425 No passive port available.\r\n");
    }
    StreamSocket AwaitData() {
        std::unique_lock lock(sockets_mutex);
        if (!ready.wait_for(lock, 30s, [&] { return data || stopped || !passive; }) || stopped ||
            !data)
            throw std::runtime_error("data connection unavailable");
        return data;
    }
    void Transfer(std::string verb, std::string relative) {
        const bool upload = verb == "STOR" || verb == "APPE";
        const bool listing = verb == "LIST" || verb == "NLST" || verb == "MLSD";
        std::uint64_t bytes = 0;
        bool success = false;
        bool connected = false;
        fs::path partial;
        try {
            const auto target = Resolve(relative);
            if (upload) {
                partial = target;
                partial += L".part";
                // Do not destroy a pre-existing temporary file from another producer.
                if (Common::FS::Exists(partial)) {
                    partial.clear();
                    throw std::runtime_error("partial exists");
                }
                if (verb == "APPE" && Common::FS::Exists(target)) {
                    Common::FS::IOFile original(target, Common::FS::FileAccessMode::Read);
                    Common::FS::IOFile copy(partial, Common::FS::FileAccessMode::Write);
                    if (!original.IsOpen() || !copy.IsOpen())
                        throw std::runtime_error("cannot prepare append");
                    std::vector<char> chunk(kBufferSize);
                    std::uint64_t remaining = original.GetSize();
                    while (remaining != 0 && !stopped && transferring) {
                        const auto count = static_cast<std::size_t>(
                            std::min<std::uint64_t>(chunk.size(), remaining));
                        const auto bytes_to_copy = std::span<char>(chunk.data(), count);
                        if (original.ReadSpan<char>(bytes_to_copy) != count ||
                            copy.WriteSpan<char>(bytes_to_copy) != count)
                            throw std::runtime_error("append copy failed");
                        remaining -= count;
                    }
                    if (remaining != 0 || !copy.Flush() || !copy.Commit() || stopped ||
                        !transferring)
                        throw std::runtime_error("append cancelled");
                    copy.Close();
                }
            }
            const auto socket = AwaitData();
            connected = true;
            if (upload) {
                Common::FS::IOFile output(partial, Common::FS::FileAccessMode::Append);
                if (!output.IsOpen())
                    throw std::runtime_error("cannot open upload");
                DataReader reader(socket.InputStream());
                reader.InputStreamOptions(InputStreamOptions::Partial);
                std::vector<std::uint8_t> buffer(kBufferSize);
                while (!stopped && transferring) {
                    const auto count = reader.LoadAsync(kBufferSize).get();
                    if (count == 0)
                        break;
                    reader.ReadBytes(
                        winrt::array_view<std::uint8_t>(buffer.data(), buffer.data() + count));
                    if (output.WriteSpan<std::uint8_t>(
                            std::span<const std::uint8_t>(buffer.data(), count)) != count)
                        throw std::runtime_error("upload write failed");
                    bytes += count;
                }
                reader.DetachStream();
                if (!output.Flush() || !output.Commit() || stopped || !transferring)
                    throw std::runtime_error("upload cancelled");
                output.Close();
                if (!Publish(partial, target))
                    throw std::runtime_error("upload rename failed");
                partial.clear();
                Changed(relative);
            } else if (listing) {
                auto entry = [&](const fs::path& item) {
                    const auto filename = Name(item);
                    if (!NormalizePath("", filename))
                        return;
                    const auto checked =
                        Resolve(relative.empty() ? filename : relative + "/" + filename);
                    const bool directory = Common::FS::IsDir(checked);
                    const auto size = directory ? 0 : Common::FS::GetSize(checked);
                    const auto utc = Modified(checked);
                    std::string line;
                    if (verb == "NLST")
                        line = filename + "\r\n";
                    else if (verb == "MLSD")
                        line = std::string("type=") + (directory ? "dir" : "file") +
                               ";size=" + std::to_string(size) + ";modify=" + Timestamp(utc) +
                               "; " + filename + "\r\n";
                    else
                        line = ListLine(filename, directory, size, utc);
                    Write(socket, line);
                    bytes += line.size();
                };
                if (relative.empty()) {
                    std::vector<std::string> aliases{"interno"};
                    for (std::size_t index = 0; index < externals.size(); ++index)
                        aliases.push_back(ExternalAlias(index));
                    for (const auto& alias_name : aliases) {
                        const char* alias = alias_name.c_str();
                        std::string line;
                        if (verb == "NLST")
                            line = std::string(alias) + "\r\n";
                        else if (verb == "MLSD")
                            line = std::string("type=dir; ") + alias + "\r\n";
                        else {
                            std::tm date{};
                            date.tm_year = 126;
                            date.tm_mday = 1;
                            line = ListLine(alias, true, 0, date);
                        }
                        Write(socket, line);
                        bytes += line.size();
                    }
                } else {
                    if (!Common::FS::Uwp::ForEachEntry(
                            target, [&](const fs::path& item, const auto&) {
                                if (stopped || !transferring)
                                    throw std::runtime_error("listing cancelled");
                                entry(item);
                                return true;
                            }))
                        throw std::runtime_error("listing failed");
                }
            } else {
                Common::FS::IOFile input(target, Common::FS::FileAccessMode::Read);
                if (!input.IsOpen())
                    throw std::runtime_error("cannot open download");
                std::vector<char> buffer(kBufferSize);
                std::uint64_t remaining = input.GetSize();
                while (remaining != 0 && !stopped && transferring) {
                    const auto count =
                        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), remaining));
                    if (input.ReadSpan<char>(std::span<char>(buffer.data(), count)) != count)
                        throw std::runtime_error("download read failed");
                    Write(socket, std::string_view(buffer.data(), count));
                    bytes += count;
                    remaining -= count;
                }
                if (remaining != 0 || stopped || !transferring)
                    throw std::runtime_error("download cancelled");
            }
            success = true;
        } catch (...) {
            // A failed transfer must leave the old destination intact.
        }
        if (!partial.empty()) {
            DeleteFileFromAppW(partial.c_str());
        }
        CloseData();
        const bool aborted = !transferring;
        transferring = false;
        if (!stopped && !aborted) {
            try {
                Reply(success     ? "226 Transfer complete.\r\n"
                      : connected ? "426 Transfer failed.\r\n"
                                  : "425 Cannot open data connection.\r\n");
            } catch (...) {
                Stop();
            }
        }
        std::string action = verb;
        std::transform(action.begin(), action.end(), action.begin(),
                       [](char letter) { return static_cast<char>(letter + ('a' - 'A')); });
        Diagnostic("FTP " + action + " path=" + relative + " bytes=" + std::to_string(bytes) +
                   (success ? " ok" : " fail"));
    }
    void Abort() {
        const bool active = transferring.exchange(false);
        CloseData();
        if (transfer.joinable())
            transfer.join();
        Reply(active ? "426 Transfer aborted.\r\n226 Abort successful.\r\n"
                     : "225 No transfer active.\r\n");
    }
    bool Handle(const Command& command) {
        const auto& verb = command.verb;
        const auto& argument = command.argument;
        if (verb == "QUIT") {
            Reply("221 Goodbye.\r\n");
            return false;
        }
        if (verb == "USER" && !transferring) {
            authenticated = false;
            username_ok = argument == "nxbox";
            rename_from.reset();
            CloseData();
            Reply("331 Password required.\r\n");
        } else if (verb == "PASS" && !transferring) {
            if (username_ok && argument == password) {
                authenticated = true;
                Reply("230 Logged in.\r\n");
                Diagnostic("FTP login ok ip=" + peer);
            } else {
                authenticated = false;
                ++failures;
                std::unique_lock lock(sockets_mutex);
                ready.wait_for(lock, 2s, [&] { return stopped.load(); });
                lock.unlock();
                Reply("530 Login incorrect.\r\n");
                if (failures >= 5)
                    return false;
            }
        } else if (verb == "SYST") {
            Reply("215 UNIX Type: L8\r\n");
        } else if (verb == "FEAT") {
            Reply("211-Features\r\n UTF8\r\n EPSV\r\n PASV\r\n SIZE\r\n MDTM\r\n"
                  " MLST type*;size*;modify*;\r\n211 End\r\n");
        } else if (verb == "OPTS") {
            Reply(argument == "UTF8 ON" ? "200 UTF8 enabled.\r\n" : "501 Unsupported option.\r\n");
        } else if (!authenticated) {
            Reply("530 Log in first.\r\n");
        } else if (verb == "ABOR") {
            Abort();
        } else if (verb == "NOOP") {
            Reply("200 OK.\r\n");
        } else if (transferring) {
            Reply("450 Transfer in progress.\r\n");
        } else if (verb == "PASV" || verb == "EPSV") {
            if (verb == "EPSV" && !argument.empty() && argument != "1")
                Reply("522 Use IPv4.\r\n");
            else
                Passive(verb == "EPSV");
        } else if (verb == "PWD") {
            std::string quoted;
            for (const char letter : cwd) {
                quoted += letter;
                if (letter == '"')
                    quoted += '"';
            }
            Reply("257 \"/" + quoted + "\"\r\n");
        } else if (verb == "TYPE") {
            Reply(argument == "A" || argument == "I" ? "200 Binary transfers.\r\n"
                                                     : "504 Unsupported type.\r\n");
        } else if (verb == "REST") {
            Reply(argument == "0" ? "350 Restart at zero.\r\n" : "504 Only REST 0 supported.\r\n");
        } else if (verb == "CWD" || verb == "CDUP" || verb == "SIZE" || verb == "MDTM" ||
                   verb == "STOR" || verb == "APPE" || verb == "RETR" || verb == "LIST" ||
                   verb == "NLST" || verb == "MLSD" || verb == "DELE" || verb == "MKD" ||
                   verb == "RMD" || verb == "RNFR" || verb == "RNTO") {
            const auto source = rename_from;
            rename_from.reset();
            const auto relative = NormalizePath(cwd, verb == "CDUP" ? ".." : argument);
            if (!relative) {
                Reply("550 Invalid path.\r\n");
                return true;
            }
            try {
                const bool virtual_directory = relative->find('/') == std::string::npos;
                if (virtual_directory && verb != "CWD" && verb != "CDUP" && verb != "LIST" &&
                    verb != "NLST" && verb != "MLSD")
                    throw std::runtime_error("virtual root is read-only");
                const auto target = Resolve(*relative);
                if (verb == "CWD" || verb == "CDUP") {
                    if (!Common::FS::IsDir(target))
                        throw std::runtime_error("not a directory");
                    cwd = *relative;
                    Reply("250 Directory changed.\r\n");
                } else if (verb == "SIZE") {
                    if (!Common::FS::IsFile(target))
                        throw std::runtime_error("not a file");
                    Reply("213 " + std::to_string(Common::FS::GetSize(target)) + "\r\n");
                } else if (verb == "MDTM") {
                    Reply("213 " + Timestamp(Modified(target)) + "\r\n");
                } else if (verb == "RNFR") {
                    if (relative->empty() || !Common::FS::Exists(target))
                        throw std::runtime_error("missing source");
                    rename_from = *relative;
                    Reply("350 Send RNTO.\r\n");
                } else if (verb == "RNTO") {
                    if (!source) {
                        Reply("503 Send RNFR first.\r\n");
                    } else {
                        if (relative->empty() || !Publish(Resolve(*source), target))
                            throw std::runtime_error("rename failed");
                        Changed(*source);
                        Changed(*relative);
                        Reply("250 Renamed.\r\n");
                    }
                } else if (verb == "MKD") {
                    if (!CreateDirectoryFromAppW(target.c_str(), nullptr))
                        throw std::runtime_error("cannot create directory");
                    Reply("257 Directory created.\r\n");
                } else if (verb == "DELE" || verb == "RMD") {
                    if (relative->empty() || (verb == "RMD") != Common::FS::IsDir(target) ||
                        !(verb == "RMD" ? RemoveDirectoryFromAppW(target.c_str())
                                        : DeleteFileFromAppW(target.c_str())))
                        throw std::runtime_error("cannot remove");
                    Changed(*relative);
                    Reply("250 Removed.\r\n");
                } else {
                    const bool listing = verb == "LIST" || verb == "NLST" || verb == "MLSD";
                    if (listing ? !Common::FS::IsDir(target) : relative->empty())
                        throw std::runtime_error("invalid target");
                    if (verb == "RETR" && !Common::FS::IsFile(target))
                        throw std::runtime_error("not a file");
                    if ((verb == "STOR" || verb == "APPE") &&
                        (!Common::FS::IsDir(target.parent_path()) ||
                         (Common::FS::Exists(target) && !Common::FS::IsFile(target))))
                        throw std::runtime_error("invalid upload target");
                    bool has_passive;
                    {
                        std::lock_guard lock(sockets_mutex);
                        has_passive = static_cast<bool>(passive);
                    }
                    if (!has_passive) {
                        Reply("425 Use PASV or EPSV first.\r\n");
                    } else {
                        if (transfer.joinable())
                            transfer.join();
                        transferring = true;
                        Reply("150 Opening data connection.\r\n");
                        transfer = std::thread([self = shared_from_this(), verb, path = *relative] {
                            winrt::init_apartment(winrt::apartment_type::multi_threaded);
                            self->Transfer(verb, path);
                            winrt::uninit_apartment();
                        });
                    }
                }
            } catch (...) {
                Reply("550 File action unavailable.\r\n");
            }
        } else {
            rename_from.reset();
            Reply("502 Command not implemented.\r\n");
        }
        return !stopped;
    }
    void Run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            Reply("220 NXbox FTP ready.\r\n");
            DataReader reader(control.InputStream());
            reader.InputStreamOptions(InputStreamOptions::Partial);
            std::string pending;
            bool keep = true;
            while (keep && !stopped) {
                if (reader.LoadAsync(1024).get() == 0)
                    break;
                while (reader.UnconsumedBufferLength() != 0) {
                    const auto byte = reader.ReadByte();
                    if (byte == 0 || pending.size() >= 8192)
                        throw std::runtime_error("invalid command");
                    pending += static_cast<char>(byte);
                    if (pending.ends_with("\r\n")) {
                        keep = Handle(ParseCommand(pending));
                        pending.clear();
                        if (!keep)
                            break;
                    }
                }
            }
            reader.DetachStream();
        } catch (...) {
        }
        Stop();
        if (transfer.joinable())
            transfer.join();
        Diagnostic("FTP session closed ip=" + peer);
        {
            std::lock_guard lock(finish_mutex);
            done = true;
        }
        finished.notify_all();
        winrt::uninit_apartment();
    }
};
} // namespace

struct Server::State {
    mutable std::mutex mutex;
    StreamSocketListener listener{nullptr};
    std::shared_ptr<Session> session;
    std::shared_ptr<std::atomic<std::uint64_t>> changes =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    std::string ip;
    std::string password;
    bool running = false;
};
Server::Server() : state_(std::make_shared<State>()) {}
Server::~Server() {
    Stop();
}
bool Server::Start(const fs::path& local_state) {
    Stop();
    std::lock_guard lock(state_->mutex);
    try {
        state_->ip.clear();
        // The console also has internal virtual adapters with private addresses; prefer the
        // one carrying the internet connection, else fall back to the first private IPv4.
        winrt::guid lan_adapter{};
        if (const auto profile = Connectivity::NetworkInformation::GetInternetConnectionProfile())
            if (const auto adapter = profile.NetworkAdapter())
                lan_adapter = adapter.NetworkAdapterId();
        std::string fallback;
        for (const auto& host : Connectivity::NetworkInformation::GetHostNames()) {
            const auto address = winrt::to_string(host.CanonicalName());
            if (host.Type() != HostNameType::Ipv4 || !PrivateIPv4(address))
                continue;
            Diagnostic("FTP candidate " + address);
            const auto info = host.IPInformation();
            if (info && info.NetworkAdapter() &&
                info.NetworkAdapter().NetworkAdapterId() == lan_adapter) {
                state_->ip = address;
                break;
            }
            if (fallback.empty())
                fallback = address;
        }
        if (state_->ip.empty())
            state_->ip = fallback;
        Diagnostic("FTP address " + state_->ip);
        if (state_->ip.empty())
            return false;
        using winrt::Windows::Security::Cryptography::CryptographicBuffer;
        std::uint32_t random;
        // Rejection sampling keeps all six-digit passwords equally likely.
        do {
            random = CryptographicBuffer::GenerateRandomNumber();
        } while (random >= 4294800000u);
        state_->password = std::to_string(100000u + random % 900000u);
        // Headless start: a password of at least 8 characters in ftp-autostart replaces the
        // on-screen one, so a script can log in without reading the TV.
        if (std::ifstream autostart{local_state / "ftp-autostart"}) {
            std::string fixed;
            std::getline(autostart, fixed);
            while (!fixed.empty() && (fixed.back() == '\r' || fixed.back() == ' '))
                fixed.pop_back();
            if (fixed.size() >= 8)
                state_->password = fixed;
        }
        fs::create_directories(local_state / "games");
        fs::create_directories(local_state / "eden" / "keys");
        fs::create_directories(local_state / "firmware");
        const auto game_folders = ExternalGameFolders();
        std::vector<fs::path> externals;
        for (const auto& folder : game_folders)
            externals.push_back(folder.parent_path());
        StreamSocketListener listener;
        const std::weak_ptr<State> weak = state_;
        listener.ConnectionReceived([weak, local_state, externals](const auto&, const auto& args) {
            const auto owner = weak.lock();
            const auto socket = args.Socket();
            Diagnostic("FTP connection from " +
                       winrt::to_string(socket.Information().RemoteAddress().CanonicalName()));
            if (!owner) {
                socket.Close();
                return;
            }
            std::lock_guard session_lock(owner->mutex);
            if (!owner->running || (owner->session && !owner->session->done)) {
                std::thread([socket] {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    try {
                        Write(socket, "421 Server busy.\r\n");
                    } catch (...) {
                    }
                    socket.Close();
                    winrt::uninit_apartment();
                }).detach();
                return;
            }
            auto session = std::make_shared<Session>();
            session->control = socket;
            session->root = local_state;
            session->externals = externals;
            session->ip = owner->ip;
            session->password = owner->password;
            session->changes = owner->changes;
            session->peer = winrt::to_string(socket.Information().RemoteAddress().CanonicalName());
            owner->session = session;
            std::thread([session] { session->Run(); }).detach();
        });
        // Listen on every interface: bound to the LAN address alone the port stayed closed
        // to the LAN on the console; state_->ip is only what the UI and PASV show.
        listener.BindServiceNameAsync(L"2121").get();
        state_->listener = listener;
        state_->running = true;
        Diagnostic("FTP listening on port 2121");
        return true;
    } catch (...) {
        Diagnostic("FTP start failed");
        return false;
    }
}
void Server::Stop() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(state_->mutex);
        state_->running = false;
        if (state_->listener) {
            Diagnostic("FTP stopped");
            state_->listener.Close();
            state_->listener = nullptr;
        }
        session = std::move(state_->session);
        state_->password.clear();
    }
    if (session) {
        session->Stop();
        session->Wait();
    }
}
bool Server::Running() const {
    std::lock_guard lock(state_->mutex);
    return state_->running;
}
std::wstring Server::Address() const {
    std::lock_guard lock(state_->mutex);
    return L"ftp://" + std::wstring(winrt::to_hstring(state_->ip)) + L":2121";
}
std::wstring Server::Password() const {
    std::lock_guard lock(state_->mutex);
    return std::wstring(winrt::to_hstring(state_->password));
}
std::uint64_t Server::Changes() const {
    return state_->changes->load();
}
} // namespace EdenXbox::Ftp
