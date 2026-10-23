// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/fs/fs_uwp.h"
#include "eden_uwp/firmware.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "core/core.h"
#include "core/crypto/aes_util.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/file_manager.h"
#include "frontend_common/firmware_files.h"
#include "frontend_common/firmware_install.h"
#include "frontend_common/firmware_manager.h"
#include "frontend_common/firmware_sources.h"

namespace EdenXbox {
namespace {
std::mutex status_mutex;
FirmwareStatus status;

namespace Sources = FirmwareManager::Sources;

Sources::IO FirmwareIO() {
    Sources::IO io;
    io.list = [](const std::filesystem::path& path) {
        std::vector<Sources::Entry> entries;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &attributes)) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return entries;
            throw std::system_error(error, std::system_category(), "Firmware directory access");
        }
        if (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Firmware directory is a reparse point");
        DWORD enumeration_error{};
        if (!Common::FS::Uwp::ForEachEntry(
                path,
                [&](const auto& child, const WIN32_FIND_DATAW& data) {
                    entries.push_back(
                        {child, bool(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
                         bool(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
                         (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                         (std::uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) |
                             data.ftLastWriteTime.dwLowDateTime});
                    return true;
                },
                &enumeration_error)) {
            throw std::system_error(enumeration_error, std::system_category(),
                                    "Firmware enumeration");
        }
        return entries;
    };
    io.open = [](const std::filesystem::path& path) {
        const auto raw = CreateFileFromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (raw == INVALID_HANDLE_VALUE)
            throw std::system_error(GetLastError(), std::system_category(), "Firmware source open");
        const auto handle = std::shared_ptr<void>(raw, [](void* h) { CloseHandle(h); });
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(raw, &size))
            throw std::system_error(GetLastError(), std::system_category(), "Firmware source size");
        return Sources::Reader{
            static_cast<std::uint64_t>(size.QuadPart),
            [handle](std::uint64_t offset, Sources::Bytes bytes) {
                LARGE_INTEGER position{};
                position.QuadPart = static_cast<LONGLONG>(offset);
                if (!SetFilePointerEx(handle.get(), position, nullptr, FILE_BEGIN))
                    throw std::system_error(GetLastError(), std::system_category(),
                                            "Firmware source seek");
                DWORD count{};
                if (!ReadFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count,
                              nullptr))
                    throw std::system_error(GetLastError(), std::system_category(),
                                            "Firmware source read");
                if (count != bytes.size())
                    throw std::runtime_error("Firmware source short read");
            }};
    };
    struct FileWriter final : Sources::Writer {
        Common::FS::IOFile file;
        explicit FileWriter(const std::filesystem::path& path)
            : file(path, Common::FS::FileAccessMode::Write) {
            if (!file.IsOpen())
                throw std::runtime_error("Firmware temporary open failed");
        }
        void Write(Sources::View bytes) override {
            if (file.WriteSpan(bytes) != bytes.size())
                throw std::runtime_error("Firmware temporary write failed (drive full?)");
        }
        void Finish() override {
            if (!file.Flush())
                throw std::runtime_error("Firmware temporary flush failed");
            file.Close();
        }
    };
    io.create = [](const auto& path) { return std::make_unique<FileWriter>(path); };
    io.mkdir = [](const auto& path) {
        if (Common::FS::Exists(path) || !Common::FS::CreateDir(path))
            throw std::runtime_error("Firmware temporary directory creation failed");
    };
    io.remove_tree = [](const auto& path) {
        if (!Common::FS::RemoveDirRecursively(path))
            Diagnostic("FIRMWARE temporary_cleanup_failed");
    };
    io.log = [](const auto& text) { Diagnostic(text); };
    return io;
}

// The library's USB import copies the keys only after this runs, so the first launch with a fresh
// drive would try its firmware without keys and fail. Take missing keys from the drive first, from
// the same places the import looks (NXbox\, then switch\, then the drive root).
void CopyMissingKeys(const std::filesystem::path& local_state,
                     const std::vector<winrt::Windows::Storage::StorageFolder>& drives) {
    using namespace winrt::Windows::Storage;
    for (const wchar_t* name : {L"prod.keys", L"title.keys"}) {
        if (Common::FS::Exists(local_state / "eden" / "keys" / name)) {
            continue;
        }
        bool copied = false;
        for (const auto& drive : drives) {
            for (const wchar_t* prefix : {L"NXbox\\", L"switch\\", L""}) {
                if (copied) {
                    break;
                }
                try {
                    const auto item =
                        AwaitBounded(drive.TryGetItemAsync(std::wstring(prefix) + name),
                                     std::chrono::seconds(3));
                    const auto file = item && *item ? item->try_as<StorageFile>() : nullptr;
                    if (!file) {
                        continue;
                    }
                    const auto keys =
                        AwaitBounded(ApplicationData::Current().LocalFolder().CreateFolderAsync(
                                         L"eden\\keys", CreationCollisionOption::OpenIfExists),
                                     std::chrono::seconds(5));
                    copied = keys && AwaitBounded(file.CopyAsync(*keys, name,
                                                                 NameCollisionOption::FailIfExists),
                                                  std::chrono::seconds(10));
                    Diagnostic("FIRMWARE keys_from_usb " + winrt::to_string(name) +
                               (copied ? " copied" : " copy_failed"));
                } catch (const winrt::hresult_error& error) {
                    Diagnostic("FIRMWARE keys_from_usb_failed " +
                               winrt::to_string(error.message()));
                }
            }
        }
    }
}
} // namespace

