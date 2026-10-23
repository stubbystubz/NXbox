// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// The Windows headers come first: the Eden file system headers #undef the A/W macros of the Win32
// calls they name their methods after.
#include <windows.h>

#include <fileapifromapp.h>

#include "eden_uwp/ui/library.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/game_download.h"
#include "eden_uwp/ui/strings.h"
#include "frontend_common/library_files.h"

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/nsz.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/art.h"
#include "eden_uwp/usb_library.h"
#include "eden_uwp/file_manager.h"
#include "frontend_common/split_upload.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

// A fresh report lets NxbHelper preflight uploads without a Device Portal drive API.
void WriteStorageReport(const fs::path& local_state) {
    const fs::path temporary = local_state / "storage.json.tmp";
    try {
        ULARGE_INTEGER free_bytes{}, total_bytes{};
        if (!GetDiskFreeSpaceExW(local_state.c_str(), &free_bytes, &total_bytes, nullptr)) {
            winrt::throw_last_error();
        }
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        const auto updated = fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", utc.wYear,
                                         utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
        {
            std::ofstream output(temporary, std::ios::trunc);
            output << fmt::format(R"({{"free":{},"total":{},"updated":"{}"}})", free_bytes.QuadPart,
                                  total_bytes.QuadPart, updated);
            output.close();
            if (!output) {
                throw std::ios_base::failure("storage report write failed");
            }
        }
        const fs::path destination = local_state / "storage.json";
        const bool renamed = fs::exists(destination)
                                 ? ReplaceFileFromAppW(destination.c_str(), temporary.c_str(),
                                                       nullptr, 0, nullptr, nullptr) != 0
                                 : MoveFileFromAppW(temporary.c_str(), destination.c_str()) != 0;
        if (!renamed) {
            winrt::throw_last_error();
        }
    } catch (...) {
        std::error_code cleanup_error;
        fs::remove(temporary, cleanup_error);
        Diagnostic("UI library storage report write failed");
    }
}

// What a cached LocalState\library\<TITLEID16>.json says about a game.
struct CacheRecord {
    std::string title_id;
    std::wstring name;
    std::wstring path;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::vector<std::string> nacp_names;
    std::string icon_language;
    LibraryContent::Classification content;
    LibraryContent::Problem problem = LibraryContent::Problem::None;
    bool has_icon = false;
};

