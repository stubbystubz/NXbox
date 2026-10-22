// SPDX-License-Identifier: GPL-3.0-or-later
#include <functional>
#include <span>
#include <stdexcept>
#include <windows.h>

#include <fileapifromapp.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_uwp.h"
#include "eden_uwp/file_manager.h"
#include "eden_uwp/ftp/ftp_protocol.h"
#include "eden_uwp/usb_library.h"

namespace EdenXbox::Files {
namespace {
void Check(bool success) {
    if (!success)
        throw std::runtime_error("file action unavailable");
}
void Cancelled(const Progress& progress) {
    if (progress.cancel)
        throw std::runtime_error("file action cancelled");
}
Location Child(const Location& parent, const std::wstring& name) {
    Check(ValidName(name));
    return {parent.root, Join(parent.relative, name)};
}
void Walk(const Location& location, const std::function<void(const Location&, bool)>& visit,
          Progress& progress) {
    Cancelled(progress);
    const auto path = Resolve(location);
    const bool directory = Common::FS::IsDir(path);
    visit(location, directory);
    if (directory)
        for (const auto& entry : List(location))
            Walk(Child(location, entry.name), visit, progress);
}
void RemoveTree(const Location& location, Progress& progress) {
    Cancelled(progress);
    const auto path = Resolve(location);
    if (Common::FS::IsDir(path)) {
        for (const auto& item : List(location))
            RemoveTree(Child(location, item.name), progress);
        Check(RemoveDirectoryFromAppW(Resolve(location).c_str()) != 0);
    } else {
        Check(DeleteFileFromAppW(Resolve(location).c_str()) != 0);
    }
}
void CopyTree(const Location& source, const Location& target, Progress& progress) {
    Cancelled(progress);
    const auto from = Resolve(source);
    if (Common::FS::IsDir(from)) {
        MakeDirectory(target);
        for (const auto& entry : List(source))
            CopyTree(Child(source, entry.name), Child(target, entry.name), progress);
        return;
    }
    // IOFile uses CreateFileFromAppW, the same brokered handle used by FTP for USB drives.
    Common::FS::IOFile input(from, Common::FS::FileAccessMode::Read);
    Common::FS::IOFile output(Resolve(target), Common::FS::FileAccessMode::Write);
    Check(input.IsOpen() && output.IsOpen());
    std::vector<char> buffer(1024 * 1024);
    auto remaining = input.GetSize();
    while (remaining) {
        Cancelled(progress);
        // Revalidate before each write: never follow a newly introduced junction.
        Resolve(source);
        Resolve(target);
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        auto chunk = std::span<char>(buffer.data(), count);
        Check(input.ReadSpan<char>(chunk) == count && output.WriteSpan<char>(chunk) == count);
        progress.done += count;
        remaining -= count;
    }
    Check(output.Flush() && output.Commit());
}
} // namespace
std::vector<Root> Roots(const std::filesystem::path& local_state) {
    std::vector<Root> roots{{local_state, {}}};
    for (const auto& games : ExternalGameFolders()) {
        const auto path = games.parent_path();
        auto label = std::wstring(winrt::to_hstring(Ftp::ExternalAlias(roots.size() - 1)));
        try {
            const auto folder = winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(
                                    path.parent_path().wstring())
                                    .get();
            const auto display = folder.DisplayName();
            if (!display.empty())
                label += L" · " + std::wstring(display);
        } catch (...) {
        }
        roots.push_back({path, label});
    }
    return roots;
}
std::filesystem::path Resolve(const Location& location) {
    const auto normalized = Normalize(location.relative);
    Check(normalized.has_value() && !location.root.path.empty());
    auto result = location.root.path;
    auto guard = [](const auto& path) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &attributes))
            Check((attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
        else
            Check(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);
    };
    guard(result);
    for (const auto& part : std::filesystem::path(*normalized)) {
        result /= part;
        guard(result);
    }
    return result;
}
std::vector<Entry> List(const Location& location) {
    std::vector<Entry> entries;
    Check(Common::FS::Uwp::ForEachEntry(Resolve(location), [&](const auto&, const auto& data) {
        // Refuse the entire operation rather than silently skipping a junction during a move.
        Check((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
        Check(ValidName(data.cFileName));
        entries.push_back({data.cFileName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                           (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                           (std::uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) |
                               data.ftLastWriteTime.dwLowDateTime});
        return true;
    }));
    return entries;
}
bool Exists(const Location& location) {
    return Common::FS::Exists(Resolve(location));
}
bool Directory(const Location& location) {
    return Common::FS::IsDir(Resolve(location));
}
void MakeDirectory(const Location& location) {
    Check(!Normalize(location.relative).value_or(L"").empty());
    Check(CreateDirectoryFromAppW(Resolve(location).c_str(), nullptr) != 0);
}
void Rename(const Location& source, const Location& destination, Progress& progress) {
    Check(!Normalize(source.relative).value_or(L"").empty() &&
          !Normalize(destination.relative).value_or(L"").empty());
    const auto from = Resolve(source), to = Resolve(destination);
    Walk(source, [](const auto&, bool) {}, progress);
    Cancelled(progress);
    Check(!Common::FS::Exists(to));
    if (MoveFileFromAppW(from.c_str(), to.c_str()))
        return;
    using namespace winrt::Windows::Storage;
    // Removable storage may require the StorageFolder broker for publication.
    Check(source.root.path == destination.root.path && from.parent_path() == to.parent_path());
    const auto parent = StorageFolder::GetFolderFromPathAsync(from.parent_path().wstring()).get();
    const auto item = parent.GetItemAsync(from.filename().wstring()).get();
    Cancelled(progress);
    item.RenameAsync(to.filename().wstring(), NameCollisionOption::FailIfExists).get();
}
void Remove(const Location& location, Progress& progress) {
    Check(!Normalize(location.relative).value_or(L"").empty());
    // Preflight the whole tree before any destructive action.
    Walk(location, [](const auto&, bool) {}, progress);
    RemoveTree(location, progress);
}
void Transfer(const Location& source, const Location& destination, bool move, bool replace,
              Progress& progress) {
    Check(!Normalize(source.relative).value_or(L"").empty() &&
          !Normalize(destination.relative).value_or(L"").empty());
    const auto from = Resolve(source), to = Resolve(destination);
    const auto source_relative = Normalize(source.relative);
    const auto destination_relative = Normalize(destination.relative);
    Check(source.root.path != destination.root.path ||
          !Within(*source_relative, *destination_relative));
    Walk(
        source,
        [&](const auto& item, bool directory) {
            if (!directory)
                progress.total += Common::FS::GetSize(Resolve(item));
        },
        progress);
    const bool existed = Exists(destination);
    Check(!existed || (replace && Common::FS::IsFile(to) && Common::FS::IsFile(from)));
    Location staging = destination;
    // A private sibling is published only after all bytes are flushed. Never reuse a partial file.
    static std::atomic<unsigned long long> sequence{0};
    staging.relative +=
        L"." + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".partial";
    Check(!Exists(staging));
    try {
        CopyTree(source, staging, progress);
        Cancelled(progress);
        Resolve(destination);
        if (existed) {
            if (!ReplaceFileFromAppW(to.c_str(), Resolve(staging).c_str(), nullptr, 0, nullptr,
                                     nullptr)) {
                using winrt::Windows::Storage::StorageFile;
                const auto partial =
                    StorageFile::GetFileFromPathAsync(Resolve(staging).wstring()).get();
                const auto existing =
                    StorageFile::GetFileFromPathAsync(Resolve(destination).wstring()).get();
                partial.MoveAndReplaceAsync(existing).get();
            }
        } else {
            Rename(staging, destination, progress);
        }
    } catch (...) {
        try {
            Progress cleanup;
            if (Exists(staging))
                RemoveTree(staging, cleanup);
        } catch (...) {
        }
        throw;
    }
    // Once published, a move removes its source. Cancellation here leaves both safe copies.
    if (move)
        Remove(source, progress);
}
} // namespace EdenXbox::Files