FirmwareStatus GetFirmwareStatus() {
    const std::scoped_lock lock(status_mutex);
    return status;
}

static void PrepareFirmware(const std::filesystem::path& local_state,
                            const std::filesystem::path& selected,
                            const std::function<bool(size_t, size_t)>& progress) {
    FirmwareStatus next;
    try {
        std::vector<winrt::Windows::Storage::StorageFolder> drives;
        try {
            const auto found = AwaitBounded(
                winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync(),
                std::chrono::seconds(5));
            if (found) {
                drives.assign(begin(*found), end(*found));
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("FIRMWARE usb_scan_failed " + winrt::to_string(error.message()));
        }
        if (selected.empty())
            CopyMissingKeys(local_state, drives);
        // Filesystem-only setup avoids reserving guest memory just to inspect/install firmware.
        Core::Crypto::KeyManager::Instance().ReloadKeys();
        Core::System system;
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        auto& controller = system.GetFileSystemController();
        controller.CreateFactories(*system.GetFilesystem());
        next.version = FirmwareManager::InstalledVersion(system);
        std::vector<std::filesystem::path> roots;
        for (const auto& drive : drives)
            if (!drive.Path().empty())
                roots.emplace_back(std::wstring(drive.Path()));
        auto io = FirmwareIO();
        bool extraction_failed = false;
        io.log = [&](const std::string& text) {
            Diagnostic(text);
            if (text.find("candidate_extraction_failed") != std::string::npos) {
                extraction_failed = true;
                next.install_failed = true;
            }
            if (text.find("nand_access_denied") != std::string::npos ||
                text.find("chunk_access_denied") != std::string::npos ||
                text.find("nand_no_visible_chunks") != std::string::npos ||
                text.find("chunk_no_visible_parts") != std::string::npos)
                next.conversion_required = true;
        };
        Sources::SweepTemporary(io, local_state);
        next.searched_folders = FirmwareManager::FirmwareFolders(local_state, roots);
        for (const auto& folder : next.searched_folders)
            Diagnostic("FIRMWARE searched " + Common::FS::PathToUTF8String(folder));
        auto sources =
            selected.empty()
                ? Sources::Discover(io, next.searched_folders, roots)
                : Sources::Discover(
                      io, {Common::FS::IsDir(selected) ? selected : selected.parent_path()}, {});
        if (!selected.empty() && !Common::FS::IsDir(selected))
            std::erase_if(sources, [&](const auto& item) { return item.path != selected; });
        if (!selected.empty() && sources.empty())
            next.install_failed = true;
        const auto record_path = local_state / "firmware_source.txt";
        auto records = Common::FS::ReadStringFromFile(record_path, Common::FS::FileType::TextFile);
        std::vector<std::filesystem::path> temporary_paths;
        struct Cleanup {
            const Sources::IO& io;
            const std::vector<std::filesystem::path>& paths;
            void DiscardExcept(const std::filesystem::path& retained) const {
                for (const auto& path : paths) {
                    if (path == retained)
                        continue;
                    try {
                        if (Common::FS::Exists(path))
                            io.remove_tree(path);
                    } catch (...) {
                        Diagnostic("FIRMWARE temporary_cleanup_failed");
                    }
                }
            }
            ~Cleanup() {
                DiscardExcept({});
            }
        } cleanup{io, temporary_paths};
        std::vector<Sources::Source> verified_sources;
        std::vector<std::filesystem::path> best_files;
        std::filesystem::path best_temporary;
        std::string best_version = next.version;
        const Sources::Source* best_source = nullptr;
        for (const auto& source : sources) {
            extraction_failed = false;
            try {
                const auto fingerprint = "BEGIN\n" + Sources::Fingerprint(source) + "END\n";
                if (selected.empty() && !next.version.empty() && !source.parts.empty() &&
                    records.find(fingerprint) != std::string::npos) {
                    Diagnostic("FIRMWARE source_already_installed");
                    continue;
                }
                const auto temporary =
                    local_state / (".firmware_extract-" + std::to_string(GetCurrentProcessId()) +
                                   "-" + std::to_string(GetTickCount64()) + "-" +
                                   std::to_string(temporary_paths.size()));
                temporary_paths.push_back(temporary);
                Sources::Decrypt decrypt;
                const auto key = Core::Crypto::KeyManager::Instance().GetBISKey(2);
                if (std::any_of(key.begin(), key.end(), [](auto byte) { return byte != 0; })) {
                    auto cipher = std::make_shared<Core::Crypto::AESCipher<Core::Crypto::Key256>>(
                        key, Core::Crypto::Mode::XTS);
                    decrypt = [cipher](std::uint64_t sector, Sources::View input,
                                       Sources::Bytes output) {
                        cipher->XTSTranscode(input.data(), input.size(), output.data(),
                                             static_cast<size_t>(sector), 0x4000,
                                             Core::Crypto::Op::Decrypt);
                    };
                } else if (source.kind == Sources::Kind::Nand) {
                    throw std::runtime_error("NAND requires bis_key_02 in prod.keys");
                }
                std::vector<std::vector<std::filesystem::path>> candidates;
                Sources::Extract(io, source, temporary, decrypt, &candidates);
                for (const auto& files : candidates) {
                    const auto version = FirmwareManager::VerifyFirmware(system, files, progress);
                    if (version.empty()) {
                        extraction_failed = true;
                        next.install_failed = true;
                        Diagnostic("FIRMWARE candidate_verification_failed");
                        continue;
                    }
                    if (Sources::OlderVersion(version, next.version)) {
                        Diagnostic("FIRMWARE source_older_than_installed version=" + version);
                        continue;
                    }
                    if (!best_source || Sources::OlderVersion(best_version, version)) {
                        best_files = files;
                        best_version = version;
                        best_source = &source;
                        best_temporary = temporary;
                    }
                }
                // Failed or partial sources must remain eligible for a later retry.
                if (!extraction_failed && !candidates.empty())
                    verified_sources.push_back(source);
            } catch (const std::exception& error) {
                next.install_failed = true;
                Diagnostic(std::string("FIRMWARE source_failed ") + error.what());
            }
            cleanup.DiscardExcept(best_temporary);
        }
        if (!selected.empty() && !best_source)
            next.install_failed = true;
        if (best_source) {
            Diagnostic("FIRMWARE install_begin count=" + std::to_string(best_files.size()));
            const auto result = FirmwareManager::InstallFirmware(system, best_files, progress);
            next.version = FirmwareManager::InstalledVersion(system);
            next.install_failed = result != FirmwareManager::InstallResult::Success;
            if (!next.install_failed) {
                Diagnostic("FIRMWARE install version=" + next.version +
                           " count=" + std::to_string(best_files.size()));
                // Preserve previous successes and record only completely verified sources.
                records += Sources::ScanRecords(verified_sources);
                if (Common::FS::WriteStringToFile(record_path, Common::FS::FileType::TextFile,
                                                  records) != records.size()) {
                    next.install_failed = true;
                    Diagnostic("FIRMWARE source_record_failed source retained for retry");
                }
            } else {
                Diagnostic("FIRMWARE install_failed result=" +
                           std::to_string(static_cast<int>(result)));
            }
        }
        next.home_available = controller.GetSystemNANDContents()->GetEntryRaw(
                                  static_cast<u64>(Service::AM::AppletProgramId::QLaunch),
                                  FileSys::ContentRecordType::Program) != nullptr;
        next.mii_edit_available = controller.GetSystemNANDContents()->GetEntryRaw(
                                      static_cast<u64>(Service::AM::AppletProgramId::MiiEdit),
                                      FileSys::ContentRecordType::Program) != nullptr;
        Diagnostic("FIRMWARE installed version=" +
                   (next.version.empty() ? std::string("none") : next.version));
    } catch (const winrt::hresult_error& error) {
        next.install_failed = true;
        Diagnostic("FIRMWARE prepare_failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        next.install_failed = true;
        Diagnostic(std::string("FIRMWARE prepare_failed ") + error.what());
    }
    const std::scoped_lock lock(status_mutex);
    status = std::move(next);
}
void PrepareFirmware(const std::filesystem::path& local_state) {
    PrepareFirmware(local_state, {}, [](size_t, size_t) { return false; });
}
void InstallFirmwareSource(const std::filesystem::path& local_state,
                           const std::filesystem::path& source,
                           const std::function<bool(size_t, size_t)>& progress) {
    PrepareFirmware(local_state, source, progress);
}
std::vector<Setup::Candidate> ScanSetupCandidates(const std::filesystem::path& local_state) {
    std::vector<std::filesystem::path> roots;
    for (const auto& root : Files::Roots(local_state))
        roots.push_back(root.path);
    auto io = FirmwareIO();
    const auto list = io.list;
    io.list = [list, local_state](const auto& directory) {
        auto entries = list(directory);
        if (directory == local_state)
            std::erase_if(entries,
                          [&](const auto& item) { return item.path == local_state / "eden"; });
        return entries;
    };
    return Setup::Scan(io, roots);
}
bool InstallSetupKeys(const std::filesystem::path& source) {
    if (!Setup::IsKeys(source))
        return false;
    Common::FS::CreateDirs(Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir));
    const bool ok = FirmwareManager::InstallKeys(Common::FS::PathToUTF8String(source), "keys") ==
                    FirmwareManager::KeyInstallResult::Success;
    if (ok)
        Core::Crypto::KeyManager::Instance().ReloadKeys();
    return ok;
}
std::string SetupKeysStatus(const std::filesystem::path& local_state) {
    std::string result;
    for (const auto* name : {"prod.keys", "title.keys"})
        if (Common::FS::Exists(local_state / "eden" / "keys" / name)) {
            if (!result.empty())
                result += " + ";
            result += name;
        }
    if (result.find("prod.keys") != std::string::npos) {
        for (u64 revision = 32; revision > 0; --revision) {
            const auto key = Core::Crypto::KeyManager::Instance().GetKey(
                Core::Crypto::S128KeyType::Master, revision - 1);
            if (std::any_of(key.begin(), key.end(), [](auto byte) { return byte != 0; })) {
                result += " (r" + std::to_string(revision - 1) + ")";
                break;
            }
        }
    }
    return result;
}
} // namespace EdenXbox