// Use the Xbox system locale, independently of the UI translation or emulation settings.
std::string IconLanguage() {
    try {
        const auto languages =
            winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages();
        if (languages.Size()) {
            auto tag = winrt::to_string(languages.GetAt(0));
            std::transform(tag.begin(), tag.end(), tag.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (tag == "en-gb")
                return "BritishEnglish";
            if (tag == "fr-ca")
                return "CanadianFrench";
            if (tag == "pt-br")
                return "BrazilianPortuguese";
            if (tag.starts_with("es-") && tag != "es-es")
                return "LatinAmericanSpanish";
            if (tag.starts_with("zh"))
                return tag.find("hant") != std::string::npos || tag == "zh-tw" || tag == "zh-hk"
                           ? "TraditionalChinese"
                           : "SimplifiedChinese";
            const std::pair<const char*, const char*> names[] = {
                {"ja", "Japanese"},   {"en", "AmericanEnglish"}, {"fr", "French"}, {"de", "German"},
                {"it", "Italian"},    {"es", "Spanish"},         {"ko", "Korean"}, {"nl", "Dutch"},
                {"pt", "Portuguese"}, {"ru", "Russian"}};
            for (const auto& [prefix, name] : names)
                if (tag == prefix || tag.starts_with(std::string(prefix) + "-"))
                    return name;
        }
    } catch (const winrt::hresult_error&) {
        // A missing locale must not prevent offline library scanning.
    }
    return "AmericanEnglish";
}

// What reading a package yields.
struct PackageInfo {
    std::uint64_t title_id = 0;
    std::wstring name;
    std::vector<std::string> nacp_names;
    std::vector<std::uint8_t> icon; // JPEG bytes, empty when the package has none
    LibraryContent::Classification content;
    LibraryContent::Problem problem = LibraryContent::Problem::None;
    bool keys_problem = false;      // set when the package could not be read for want of keys
};

struct ScanStats {
    int cached = 0;
    int parsed = 0;
    int skipped = 0;
    int key_problems = 0; // skipped for want of keys
};

// Gives the worker thread a COM apartment of its own for the WinRT calls it makes.
class ScopedApartment {
public:
    ScopedApartment() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    ~ScopedApartment() {
        winrt::uninit_apartment();
    }
    ScopedApartment(const ScopedApartment&) = delete;
    ScopedApartment& operator=(const ScopedApartment&) = delete;
};

// Statuses that say a key is missing or wrong, as opposed to a broken or unsupported package.
bool IsKeyError(Loader::ResultStatus status) {
    switch (status) {
    case Loader::ResultStatus::ErrorMissingProductionKeyFile:
    case Loader::ResultStatus::ErrorMissingHeaderKey:
    case Loader::ResultStatus::ErrorIncorrectHeaderKey:
    case Loader::ResultStatus::ErrorMissingTitlekey:
    case Loader::ResultStatus::ErrorMissingTitlekek:
    case Loader::ResultStatus::ErrorMissingKeyAreaKey:
    case Loader::ResultStatus::ErrorIncorrectKeyAreaKey:
    case Loader::ResultStatus::ErrorIncorrectTitlekeyOrTitlekek:
        return true;
    default:
        return false;
    }
}

std::wstring Lowercase(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    return text;
}

std::string Utf8(const std::wstring& text) {
    return winrt::to_string(text);
}


bool ReadRecord(const fs::path& json_file, CacheRecord& record) {
    try {
        std::ifstream in(json_file, std::ios::binary);
        if (!in) {
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        JsonObject object;
        if (!JsonObject::TryParse(winrt::to_hstring(text), object)) {
            return false;
        }
        // Required fields invalidate records written before content classification was cached.
        const auto problem = object.GetNamedNumber(L"problem");
        const auto kind = object.GetNamedNumber(L"content_kind");
        if (!(problem >= 0 &&
              problem <= static_cast<int>(LibraryContent::Problem::MissingFirmware)) ||
            problem != static_cast<int>(problem) ||
            !(kind >= 0 && kind <= static_cast<int>(LibraryContent::Kind::AddOnContent)) ||
            kind != static_cast<int>(kind))
            return false;
        record.problem = static_cast<LibraryContent::Problem>(static_cast<int>(problem));
        record.content.kind = static_cast<LibraryContent::Kind>(static_cast<int>(kind));
        const auto bases = object.GetNamedArray(L"content_bases");
        for (uint32_t i = 0; i < bases.Size(); ++i)
            record.content.bases.push_back(
                std::stoull(winrt::to_string(bases.GetAt(i).GetString()), nullptr, 16));
        record.has_icon = object.GetNamedBoolean(L"has_icon");
        record.icon_language = winrt::to_string(object.GetNamedString(L"icon_language"));
        record.title_id = winrt::to_string(object.GetNamedString(L"title_id"));
        record.name = object.GetNamedString(L"name").c_str();
        record.path = object.GetNamedString(L"path").c_str();
        record.size = static_cast<std::uint64_t>(object.GetNamedNumber(L"size"));
        record.mtime = static_cast<std::int64_t>(object.GetNamedNumber(L"mtime"));
        // A record from before the names were cached has no such array: it is rebuilt, which is
        // how the names get into the cache.
        const winrt::Windows::Data::Json::JsonArray names = object.GetNamedArray(L"nacp_names");
        for (uint32_t i = 0; i < names.Size(); ++i) {
            record.nacp_names.push_back(winrt::to_string(names.GetAt(i).GetString()));
        }
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    } catch (const std::exception&) {
        return false; // a field is missing or has the wrong type: the entry is rebuilt
    }
}

bool WriteRecord(const fs::path& json_file, const GameEntry& game) {
    try {
        JsonObject object;
        object.SetNamedValue(L"problem",
                             JsonValue::CreateNumberValue(static_cast<int>(game.problem)));
        object.SetNamedValue(L"content_kind",
                             JsonValue::CreateNumberValue(static_cast<int>(game.content.kind)));
        winrt::Windows::Data::Json::JsonArray bases;
        for (const auto base : game.content.bases)
            bases.Append(
                JsonValue::CreateStringValue(winrt::to_hstring(fmt::format("{:016X}", base))));
        object.SetNamedValue(L"content_bases", bases);
        object.SetNamedValue(L"has_icon", JsonValue::CreateBooleanValue(!game.icon.empty()));
        object.SetNamedValue(L"icon_language",
                             JsonValue::CreateStringValue(winrt::to_hstring(IconLanguage())));
        object.SetNamedValue(L"title_id",
                             JsonValue::CreateStringValue(winrt::to_hstring(game.title_id)));
        object.SetNamedValue(L"name", JsonValue::CreateStringValue(game.name));
        object.SetNamedValue(L"path", JsonValue::CreateStringValue(game.path.wstring()));
        object.SetNamedValue(L"size", JsonValue::CreateNumberValue(static_cast<double>(game.size)));
        object.SetNamedValue(L"mtime",
                             JsonValue::CreateNumberValue(static_cast<double>(game.mtime)));
        winrt::Windows::Data::Json::JsonArray names;
        for (const std::string& name : game.nacp_names) {
            names.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
        }
        object.SetNamedValue(L"nacp_names", names);
        std::ofstream out(json_file, std::ios::binary | std::ios::trunc);
        out << winrt::to_string(object.Stringify());
        return static_cast<bool>(out);
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

// The control NCA of the base game, which holds control.nacp and the icons.
std::shared_ptr<FileSys::NCA> FindControlNca(const FileSys::NSP& nsp, std::uint64_t title_id) {
    if (auto control = nsp.GetNCA(title_id, FileSys::ContentRecordType::Control)) {
        return control;
    }
    // Multi-program titles can keep their control data under another title ID.
    for (const auto& title : nsp.GetNCAs()) {
        for (const auto& entry : title.second) {
            const auto& [title_type, record_type] = entry.first;
            if (title_type == FileSys::TitleType::Application &&
                record_type == FileSys::ContentRecordType::Control) {
                return entry.second;
            }
        }
    }
    return nullptr;
}

// Reads a package's title ID, name and icon. Returns why the package cannot be used, or an empty
// string on success.
std::string ReadPackage(FileSys::RealVfsFilesystem& vfs, const fs::path& file, PackageInfo& info,
                        bool read_details) {
    const FileSys::VirtualFile source =
        vfs.OpenFile(Common::FS::PathToUTF8String(file), FileSys::OpenMode::Read);
    if (!source) {
        return "cannot open the file";
    }

    std::shared_ptr<FileSys::NSP> nsp;
    if (Lowercase(file.extension().wstring()) == L".xci") {
        const FileSys::XCI xci(source, 0, 0, /*program_headers_only=*/true);
        if (xci.GetStatus() != Loader::ResultStatus::Success) {
            info.problem = LibraryContent::LoadProblem(xci.GetStatus());
            return "XCI: " + Loader::GetResultStatusString(xci.GetStatus());
        }
        nsp = xci.GetSecurePartitionNSP();
    } else {
        nsp = std::make_shared<FileSys::NSP>(source, 0, 0, /*program_headers_only=*/true);
    }
    if (!nsp) {
        return "no secure partition";
    }
    if (nsp->GetStatus() != Loader::ResultStatus::Success) {
        info.problem = LibraryContent::LoadProblem(nsp->GetStatus());
        return "NSP: " + Loader::GetResultStatusString(nsp->GetStatus());
    }
    if (nsp->IsExtractedType()) {
        return "extracted NSP, it has no control data";
    }

    std::vector<LibraryContent::Meta> meta;
    for (const auto& [id, type] : nsp->GetContentMeta()) {
        const auto kind = type == FileSys::TitleType::Application
                              ? LibraryContent::Kind::Application
                          : type == FileSys::TitleType::Update ? LibraryContent::Kind::Patch
                          : type == FileSys::TitleType::AOC    ? LibraryContent::Kind::AddOnContent
                                                               : LibraryContent::Kind::Unknown;
        meta.push_back({id, kind});
    }
    info.content = LibraryContent::Classify(meta);
    info.problem = LibraryContent::Standalone(info.content.kind);
    info.title_id = meta.empty() ? 0 : meta.front().title_id;
    if (!meta.empty() && info.content.kind == LibraryContent::Kind::Unknown)
        return "unsupported content type";
    if (nsp->HasCorruptProgram())
        info.problem = LibraryContent::Problem::Corrupted;
    else if (nsp->HasMissingProgramKeys())
        info.problem = LibraryContent::Problem::MissingKeys;
    if (info.problem != LibraryContent::Problem::None)
        return {};
    if (info.content.kind == LibraryContent::Kind::Application) {
        bool has_program = false;
        for (const auto& title : nsp->GetNCAs()) {
            has_program |= title.second.contains(
                {FileSys::TitleType::Application, FileSys::ContentRecordType::Program});
        }
        if (!has_program) {
            info.problem = LibraryContent::Problem::Corrupted;
            return {};
        }
    }
    if (info.content.kind == LibraryContent::Kind::Unknown) {
        const auto status = nsp->GetProgramStatus();
        info.keys_problem = IsKeyError(status) || !Core::Crypto::KeyManager::Instance().HasKey(
                                                      Core::Crypto::S256KeyType::Header);
        info.problem = info.keys_problem ? LibraryContent::Problem::MissingKeys
                                         : LibraryContent::Problem::None;
        return info.keys_problem ? "" : "unsupported content type";
    }
    if (!read_details)
        return {};

    info.title_id = nsp->GetProgramTitleID();
    if (info.title_id == 0) {
        // Without the keys no NCA opens, and the status kept for the program says exactly that.
        const Loader::ResultStatus status = nsp->GetProgramStatus();
        info.keys_problem = IsKeyError(status);
        return status == Loader::ResultStatus::Success
                   ? "no base-game program NCA (update or DLC only?)"
                   : "NSP: " + Loader::GetResultStatusString(status);
    }
    const std::shared_ptr<FileSys::NCA> control = FindControlNca(*nsp, info.title_id);
    if (!control) {
        return "no control NCA";
    }
    if (control->GetStatus() != Loader::ResultStatus::Success) {
        info.keys_problem = IsKeyError(control->GetStatus());
        return "control NCA: " + Loader::GetResultStatusString(control->GetStatus());
    }
    const FileSys::VirtualFile romfs = control->GetRomFS();
    if (!romfs) {
        return "control NCA has no RomFS";
    }
    const FileSys::VirtualDir content = FileSys::ExtractRomFS(romfs);
    if (!content) {
        return "control RomFS is unreadable";
    }
    FileSys::VirtualFile nacp_file = content->GetFile("control.nacp");
    if (!nacp_file) {
        nacp_file = content->GetFile("Control.nacp");
    }
    if (!nacp_file) {
        return "no control.nacp";
    }
    const FileSys::NACP nacp(nacp_file);
    info.name = winrt::to_hstring(nacp.GetApplicationName()).c_str();
    info.nacp_names = nacp.GetApplicationNames();
    if (info.name.empty()) {
        info.name = file.stem().wstring();
    }

    // Reuse the library's control RomFS extraction; prefer the system language, then any icon.
    FileSys::VirtualFile icon = content->GetFile("icon_" + IconLanguage() + ".dat");
    if (!icon) {
        for (const FileSys::VirtualFile& candidate : content->GetFiles()) {
            const std::string name = candidate->GetName();
            if (name.starts_with("icon_") && name.ends_with(".dat")) {
                icon = candidate;
                break;
            }
        }
    }
    if (icon) {
        info.icon = icon->ReadAllBytes();
    }
    return {};
}

std::optional<bool> TruncatedPackage(FileSys::RealVfsFilesystem& vfs, const fs::path& path) {
    const auto file = vfs.OpenFile(Common::FS::PathToUTF8String(path), FileSys::OpenMode::Read);
    if (!file)
        return std::nullopt;
    // RealVfsFile::GetSize() returns zero when opening the underlying handle fails.
    // Query the actual length separately so an unreadable NSP cannot authorize replacement.
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return std::nullopt;
    const auto size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return LibraryFiles::TruncatedPfs0(size, [&](std::uint64_t offset, std::size_t size) {
        return file->ReadBytes(size, offset);
    });
}

// One game from one file: the cache when it still matches, otherwise the package itself.
std::optional<GameEntry> LoadGame(FileSys::RealVfsFilesystem& vfs, const fs::path& library_dir,
                                  const std::map<std::wstring, CacheRecord>& cached,
                                  const fs::path& file, ScanStats& stats) {
    GameEntry game;
    game.path = file;
    game.size = Common::FS::GetSize(file);
    game.mtime = ModifiedSeconds(file);

    const auto hit = cached.find(file.wstring());
    bool cache_valid = hit != cached.end() && game.size != 0 && hit->second.size == game.size &&
                       hit->second.mtime == game.mtime &&
                       hit->second.icon_language == IconLanguage() &&
                       // Keys can be added without touching the file: re-check those entries.
                       hit->second.problem != LibraryContent::Problem::MissingKeys;
    if (cache_valid && hit->second.has_icon) {
        std::error_code icon_error;
        const auto icon = library_dir / (hit->second.title_id + ".control.jpg");
        cache_valid = fs::file_size(icon, icon_error) > 0 && !icon_error;
    }
    if (cache_valid) {
        game.title_id = hit->second.title_id;
        game.name = hit->second.name;
        game.nacp_names = hit->second.nacp_names;
        game.content = hit->second.content;
        game.problem = hit->second.problem;
        game.incomplete = game.problem != LibraryContent::Problem::None;
        if (game.incomplete)
            game.name = file.stem().wstring() + L" — " + Tr(LibraryContent::Label(game.problem));
        if (hit->second.has_icon)
            game.icon = library_dir / (game.title_id + ".control.jpg");
        stats.key_problems += game.problem == LibraryContent::Problem::MissingKeys ? 1 : 0;
        ++stats.cached;
        return game;
    }
    // Cache problem entries by path too, including files without a readable title ID.
    const auto remember_problem = [&] {
        std::uint64_t hash = 14695981039346656037ull;
        for (const unsigned char byte : Utf8(file.wstring())) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        if (!WriteRecord(library_dir / fmt::format("problem-{:016x}.json", hash), game))
            Diagnostic("UI library problem cache write failed");
    };
    if (Lowercase(file.extension().wstring()) == L".nsp" &&
        TruncatedPackage(vfs, file).value_or(false)) {
        game.incomplete = true;
        game.problem = LibraryContent::Problem::Corrupted;
        game.name = file.stem().wstring() + L" — " + Tr(Text::IncompleteFile);
        remember_problem();
        return game;
    }
    // Changed files and legacy records must be classified again.
    PackageInfo info;
    std::string reason;
    try {
        reason = ReadPackage(vfs, file, info, true);
    } catch (const std::exception& failure) {
        reason = failure.what();
    }
    game.content = info.content;
    game.problem = info.problem;
    if (game.problem != LibraryContent::Problem::None) {
        game.incomplete = true;
        stats.key_problems += game.problem == LibraryContent::Problem::MissingKeys ? 1 : 0;
        if (info.title_id)
            game.title_id = fmt::format("{:016X}", info.title_id);
        game.name = file.stem().wstring() + L" — " + Tr(LibraryContent::Label(game.problem));
        Diagnostic("UI library unplayable " + Utf8(file.filename().wstring()) +
                   " reason=" + std::to_string(static_cast<int>(game.problem)));
        remember_problem();
        return game;
    }
    if (!reason.empty()) {
        Diagnostic("UI library skip " + Utf8(file.filename().wstring()) + ": " + reason);
        ++stats.skipped;
        stats.key_problems += info.keys_problem ? 1 : 0;
        return std::nullopt;
    }

    game.title_id = fmt::format("{:016X}", info.title_id);
    game.name = info.name;
    game.nacp_names = info.nacp_names;
    const fs::path icon = library_dir / (game.title_id + ".control.jpg");
    std::error_code ec;
    // Source or locale changed: invalidate all derived art, even if the new package has no icon.
    fs::remove(icon, ec);
    fs::remove(library_dir / (game.title_id + ".control.bmp"), ec);
    fs::remove(library_dir / (game.title_id + ".control.hero-v1.bmp"), ec);
    if (!info.icon.empty() && Art::WriteBytes(icon, info.icon))
        game.icon = icon;
    if (!WriteRecord(library_dir / (game.title_id + ".json"), game)) {
        Diagnostic("UI library cache write failed for " + game.title_id);
    }
    ++stats.parsed;
    return game;
}

// A successful conversion may live on another drive while the original NSZ is read-only.
// Persist the source/output association, rather than inferring completion from sibling names.
fs::path ConversionRecordPath(const fs::path& local_state, const fs::path& source) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : Utf8(LibraryFiles::Fold(source.wstring()))) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return local_state / "library" / fmt::format("nsz-{:016x}.json", hash);
}

// "Game [0100...][v0]" -> "Game": the bracketed ids make a toast unreadable.
std::wstring DisplayName(const fs::path& file) {
    const std::wstring name = file.stem().wstring();
    return name.substr(0, name.find(L" ["));
}

fs::path FailureRecordPath(const fs::path& local_state, const fs::path& source) {
    auto path = ConversionRecordPath(local_state, source);
    path.replace_extension(L".failed.json");
    return path;
}

std::optional<std::string> PreviousFailure(const fs::path& record_path, const fs::path& source) {
    try {
        std::ifstream input(record_path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        JsonObject record;
        if (JsonObject::TryParse(winrt::to_hstring(text), record) &&
            std::wstring(record.GetNamedString(L"source")) == source.native() &&
            NszFailure::Unchanged(
                std::stoull(winrt::to_string(record.GetNamedString(L"source_size"))),
                std::stoll(winrt::to_string(record.GetNamedString(L"source_mtime"))),
                Common::FS::GetSize(source), ModifiedSeconds(source))) {
            const auto reason = winrt::to_string(record.GetNamedString(L"failure"));
            if (NszFailure::ShouldRemember(reason, false))
                return reason;
        }
    } catch (...) {
        // Missing or invalid records must not prevent a fresh attempt.
    }
    std::error_code ec;
    fs::remove(record_path, ec);
    return std::nullopt;
}

void RememberFailure(const fs::path& record_path, const fs::path& source, std::uint64_t size,
                     std::int64_t mtime, const std::string& failure) {
    try {
        JsonObject record;
        record.SetNamedValue(L"source", JsonValue::CreateStringValue(source.native()));
        // Strings preserve the full integer precision in JSON.
        record.SetNamedValue(L"source_size", JsonValue::CreateStringValue(std::to_wstring(size)));
        record.SetNamedValue(L"source_mtime", JsonValue::CreateStringValue(std::to_wstring(mtime)));
        record.SetNamedValue(L"failure", JsonValue::CreateStringValue(winrt::to_hstring(failure)));
        fs::create_directories(record_path.parent_path());
        const auto text = winrt::to_string(record.Stringify());
        if (Common::FS::WriteStringToFile(record_path, Common::FS::FileType::TextFile, text) !=
            text.size())
            Diagnostic("NSZ_CONVERT failure_record_write_failed");
    } catch (...) {
        Diagnostic("NSZ_CONVERT failure_record_write_failed");
    }
}

std::optional<fs::path> ConvertedOutput(const fs::path& record_path, const fs::path& source) {
    try {
        std::ifstream input(record_path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        JsonObject record;
        if (!JsonObject::TryParse(winrt::to_hstring(text), record) ||
            std::wstring(record.GetNamedString(L"source")) != source.native() ||
            record.GetNamedNumber(L"source_size") !=
                static_cast<double>(Common::FS::GetSize(source)) ||
            record.GetNamedNumber(L"source_mtime") != static_cast<double>(ModifiedSeconds(source)))
            return std::nullopt;
        const fs::path output{std::wstring(record.GetNamedString(L"output"))};
        const auto size = Common::FS::GetSize(output);
        if (size != 0 && record.GetNamedNumber(L"output_size") == static_cast<double>(size))
            return output;
    } catch (...) {
        // Missing, changed or disconnected outputs remain eligible for conversion.
    }
    return std::nullopt;
}

void RememberConversion(const fs::path& record_path, const fs::path& source,
                        const fs::path& output) {
    try {
        JsonObject record;
        record.SetNamedValue(L"source", JsonValue::CreateStringValue(source.native()));
        record.SetNamedValue(L"source_size", JsonValue::CreateNumberValue(
                                                 static_cast<double>(Common::FS::GetSize(source))));
        record.SetNamedValue(L"source_mtime", JsonValue::CreateNumberValue(
                                                  static_cast<double>(ModifiedSeconds(source))));
        record.SetNamedValue(L"output", JsonValue::CreateStringValue(output.native()));
        record.SetNamedValue(L"output_size", JsonValue::CreateNumberValue(
                                                 static_cast<double>(Common::FS::GetSize(output))));
        fs::create_directories(record_path.parent_path());
        auto temporary = record_path;
        temporary += L".tmp";
        const auto text = winrt::to_string(record.Stringify());
        if (Common::FS::WriteStringToFile(temporary, Common::FS::FileType::TextFile, text) !=
                text.size() ||
            !(Common::FS::Exists(record_path)
                  ? ReplaceFileFromAppW(record_path.c_str(), temporary.c_str(), nullptr, 0, nullptr,
                                        nullptr)
                  : MoveFileFromAppW(temporary.c_str(), record_path.c_str())))
            Diagnostic("NSZ_CONVERT completion_record_failed");
    } catch (...) {
        Diagnostic("NSZ_CONVERT completion_record_failed");
    }
}

// Converts one .nsz into the .nsp at `nsp_path`. The converter writes to "<name>.nsp.partial",
// which is renamed only when the conversion is complete, so a half-written .nsp never exists under
// its real name. Returns why it failed, or an empty string when the .nsp is complete.
std::string ConvertNsz(FileSys::RealVfsFilesystem& vfs, const fs::path& nsz_path,
                       const fs::path& nsp_path,
                       const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                       const std::atomic<bool>& cancel) {
    fs::path partial = nsp_path;
    partial += L".partial";
    std::string error;
    bool converted = false;
    try {
        // Both files are closed at the end of this block, before the rename or the delete.
        const FileSys::VirtualFile source =
            vfs.OpenFile(Common::FS::PathToUTF8String(nsz_path), FileSys::OpenMode::Read);
        if (!source) {
            return "cannot open the file";
        }
        if (!FileSys::IsNsz(source)) {
            // An unfinished download leaves zeros where the container header goes: that copy is
            // damaged, while a real PFS0/HFS0 without compressed entries is only another format.
            const auto magic = source->ReadBytes(4, 0);
            const auto is = [&](std::string_view expected) {
                return magic.size() == 4 && std::equal(magic.begin(), magic.end(), expected.begin());
            };
            return is("PFS0") || is("HFS0") ? "not an NSZ package"
                                            : "corrupt header: not a PFS0 container";
        }
        const FileSys::VirtualFile output =
            vfs.CreateFile(Common::FS::PathToUTF8String(partial), FileSys::OpenMode::ReadWrite);
        if (!output) {
            void(Common::FS::RemoveFile(partial)); // an empty file may have been left behind
            return "cannot create " + Utf8(partial.filename().wstring());
        }
        converted = FileSys::ConvertNszToNsp(source, output, progress, &error, &cancel);
    } catch (const std::exception& failure) {
        // The scan goes on with the other packages; the partial output is removed below.
        converted = false;
        error = failure.what();
    }
    if (!converted) {
        // The converter leaves its output undefined after a failure.
        void(Common::FS::RemoveFile(partial));
        return error.empty() ? "the conversion failed" : error;
    }
    if (Common::FS::GetSize(partial) == 0) {
        void(Common::FS::RemoveFile(partial));
        return "the converter wrote an empty file";
    }
    // Validate the finished temporary file before replacing an interrupted copy.
    if (TruncatedPackage(vfs, partial) != std::optional<bool>{false}) {
        void(Common::FS::RemoveFile(partial));
        return "the converted PFS0 is incomplete or unreadable";
    }
    const bool replacing = Common::FS::IsFile(nsp_path);
    if (replacing && TruncatedPackage(vfs, nsp_path) != std::optional<bool>{true}) {
        void(Common::FS::RemoveFile(partial));
        return "existing output is complete or unreadable";
    }
    // Keep the normal broker rename; PublishDownload adds StorageFile for removable storage.
    if (!Common::FS::RenameFile(partial, nsp_path) && !PublishDownload(partial, nsp_path, error)) {
        const std::string reason = "cannot rename the output";
        void(Common::FS::RemoveFile(partial));
        return reason;
    }
    Diagnostic("NSZ_CONVERT replaced_truncated=" + std::string(replacing ? "true" : "false") +
               " path=" + Utf8(nsp_path.wstring()));
    return {};
}
} // namespace

LibraryContent::Problem LibraryFileProblem(const fs::path& file) {
    if (!LibraryContent::IsPackage(file))
        return LibraryContent::Problem::None;
    try {
        FileSys::RealVfsFilesystem vfs;
        if (Lowercase(file.extension().wstring()) == L".nsp" &&
            TruncatedPackage(vfs, file).value_or(false))
            return LibraryContent::Problem::Corrupted;
        PackageInfo info;
        ReadPackage(vfs, file, info, false);
        return info.problem;
    } catch (...) {
        return LibraryContent::Problem::None;
    }
}

std::int64_t ModifiedSeconds(const fs::path& file) {
    std::error_code error;
    const auto stamp = fs::last_write_time(file, error);
    if (error) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExFromAppW(file.c_str(), GetFileExInfoStandard, &data)) {
            return 0;
        }
        const auto ticks = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                           data.ftLastWriteTime.dwLowDateTime;
        return static_cast<std::int64_t>(ticks / 10000000);
    }
    return std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count();
}

namespace {
std::vector<fs::path> DeleteRoots(const fs::path& local_state) {
    auto roots = ExternalGameFolders();
    roots.push_back(local_state / "games");
    return roots;
}

bool SafeDeletePath(const fs::path& path, const std::vector<fs::path>& roots) {
    if (!LibraryFiles::Allowed(path, roots) || !LibraryFiles::IsPackage(path))
        return false;
    // Discovery supplies trusted roots. Inspect the root and every child, without querying
    // protected ancestors such as C:\Users that a UWP app cannot inspect even for LocalState.
    for (const auto& root : roots) {
        if (!LibraryFiles::Allowed(path, {root}))
            continue;
        auto prefix = path;
        for (;;) {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExFromAppW(prefix.c_str(), GetFileExInfoStandard, &data) ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return false;
            if (prefix == path && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                return false;
            if (LibraryFiles::Fold(prefix.wstring()) == LibraryFiles::Fold(root.wstring()))
                break;
            prefix = prefix.parent_path();
        }
        return true;
    }
    return false;
}

bool RemoveLibraryFile(const fs::path& path, std::string& reason) {
    std::error_code ec;
    if (fs::remove(path, ec))
        return true;
    reason = "filesystem: " + ec.message();
    if (DeleteFileFromAppW(path.c_str()))
        return true;
    reason += fmt::format("; DeleteFileFromApp: error {}", GetLastError());
    try {
        using namespace winrt::Windows::Storage;
        const auto file =
            AwaitBounded(StorageFile::GetFileFromPathAsync(path.native()), std::chrono::seconds(5));
        if (!file) {
            reason += "; StorageFile: timed out";
            return false;
        }
        auto deleting = file->DeleteAsync(StorageDeleteOption::PermanentDelete);
        const auto status = deleting.wait_for(std::chrono::seconds(30));
        if (status == winrt::Windows::Foundation::AsyncStatus::Completed)
            return true;
        if (status == winrt::Windows::Foundation::AsyncStatus::Error)
            reason += fmt::format("; StorageFile: HRESULT 0x{:08x}",
                                  static_cast<std::uint32_t>(deleting.ErrorCode().value));
        else {
            deleting.Cancel();
            reason += "; StorageFile: timed out";
        }
    } catch (const winrt::hresult_error& failure) {
        reason += fmt::format("; StorageFile: HRESULT 0x{:08x} {}",
                              static_cast<std::uint32_t>(failure.code().value),
                              winrt::to_string(failure.message()));
    }
    return false;
}
} // namespace

std::vector<LibraryDeleteFile> LibraryDeleteCandidates(const GameEntry& game,
                                                       const fs::path& local_state) {
    ScopedApartment apartment;
    std::vector<LibraryDeleteFile> result;
    const auto roots = DeleteRoots(local_state);
    if (!SafeDeletePath(game.path, roots))
        return result;
    std::set<fs::path> candidates{game.path};
    // Local subfolders are scanned by the library too. StorageFolder enumeration reaches USB.
    std::error_code ec;
    fs::recursive_directory_iterator it(local_state / "games",
                                        fs::directory_options::skip_permission_denied, ec),
        end;
    while (!ec && it != end) {
        candidates.insert(it->path());
        it.increment(ec);
    }
    auto folders = roots;
    folders.push_back(game.path.parent_path());
    for (std::size_t index = 0; index < folders.size(); ++index) {
        const auto folder = folders[index];
        try {
            const auto storage = AwaitBounded(
                winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(folder.native()),
                std::chrono::seconds(5));
            if (!storage)
                continue;
            const auto files = AwaitBounded(storage->GetFilesAsync(), std::chrono::seconds(5));
            if (files)
                for (const auto& file : *files)
                    candidates.emplace(std::wstring(file.Path()));
            const auto children = AwaitBounded(storage->GetFoldersAsync(), std::chrono::seconds(5));
            if (children) {
                for (const auto& child : *children) {
                    const fs::path path{std::wstring(child.Path())};
                    WIN32_FILE_ATTRIBUTE_DATA data{};
                    if (LibraryFiles::Allowed(path, roots) &&
                        GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &data) &&
                        !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                        std::find(folders.begin(), folders.end(), path) == folders.end())
                        folders.push_back(path);
                }
            }
        } catch (const winrt::hresult_error& failure) {
            Diagnostic("LIBRARY_DELETE list failed reason=" + winrt::to_string(failure.message()));
        }
    }
    const auto base = game.title_id.empty() ? LibraryFiles::FilenameTitle(game.path)
                                            : LibraryFiles::ParseTitle(game.title_id);
    std::set<std::wstring> seen_paths;
    for (auto candidate = candidates.begin(); candidate != candidates.end();) {
        if (seen_paths.insert(LibraryFiles::Fold(candidate->wstring())).second)
            ++candidate;
        else
            candidate = candidates.erase(candidate);
    }
    FileSys::RealVfsFilesystem vfs;
    for (const auto& path : candidates) {
        if (!SafeDeletePath(path, roots))
            continue;
        const bool sibling = LibraryFiles::Sibling(game.path, path);
        bool related = false;
        if (!sibling && base) {
            const auto id = LibraryFiles::FilenameTitle(path);
            related = id && LibraryFiles::Related(*base, *id);
            // Complete update/DLC packages need no title ID in their filename.
            if (!id && Lowercase(path.extension().wstring()) == L".nsp") {
                const auto file =
                    vfs.OpenFile(Common::FS::PathToUTF8String(path), FileSys::OpenMode::Read);
                if (file && TruncatedPackage(vfs, path) == std::optional<bool>{false}) {
                    const FileSys::NSP package(file);
                    for (const auto& [title_id, nca] : package.GetNCAsByTitleID())
                        related |= LibraryFiles::Related(*base, title_id);
                }
            }
        }
        if (sibling || related)
            result.push_back({path, Common::FS::GetSize(path), !sibling, sibling});
    }
    // Include converted/download sidecars even when only the update NSP exposed its title ID.
    const auto identified = result;
    for (const auto& path : candidates) {
        if (!SafeDeletePath(path, roots) ||
            std::any_of(result.begin(), result.end(),
                        [&](const auto& file) { return file.path == path; }))
            continue;
        for (const auto& file : identified) {
            if (LibraryFiles::Sibling(file.path, path)) {
                result.push_back({path, Common::FS::GetSize(path), file.optional, !file.optional});
                break;
            }
        }
    }
    return result;
}

bool DeleteLibraryFiles(const std::vector<LibraryDeleteFile>& files, const fs::path& local_state) {
    ScopedApartment apartment;
    const auto roots = DeleteRoots(local_state);
    bool success = true;
    for (const auto& file : files) {
        if (!file.selected)
            continue;
        std::string reason;
        const bool allowed = SafeDeletePath(file.path, roots);
        const bool ok = allowed && RemoveLibraryFile(file.path, reason);
        if (!allowed)
            reason =
                "outside scanned game folders, reparse point, unreadable attributes or not a file";
        Diagnostic("LIBRARY_DELETE path=" + Utf8(file.path.wstring()) +
                   (ok ? " ok reason=deleted" : " failed reason=" + reason));
        success &= ok;
    }
    return success;
}

LibraryTransfer::Result MoveLibraryGame(const GameEntry& game, const fs::path& local_state,
                                        LibraryMoveProgress& progress) {
    ScopedApartment apartment;
    using Result = LibraryTransfer::Result;
    const std::vector<fs::path> internal{local_state / "games"};
    if (!SafeDeletePath(game.path, internal))
        return Result::NotInternal;
    const auto folders = ExternalGameFolders();
    if (folders.empty())
        return Result::NoDrive;
    // The drive with the most room: enumeration order put a nearly full stick ahead of an empty
    // HD, and every bigger game then failed with no space.
    fs::path destination;
    std::uint64_t most_free = 0;
    for (const auto& folder : folders) {
        const auto free = FreeSpace(folder).value_or(0);
        if (destination.empty() || free > most_free) {
            destination = folder;
            most_free = free;
        }
    }
    std::vector<LibraryTransfer::File> files;
    std::map<fs::path, std::int64_t> modified;
    std::set<std::wstring> names;
    for (const auto& candidate : LibraryDeleteCandidates(game, local_state)) {
        if (!SafeDeletePath(candidate.path, internal))
            continue;
        const auto relative = candidate.path.lexically_relative(internal.front());
        if (!names.insert(LibraryFiles::Fold(relative.wstring())).second)
            return Result::Conflict;
        files.push_back({candidate.path,
                         LibraryTransfer::Target(candidate.path, internal.front(), destination),
                         candidate.size});
        modified.emplace(candidate.path, ModifiedSeconds(candidate.path));
        if (candidate.size > (std::numeric_limits<std::uint64_t>::max)() - progress.total.load())
            return Result::Failed;
        progress.total.fetch_add(candidate.size);
    }
    LibraryTransfer::IO io;
    io.exists = [](const auto& path) { return Common::FS::Exists(path); };
    io.size = [](const auto& path) { return Common::FS::GetSize(path); };
    io.unchanged = [&](const auto& file) {
        return SafeDeletePath(file.source, internal) &&
               Common::FS::GetSize(file.source) == file.size &&
               ModifiedSeconds(file.source) == modified.at(file.source);
    };
    io.copy = [&](const auto& file) {
        if (!Common::FS::CreateDirs(file.target.parent_path()))
            return false;
        auto partial = file.target;
        partial += L".move.partial";
        struct Handles {
            HANDLE input = INVALID_HANDLE_VALUE;
            HANDLE output = INVALID_HANDLE_VALUE;
            fs::path partial;
            ~Handles() {
                if (input != INVALID_HANDLE_VALUE)
                    CloseHandle(input);
                if (output != INVALID_HANDLE_VALUE)
                    CloseHandle(output);
                if (!partial.empty())
                    DeleteFileFromAppW(partial.c_str());
            }
        } handles;
        handles.input =
            CreateFileFromAppW(file.source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handles.input == INVALID_HANDLE_VALUE)
            return false;
        handles.output = CreateFileFromAppW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handles.output == INVALID_HANDLE_VALUE)
            return false;
        handles.partial = partial;
        std::vector<char> buffer(1024 * 1024);
        std::uint64_t copied = 0;
        while (copied < file.size) {
            const DWORD count =
                static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), file.size - copied));
            DWORD read = 0, written = 0;
            if (!ReadFile(handles.input, buffer.data(), count, &read, nullptr) || read != count ||
                !WriteFile(handles.output, buffer.data(), read, &written, nullptr) ||
                written != read)
                return false;
            copied += written;
            progress.done.fetch_add(written);
        }
        if (!FlushFileBuffers(handles.output))
            return false;
        CloseHandle(handles.output);
        handles.output = INVALID_HANDLE_VALUE;
        if (Common::FS::GetSize(partial) != file.size || Common::FS::Exists(file.target))
            return false;
        if (!MoveFileFromAppW(partial.c_str(), file.target.c_str())) {
            try {
                const auto storage = AwaitBounded(
                    winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(partial.native()),
                    std::chrono::seconds(5));
                if (!storage)
                    return false;
                auto renaming = storage->RenameAsync(
                    file.target.filename().native(),
                    winrt::Windows::Storage::NameCollisionOption::FailIfExists);
                if (renaming.wait_for(std::chrono::seconds(30)) !=
                    winrt::Windows::Foundation::AsyncStatus::Completed) {
                    renaming.Cancel();
                    return false;
                }
                renaming.get();
            } catch (...) {
                return false;
            }
        }
        handles.partial.clear();
        return true;
    };
    io.remove = [](const auto& path) {
        std::string reason;
        const bool removed = RemoveLibraryFile(path, reason);
        Diagnostic("LIBRARY_MOVE source=" + Utf8(path.wstring()) +
                   (removed ? " removed" : " retained reason=" + reason));
        return removed;
    };
    return LibraryTransfer::Move(files, FreeSpace(destination), io);
}

LibraryScan::LibraryScan(std::filesystem::path local_state, bool convert_nsz)
    : local_state_(std::move(local_state)), convert_nsz_(convert_nsz) {
    worker_ = std::thread([this] { Run(); });
}

LibraryScan::~LibraryScan() {
    cancel_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool LibraryScan::Finished() const {
    return finished_.load(std::memory_order_acquire);
}

int LibraryScan::Done() const {
    return done_.load();
}

int LibraryScan::Total() const {
    return total_.load();
}

bool LibraryScan::KeysMissing() const {
    return keys_missing_.load();
}

LibraryScan::Conversion LibraryScan::CurrentConversion() const {
    const std::lock_guard<std::mutex> lock(conversion_mutex_);
    return conversion_;
}

std::vector<GameEntry> LibraryScan::Take() {
    return std::move(games_);
}

const std::vector<LibraryScan::FailedConversion>& LibraryScan::FailedConversions() const {
    return failed_conversions_;
}

void LibraryScan::ConvertCompressed(const std::vector<fs::path>& compressed,
                                    std::vector<fs::path>& packages) {
    // An .nsz with an .nsp next to it has been converted before (or the player copied both): the
    // .nsp is the package, and nothing is deleted.
    std::vector<std::pair<fs::path, fs::path>> pending; // .nsz and the .nsp it becomes
    std::optional<std::vector<fs::path>> external; // removable NXbox\games folders, looked up once
    FileSys::RealVfsFilesystem probe;
    for (const fs::path& nsz : compressed) {
        if (const auto completed = ConvertedOutput(ConversionRecordPath(local_state_, nsz), nsz)) {
            packages.push_back(*completed);
            Diagnostic("NSZ_CONVERT already_converted path=" + Utf8(completed->wstring()));
            continue;
        }
        fs::path nsp = nsz;
        nsp.replace_extension(L".nsp");
        if (!external) {
            external = ExternalGameFolders();
        }
        fs::path existing;
        if (Common::FS::Exists(nsp)) {
            existing = nsp;
        }
        for (const fs::path& folder : *external) {
            if (existing.empty() && Common::FS::Exists(folder / nsp.filename())) {
                existing = folder / nsp.filename();
            }
        }
        if (!existing.empty() && TruncatedPackage(probe, existing) != std::optional<bool>{true}) {
            Diagnostic("UI nsz convert " + Utf8(nsz.filename().wstring()) + " skip " +
                       Utf8(existing.wstring()) + " already exists");
            packages.push_back(existing);
            if (TruncatedPackage(probe, existing) == std::optional<bool>{false})
                RememberConversion(ConversionRecordPath(local_state_, nsz), nsz, existing);
            continue;
        }
        if (const auto failure = PreviousFailure(FailureRecordPath(local_state_, nsz), nsz)) {
            Diagnostic("UI nsz convert " + Utf8(nsz.filename().wstring()) +
                       " skip: failed before (" + *failure + "), file unchanged");
            failed_conversions_.push_back({DisplayName(nsz), NszFailure::Classify(*failure)});
            continue;
        }
        if (!existing.empty())
            nsp = existing; // Replace in place only after the temporary conversion succeeds.
        // The .nsp runs about 1.4 times its .nsz; 1.5 leaves a margin. The console's internal
        // storage holds only a few GB, so a big game is converted onto a removable drive with
        // room, and when no place has room the conversion is not attempted at all: a doomed
        // attempt ran for minutes on every library open and failed with "disk full" each time.
        const std::uint64_t needed = Common::FS::GetSize(nsz) * 3 / 2;
        const auto free_in = [](const fs::path& folder) { return FreeSpace(folder); };
        const auto free_here = free_in(nsp.parent_path());
        if (!existing.empty() && (!free_here || *free_here < needed)) {
            Diagnostic("UI nsz convert skip: replacement needs temporary space");
            continue;
        }
        if (!free_here || *free_here < needed) {
            bool placed = false;
            for (const fs::path& folder : *external) {
                const auto free_there = free_in(folder);
                if (folder != nsz.parent_path() && free_there && *free_there >= needed) {
                    nsp = folder / nsp.filename();
                    placed = true;
                    Diagnostic("UI nsz convert " + Utf8(nsz.filename().wstring()) +
                               " to removable drive " + Utf8(folder.wstring()));
                    break;
                }
            }
            if (!placed) {
                Diagnostic(fmt::format("UI nsz convert {} skip: needs {} MiB free, has {} MiB",
                                       Utf8(nsz.filename().wstring()), needed >> 20,
                                       free_here.value_or(0) >> 20));
                continue;
            }
        }
        // A conversion that ran out of space is not retried until the target has gained room:
        // the free-space figures overstate what the console lets an app write.
        const fs::path marker = local_state_ / "library" / (nsz.stem().wstring() + L".nospace");
        std::uint64_t free_at_failure = 0;
        if (std::ifstream in{marker}; in >> free_at_failure) {
            const auto free_now = free_in(nsp.parent_path()).value_or(0);
            if (free_now < free_at_failure + (1ull << 30)) {
                Diagnostic(fmt::format("UI nsz convert {} skip: ran out of space before with {} "
                                       "MiB free, {} MiB now",
                                       Utf8(nsz.filename().wstring()), free_at_failure >> 20,
                                       free_now >> 20));
                continue;
            }
            std::error_code ec;
            fs::remove(marker, ec);
        }
        pending.emplace_back(nsz, nsp);
    }
    if (pending.empty()) {
        return;
    }

    FileSys::RealVfsFilesystem vfs;
    int index = 0;
    for (const auto& entry : pending) {
        if (cancel_.load()) {
            break;
        }
        const fs::path& nsz = entry.first;
        const fs::path& nsp = entry.second;
        const std::string file_name = Utf8(nsz.filename().wstring());
        ++index;
        {
            const std::lock_guard<std::mutex> lock(conversion_mutex_);
            conversion_ = Conversion{true, nsz.stem().wstring(), index,
                                     static_cast<int>(pending.size()), 0, 0};
        }
        const auto source_size = Common::FS::GetSize(nsz);
        const auto source_mtime = ModifiedSeconds(nsz);
        const std::string failure = ConvertNsz(
            vfs, nsz, nsp,
            [this](std::uint64_t written, std::uint64_t size) {
                const std::lock_guard<std::mutex> lock(conversion_mutex_);
                conversion_.done = written;
                conversion_.total = size;
            },
            cancel_);
        if (!failure.empty()) {
            Diagnostic("UI nsz convert " + file_name + " fail " + failure);
            if (failure.find("disk full") != std::string::npos) {
                std::ofstream(local_state_ / "library" / (nsz.stem().wstring() + L".nospace"))
                    << FreeSpace(nsp.parent_path()).value_or(0) << '\n';
            } else if (NszFailure::ShouldRemember(failure, cancel_.load())) {
                const auto kind = NszFailure::Classify(failure);
                // Only a damaged file is skipped until it changes: a drive that went away or a
                // folder that cannot be written may clear up, and those failures are quick.
                if (kind == NszFailure::Kind::Damaged)
                    RememberFailure(FailureRecordPath(local_state_, nsz), nsz, source_size,
                                    source_mtime, failure);
                failed_conversions_.push_back({DisplayName(nsz), kind});
            }
            continue;
        }
        std::error_code ec;
        fs::remove(FailureRecordPath(local_state_, nsz), ec);
        RememberConversion(ConversionRecordPath(local_state_, nsz), nsz, nsp);
        Diagnostic("UI nsz convert " + file_name + " ok");
        // The .nsz goes only now that its .nsp is complete and in place.
        if (!Common::FS::RemoveFile(nsz)) {
            Diagnostic("UI nsz convert " + file_name + " ok, but the .nsz could not be removed");
        }
        packages.push_back(nsp);
    }
    const std::lock_guard<std::mutex> lock(conversion_mutex_);
    conversion_ = Conversion{};
}

std::vector<GameEntry> LibraryScan::Scan() {
    PurgeLegacyArt(local_state_);
    const auto started = std::chrono::steady_clock::now();
    const fs::path games_dir = local_state_ / "games";
    const fs::path library_dir = local_state_ / "library";

    auto game_roots = ExternalGameFolders(false);
    game_roots.push_back(games_dir);
    for (const auto& root : game_roots) {
        for (const auto* name : {"Base", "Updates", "DLC"}) {
            if (!Common::FS::CreateDirs(root / name))
                Diagnostic("UI library folders: cannot create " + Utf8((root / name).wstring()));
        }
    }

    // Big games arrive from NXbox Pack as parts; join them before looking for games.
    SplitUpload::JoinSplitUploads(games_dir, [](const std::string& line) { Diagnostic(line); });

    std::vector<fs::path> packages;   // .nsp and .xci
    std::vector<fs::path> compressed; // .nsz
    {
        std::error_code error;
        // Players often keep one folder per game, so subfolders are scanned too.
        fs::recursive_directory_iterator iterator(
            games_dir, fs::directory_options::skip_permission_denied, error);
        if (error) {
            Diagnostic("UI library scan: no local games folder (" + error.message() + ")");
        }
        for (const fs::directory_entry& entry : iterator) {
            std::error_code entry_error;
            if (!entry.is_regular_file(entry_error)) {
                continue;
            }
            const std::wstring extension = Lowercase(entry.path().extension().wstring());
            if (extension == L".nsp" || extension == L".xci") {
                packages.push_back(entry.path());
            } else if (extension == L".nsz") {
                compressed.push_back(entry.path());
            }
        }
    }
    {
        std::ifstream selection(local_state_ / "games_folder.txt");
        std::string path_text;
        std::getline(selection, path_text);
        if (!path_text.empty()) {
            const auto selected_root = fs::u8path(path_text);
            const auto walk = [&](auto&& self, const Files::Location& location,
                                  unsigned depth) -> void {
                try {
                    for (const auto& entry : Files::List(location)) {
                        const Files::Location child{location.root,
                                                    Files::Join(location.relative, entry.name)};
                        if (entry.directory) {
                            if (depth < 8)
                                self(self, child, depth + 1);
                        } else {
                            const auto path = Files::Resolve(child);
                            const auto extension = Lowercase(path.extension().wstring());
                            if (extension == L".nsp" || extension == L".xci")
                                packages.push_back(path);
                            else if (extension == L".nsz")
                                compressed.push_back(path);
                        }
                    }
                } catch (...) {
                    Diagnostic("UI selected games folder unavailable");
                }
            };
            walk(walk, Files::Location{{selected_root, {}}, {}}, 0);
        }
    }
    for (const auto& file : ListExternalGames()) {
        if (Lowercase(file.extension().wstring()) == L".nsz") {
            compressed.push_back(file);
        } else {
            packages.push_back(file);
        }
    }
    std::sort(compressed.begin(), compressed.end());
    if (convert_nsz_) {
        ConvertCompressed(compressed, packages);
    }
    std::sort(packages.begin(), packages.end());
    packages.erase(std::unique(packages.begin(), packages.end()), packages.end());
    total_.store(static_cast<int>(packages.size()));

    // The cache is indexed by package path: the title ID is only known after parsing.
    std::error_code create_error;
    void(fs::create_directories(library_dir, create_error));
    std::map<std::wstring, CacheRecord> cached;
    {
        std::error_code error;
        fs::directory_iterator iterator(library_dir, error);
        if (!error) {
            for (const fs::directory_entry& entry : iterator) {
                CacheRecord record;
                // Title and problem records only; ignore mod and conversion records.
                if (Lowercase(entry.path().extension().wstring()) == L".json" &&
                    (entry.path().stem().wstring().size() == 16 ||
                     entry.path().stem().wstring().starts_with(L"problem-")) &&
                    ReadRecord(entry.path(), record) &&
                    record.size == Common::FS::GetSize(fs::path(record.path)) &&
                    record.mtime == ModifiedSeconds(fs::path(record.path))) {
                    cached.emplace(record.path, record);
                }
            }
        }
    }

    FileSys::RealVfsFilesystem vfs;
    ScanStats stats;
    std::set<std::string> seen;
    std::vector<GameEntry> games;
    for (const fs::path& file : packages) {
        if (cancel_.load()) {
            break;
        }
        std::optional<GameEntry> game = LoadGame(vfs, library_dir, cached, file, stats);
        if (game) {
            // The cache files are named by title ID, so two packages of one title cannot coexist.
            if (game->incomplete || seen.insert(game->title_id).second) {
                games.push_back(std::move(*game));
            } else {
                Diagnostic("UI library skip " + Utf8(file.filename().wstring()) +
                           ": duplicate of title " + game->title_id);
                ++stats.skipped;
            }
        }
        done_.fetch_add(1);
    }

    // External content directories already associate these files with the base at boot.
    // Hide an accessory tile only when every base named by its metadata is present and playable.
    std::vector<std::uint64_t> bases;
    for (const auto& game : games) {
        if (!game.incomplete) {
            if (const auto id = LibraryFiles::ParseTitle(game.title_id))
                bases.push_back(*id);
        }
    }
    std::erase_if(games, [&](const GameEntry& game) {
        return (game.problem == LibraryContent::Problem::UpdateOnly ||
                game.problem == LibraryContent::Problem::DlcOnly) &&
               LibraryContent::Attached(game.content, bases);
    });

    std::sort(games.begin(), games.end(), [](const GameEntry& left, const GameEntry& right) {
        return LibraryContent::Less(left, right, [](const auto& a, const auto& b) {
            return _wcsicmp(a.c_str(), b.c_str());
        });
    });
    keys_missing_.store(stats.key_problems > 0 &&
                        std::none_of(games.begin(), games.end(),
                                     [](const auto& game) { return !game.incomplete; }));
    Diagnostic(fmt::format(
        "UI library scan games={} cached={} parsed={} skipped={} keys={} ms={}", games.size(),
        stats.cached, stats.parsed, stats.skipped, stats.key_problems,
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              started)
            .count()));
    WriteStorageReport(local_state_);
    return games;
}

void LibraryScan::Run() {
    try {
        const ScopedApartment apartment;
        games_ = Scan();
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI library scan failed " + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        Diagnostic(std::string("UI library scan failed ") + error.what());
    }
    finished_.store(true, std::memory_order_release);
}

} // namespace EdenXbox::Ui
