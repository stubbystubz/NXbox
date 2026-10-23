// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

// The Windows headers come first: the Eden headers #undef the A/W macros of the Win32 calls they
// name their methods after.
#include <windows.h>

// wingdi.h defines GetObject as a macro for GetObjectW, which would rename JsonValue::GetObject().
#ifdef GetObject
#undef GetObject
#endif

#include "eden_uwp/ui/mods.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/fs/path_util.h"
#include "common/common_types.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/acc/profile_manager.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/mod_archive.h"
#include "eden_uwp/ui/mods_paths.h"
#include "eden_uwp/ui/widgets.h"
#include "eden_uwp/usb_library.h"

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;
using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;
using winrt::Windows::Foundation::Uri;

constexpr const char* kApiBase = "https://gamebanana.com/apiv11";
constexpr int kPerPage = 15;
constexpr std::uint64_t kMaxArchiveBytes = 6ull << 30; // refuse to unpack more than this
constexpr int kMaxSearchDepth = 6;

// Gives a worker thread a COM apartment of its own for the WinRT calls it makes.
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

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
    return text;
}

// ---- JSON access that tolerates missing and null fields ----

JsonObject ObjectAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::Object) {
            return value.GetObject();
        }
    }
    return JsonObject{};
}

JsonArray ArrayAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::Array) {
            return value.GetArray();
        }
    }
    return JsonArray{};
}

std::string StringAt(const JsonObject& object, const wchar_t* key) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::String) {
            return winrt::to_string(value.GetString());
        }
    }
    return {};
}

double NumberAt(const JsonObject& object, const wchar_t* key, double fallback) {
    if (object.HasKey(key)) {
        const IJsonValue value = object.GetNamedValue(key);
        if (value.ValueType() == JsonValueType::Number) {
            return value.GetNumber();
        }
    }
    return fallback;
}

bool ParseObject(const std::string& text, JsonObject& object) {
    try {
        return JsonObject::TryParse(winrt::to_hstring(text), object);
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

// ---- HTTP ----

winrt::Windows::Web::Http::HttpClient MakeClient() {
    winrt::Windows::Web::Http::HttpClient client;
    // A failure to add the header is not worth failing the request for.
    void(client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"NXbox/1.0"));
    return client;
}

Uri MakeUri(const std::string& url) {
    return Uri{winrt::to_hstring(url)};
}

// GETs a text body. Returns false, with a log line, on a network error or a status other than 200.
bool HttpGetText(const std::string& url, std::string& body) {
    try {
        const auto client = MakeClient();
        const auto response = client.GetAsync(MakeUri(url)).get();
        if (response.StatusCode() != winrt::Windows::Web::Http::HttpStatusCode::Ok) {
            Diagnostic(fmt::format("MODS fetch url={} status={} bytes=0", url,
                                   static_cast<int>(response.StatusCode())));
            return false;
        }
        body = winrt::to_string(response.Content().ReadAsStringAsync().get());
        Diagnostic(fmt::format("MODS fetch url={} status={} bytes={}", url,
                               static_cast<int>(response.StatusCode()), body.size()));
        return true;
    } catch (const winrt::hresult_error& error) {
        Diagnostic(fmt::format("MODS fetch url={} status=error bytes={} hresult={:08X} error={}",
                               url, body.size(), static_cast<std::uint32_t>(error.code().value),
                               winrt::to_string(error.message())));
        return false;
    }
}

// True when `url` answers a HEAD request with a success status (redirects are followed). Used to
// hide index entries whose file no longer exists.
bool HttpUrlLive(const std::string& url) {
    try {
        const auto client = MakeClient();
        winrt::Windows::Web::Http::HttpRequestMessage request{
            winrt::Windows::Web::Http::HttpMethod::Head(), MakeUri(url)};
        const auto response = client.SendRequestAsync(request).get();
        Diagnostic(fmt::format("MODS probe url={} status={} bytes=0", url,
                               static_cast<int>(response.StatusCode())));
        // Only a confirmed missing file should hide a translation. HEAD is often blocked.
        const auto status = static_cast<int>(response.StatusCode());
        return status != 404 && status != 410;
    } catch (const winrt::hresult_error& error) {
        Diagnostic(fmt::format("MODS probe url={} status=error bytes=0 hresult={:08X}", url,
                               static_cast<std::uint32_t>(error.code().value)));
        return true; // A transient probe failure is not proof that the download is gone.
    }
}

bool HttpGetBytes(const std::string& url, std::vector<std::uint8_t>& bytes) {
    try {
        const auto client = MakeClient();
        const auto response = client.GetAsync(MakeUri(url)).get();
        if (response.StatusCode() != winrt::Windows::Web::Http::HttpStatusCode::Ok) {
            return false;
        }
        const auto buffer = response.Content().ReadAsBufferAsync().get();
        if (buffer.Length() == 0 || buffer.Length() > (8u << 20)) {
            return false;
        }
        bytes.assign(buffer.data(), buffer.data() + buffer.Length());
        return true;
    } catch (const winrt::hresult_error& error) {
        Diagnostic("UI mods thumbnail failed " + winrt::to_string(error.message()));
        return false;
    }
}

// Streams `url` into `file`, reporting (bytes so far, total or 0). Returns an empty string on
// success, otherwise why it failed.
std::string HttpDownload(const std::string& url, const fs::path& file, std::uint64_t size_hint,
                         const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                         const std::atomic<bool>& cancel) {
    using namespace winrt::Windows::Storage::Streams;
    using winrt::Windows::Web::Http::HttpCompletionOption;
    try {
        const auto client = MakeClient();
        const auto response =
            client.GetAsync(MakeUri(url), HttpCompletionOption::ResponseHeadersRead).get();
        if (!response.IsSuccessStatusCode()) {
            return fmt::format("the download answered with status {}",
                               static_cast<int>(response.StatusCode()));
        }
        std::uint64_t total = size_hint;
        const auto length = response.Content().Headers().ContentLength();
        if (length) {
            total = length.Value();
        }
        const IInputStream stream = response.Content().ReadAsInputStreamAsync().get();
        const Buffer buffer(64 * 1024);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            return "cannot create the download file";
        }
        std::uint64_t done = 0;
        for (;;) {
            if (cancel.load()) {
                return "cancelled";
            }
            const IBuffer chunk =
                stream.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial).get();
            if (chunk.Length() == 0) {
                break;
            }
            out.write(reinterpret_cast<const char*>(chunk.data()),
                      static_cast<std::streamsize>(chunk.Length()));
            if (!out) {
                return "the disk is full or not writable";
            }
            done += chunk.Length();
            progress(done, total);
        }
        out.close();
        if (done == 0) {
            return "the download was empty";
        }
        progress(done, total > 0 ? total : done);
        return {};
    } catch (const winrt::hresult_error& error) {
        return "network: " + winrt::to_string(error.message());
    }
}

std::string UrlEncode(const std::string& text) {
    return winrt::to_string(Uri::EscapeComponent(winrt::to_hstring(text)));
}

// ---- Plain text from GameBanana's HTML ----

std::string HtmlToText(const std::string& html) {
    std::string out;
    bool in_tag = false;
    for (std::size_t i = 0; i < html.size(); ++i) {
        const char letter = html[i];
        if (in_tag) {
            if (letter == '>') {
                in_tag = false;
            }
            continue;
        }
        if (letter == '<') {
            in_tag = true;
            // Line breaks and block ends become a newline.
            const std::string rest = Lower(html.substr(i, 5));
            if (rest.rfind("<br", 0) == 0 || rest.rfind("</p", 0) == 0 ||
                rest.rfind("</li", 0) == 0 || rest.rfind("</h", 0) == 0) {
                out.push_back('\n');
            }
            continue;
        }
        if (letter == '&') {
            const std::size_t end = html.find(';', i);
            if (end != std::string::npos && end - i <= 8) {
                const std::string entity = html.substr(i, end - i + 1);
                const std::pair<const char*, const char*> known[] = {
                    {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
                    {"&#39;", "'"}, {"&nbsp;", " "}};
                bool replaced = false;
                for (const auto& [from, to] : known) {
                    if (entity == from) {
                        out += to;
                        i = end;
                        replaced = true;
                        break;
                    }
                }
                if (replaced) {
                    continue;
                }
            }
        }
        out.push_back(letter);
    }
    // Collapse the blank runs and trim.
    std::string clean;
    int newlines = 0;
    for (const char letter : out) {
        if (letter == '\r') {
            continue;
        }
        if (letter == '\n') {
            if (++newlines <= 2) {
                clean.push_back('\n');
            }
            continue;
        }
        if ((letter == ' ' || letter == '\t') && (clean.empty() || clean.back() == ' ' ||
                                                  clean.back() == '\n')) {
            continue;
        }
        newlines = 0;
        clean.push_back(letter == '\t' ? ' ' : letter);
    }
    while (!clean.empty() && (clean.back() == '\n' || clean.back() == ' ')) {
        clean.pop_back();
    }
    constexpr std::size_t kMaxText = 700;
    if (clean.size() > kMaxText) {
        std::size_t cut = kMaxText;
        // Do not cut in the middle of a UTF-8 sequence.
        while (cut > 0 && (static_cast<unsigned char>(clean[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        clean.resize(cut);
        clean += "...";
    }
    return clean;
}

// ---- Finding the mod inside an archive ----

bool IsModFolderName(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    return lower == L"romfs" || lower == L"exefs" || lower == L"cheats";
}

// The shallowest folder that has a romfs, exefs or cheats folder in it (so
// atmosphere\contents\<titleid>\ is found too), searched breadth first a few levels deep.
bool FindModRoot(const fs::path& root, fs::path& found) {
    std::deque<std::pair<fs::path, int>> queue;
    queue.emplace_back(root, 0);
    while (!queue.empty()) {
        const auto [directory, depth] = queue.front();
        queue.pop_front();
        std::vector<fs::path> children;
        std::error_code ec;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type_error;
            if (it->is_directory(type_error)) {
                children.push_back(it->path());
            }
        }
        std::sort(children.begin(), children.end());
        for (const fs::path& child : children) {
            if (IsModFolderName(child.filename().wstring())) {
                found = directory;
                return true;
            }
        }
        if (depth < kMaxSearchDepth) {
            for (const fs::path& child : children) {
                queue.emplace_back(child, depth + 1);
            }
        }
    }
    return false;
}

// A folder name Windows accepts, taken from the mod's name.
std::string SanitizeFolderName(const std::string& name, std::int64_t id) {
    std::string folder;
    for (const char letter : name) {
        const bool bad = letter == '<' || letter == '>' || letter == ':' || letter == '"' ||
                         letter == '/' || letter == '\\' || letter == '|' || letter == '?' ||
                         letter == '*' || static_cast<unsigned char>(letter) < 32;
        folder.push_back(bad ? '_' : letter);
    }
    while (!folder.empty() && (folder.back() == ' ' || folder.back() == '.')) {
        folder.pop_back();
    }
    while (!folder.empty() && folder.front() == ' ') {
        folder.erase(folder.begin());
    }
    constexpr std::size_t kMaxName = 48;
    if (folder.size() > kMaxName) {
        std::size_t cut = kMaxName;
        while (cut > 0 && (static_cast<unsigned char>(folder[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        folder.resize(cut);
        while (!folder.empty() && (folder.back() == ' ' || folder.back() == '.')) {
            folder.pop_back();
        }
    }
    if (folder.empty()) {
        folder = "mod-" + std::to_string(id);
    }
    return folder;
}

std::string Extension(const std::string& file_name) {
    const std::size_t dot = file_name.rfind('.');
    return dot == std::string::npos ? std::string() : Lower(file_name.substr(dot));
}

// Copies every .pchtxt and .ips under `root` (a few levels deep) into `exefs`. Two patches with
// the same name in different folders (one per game version, say) keep both, the later one
// prefixed with its folder's name. Returns false when there was none.
bool GatherLoosePatches(const fs::path& root, const fs::path& exefs) {
    std::error_code ec;
    bool any = false;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end{};
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() > kMaxSearchDepth) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code type_error;
        if (!it->is_regular_file(type_error)) {
            continue;
        }
        const std::string extension = Extension(winrt::to_string(it->path().filename().wstring()));
        if (extension != ".pchtxt" && extension != ".ips") {
            continue;
        }
        if (!any) {
            fs::create_directories(exefs, type_error);
            any = true;
        }
        fs::path target = exefs / it->path().filename();
        if (fs::exists(target, type_error)) {
            target = exefs / (it->path().parent_path().filename().wstring() + L"_" +
                              it->path().filename().wstring());
        }
        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, type_error);
        if (type_error) {
            return false;
        }
    }
    return any;
}

// "x.part2.rar" and later: volumes of a split RAR, which libarchive cannot open alone.
bool IsLaterRarVolume(const std::string& file_name) {
    const std::string lower = Lower(file_name);
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".rar") != 0) {
        return false;
    }
    const std::size_t part = lower.rfind(".part");
    if (part == std::string::npos) {
        return false;
    }
    const std::string digits = lower.substr(part + 5, lower.size() - 4 - (part + 5));
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    // Compare as text (no std::stoul, which throws on a 30-digit name): "1", "01", "001" are
    // the first volume.
    const std::size_t first = digits.find_first_not_of('0');
    return first == std::string::npos || digits.substr(first) != "1";
}

// ---- Super Smash Bros. Ultimate (ARCropolis) ----

// Smash Ultimate keeps its data in one data.arc, so its mods are not LayeredFS mods: they are
// ARCropolis folders (sd:/ultimate/mods/<mod>/fighter/...), loaded by the ARCropolis plugin of
// Skyline. Skyline is an exefs mod (main.npdm + subsdk9); the plugin is read from rom:/skyline/plugins,
// so on Eden it goes in a LayeredFS romfs.
constexpr std::string_view kSmashTitle = "01006A800016E000";
constexpr const char* kSkylineUrl =
    "https://github.com/skyline-dev/skyline/releases/download/beta/skyline.zip";
constexpr const char* kArcropolisUrl =
    "https://github.com/Raytwo/ARCropolis/releases/download/v4.1.1/release.zip";
constexpr std::wstring_view kSkylineFolder = L"Skyline";
constexpr std::wstring_view kArcropolisFolder = L"ARCropolis";

std::wstring LowerW(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    return text;
}

// Top-level folders of data.arc that ARCropolis mods replace.
bool IsArcFolderName(const std::wstring& name) {
    static const std::set<std::wstring> names = {
        L"fighter", L"ui",     L"sound",  L"stream;", L"effect", L"stage",  L"camera",
        L"item",    L"assist", L"prebuilt;", L"param", L"boss",  L"enemy",  L"render",
        L"spirits", L"common", L"standard", L"finger", L"miihat", L"pokemon"};
    return names.count(LowerW(name)) != 0;
}

std::vector<fs::path> ChildDirectories(const fs::path& directory) {
    std::vector<fs::path> children;
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_error;
        if (it->is_directory(type_error)) {
            children.push_back(it->path());
        }
    }
    std::sort(children.begin(), children.end());
    return children;
}

// The shallowest folder holding a data.arc folder (fighter, ui, sound...), outside any romfs or
// exefs: "<mod>/fighter", "ultimate/mods/<mod>/fighter" and a bare "fighter" all work.
bool FindArcRoot(const fs::path& root, fs::path& found) {
    std::deque<std::pair<fs::path, int>> queue;
    queue.emplace_back(root, 0);
    while (!queue.empty()) {
        const auto [directory, depth] = queue.front();
        queue.pop_front();
        const std::vector<fs::path> children = ChildDirectories(directory);
        for (const fs::path& child : children) {
            if (IsArcFolderName(child.filename().wstring())) {
                found = directory;
                return true;
            }
        }
        if (depth < kMaxSearchDepth) {
            for (const fs::path& child : children) {
                const std::wstring name = LowerW(child.filename().wstring());
                if (name != L"romfs" && name != L"exefs" && name != L"atmosphere") {
                    queue.emplace_back(child, depth + 1);
                }
            }
        }
    }
    return false;
}

// Copies `from` into `to`, merging with what is there and replacing files of the same name.
bool MergeCopy(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

// Moves `from` to `to` (replacing it), copying when a rename is not possible.
bool PlaceFolder(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) {
        return true;
    }
    return MergeCopy(from, to, error);
}

// ---- Save files ----

std::string Hex16Upper(std::uint64_t value) {
    return fmt::format("{:016X}", value);
}

// A save mod is said to be one by its name or GameBanana category ("100% Save File", "Saves").
bool LooksLikeSaveMod(const std::string& name, const std::string& category) {
    const std::string text = Lower(name + " " + category);
    return text.find("save") != std::string::npos;
}

// Text that comes with a download but is not part of a save.
bool IsSaveNoise(const std::wstring& name) {
    const std::wstring lower = LowerW(name);
    if (lower.starts_with(L"readme") || lower.starts_with(L"read me") ||
        lower.starts_with(L"__macosx") || lower == L".ds_store" || lower == L"desktop.ini") {
        return true;
    }
    for (const wchar_t* extension : {L".txt", L".md", L".url", L".html", L".htm", L".nfo",
                                     L".jpg", L".jpeg", L".png", L".webp", L".gif"}) {
        if (lower.ends_with(extension)) {
            return true;
        }
    }
    return false;
}

std::set<std::wstring> EntryNames(const fs::path& directory) {
    std::set<std::wstring> names;
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (!IsSaveNoise(name)) {
            names.insert(LowerW(name));
        }
    }
    return names;
}

// The folder of the download that is the save. With a save already on the console, it is the
// folder whose entries match the save's own entries best (so "Game/user - date/save_data" and a
// bare "save_data" both land right); otherwise single wrapper folders are stripped.
bool FindSaveRoot(const fs::path& unpacked, const std::set<std::wstring>& current,
                  fs::path& found) {
    std::size_t best = 0;
    std::deque<std::pair<fs::path, int>> queue;
    queue.emplace_back(unpacked, 0);
    while (!queue.empty()) {
        const auto [directory, depth] = queue.front();
        queue.pop_front();
        std::size_t score = 0;
        for (const std::wstring& name : EntryNames(directory)) {
            score += current.count(name);
        }
        if (score > best) {
            best = score;
            found = directory;
        }
        if (depth < kMaxSearchDepth) {
            for (const fs::path& child : ChildDirectories(directory)) {
                queue.emplace_back(child, depth + 1);
            }
        }
    }
    if (best > 0) {
        return true;
    }
    static const std::set<std::wstring> save_folders = {L"save_data", L"savedata", L"save",
                                                        L"saves", L"data"};
    fs::path directory = unpacked;
    for (int depth = 0; depth < kMaxSearchDepth; ++depth) {
        const std::set<std::wstring> names = EntryNames(directory);
        const std::vector<fs::path> children = ChildDirectories(directory);
        if (names.size() != 1 || children.size() != 1 ||
            save_folders.count(LowerW(children.front().filename().wstring())) != 0) {
            break;
        }
        directory = children.front();
    }
    if (EntryNames(directory).empty()) {
        return false;
    }
    found = directory;
    return true;
}

// Copies the save into `destination`, leaving out readmes and pictures at its top level.
bool CopySave(const fs::path& from, const fs::path& destination, std::string& error) {
    std::error_code ec;
    fs::create_directories(destination, ec);
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        if (IsSaveNoise(it->path().filename().wstring())) {
            continue;
        }
        std::error_code copy_error;
        fs::copy(it->path(), destination / it->path().filename(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, copy_error);
        if (copy_error) {
            error = copy_error.message();
            return false;
        }
    }
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

bool DirectoryHasEntries(const fs::path& directory) {
    std::error_code ec;
    return fs::is_directory(directory, ec) && !fs::is_empty(directory, ec);
}

// Eden's save folder of `title` for one user (savedata_factory.cpp GetFullPath): the "future"
// account/<uuid>/<title & ~0xFF>/0 when it exists, else 0000000000000000/<user>/<title>.
fs::path UserSaveDir(const fs::path& nand, std::uint64_t title, const Common::UUID& uuid) {
    const fs::path root = nand / "user" / "save";
    const fs::path future =
        root / "account" / uuid.RawString() / Hex16Upper(title & ~0xFFull) / "0";
    std::error_code ec;
    if (fs::exists(future, ec)) {
        return future;
    }
    const u128 id = uuid.AsU128();
    return root / "0000000000000000" / (Hex16Upper(id[1]) + Hex16Upper(id[0])) / Hex16Upper(title);
}

// The save to replace: the one user that already has a save of the game, else the user Eden
// boots games as. Empty when there are no profiles.
fs::path CurrentSaveDir(std::uint64_t title) {
    const fs::path nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    const Service::Account::ProfileManager manager;
    std::vector<fs::path> with_save;
    for (const auto& uuid : manager.GetAllUsers()) {
        if (!uuid.IsInvalid()) {
            const fs::path directory = UserSaveDir(nand, title, uuid);
            if (DirectoryHasEntries(directory)) {
                with_save.push_back(directory);
            }
        }
    }
    if (with_save.size() == 1) {
        return with_save.front();
    }
    const Common::UUID last = manager.GetLastOpenedUser();
    if (last.IsInvalid()) {
        return {};
    }
    return UserSaveDir(nand, title, last);
}

// Prefix match on the words of a category name, so "ui" does not match "build".
bool CategoryMatches(const std::string& category, const std::vector<const char*>& words) {
    const std::string lower = Lower(category);
    std::size_t start = 0;
    while (start < lower.size()) {
        while (start < lower.size() && !std::isalnum(static_cast<unsigned char>(lower[start]))) {
            ++start;
        }
        std::size_t end = start;
        while (end < lower.size() && std::isalnum(static_cast<unsigned char>(lower[end]))) {
            ++end;
        }
        const std::string token = lower.substr(start, end - start);
        for (const char* word : words) {
            if (!token.empty() && token.rfind(word, 0) == 0) {
                return true;
            }
        }
        start = end;
    }
    return false;
}

// The trademark signs are noise for the search.
std::string StripMarks(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 3, "\xE2\x84\xA2") == 0) {
            i += 3;
            continue;
        }
        if (text.compare(i, 2, "\xC2\xAE") == 0) {
            i += 2;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool EndsWithSwitch(const std::string& name) {
    const std::string lower = Lower(name);
    constexpr std::string_view suffix = "(switch)";
    return lower.size() >= suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Unofficial translations, subtitles and dubs. GameBanana files them as ordinary mods, so they are
// recognised by words in the name or the category.
bool ContainsAny(const std::string& lower, const std::vector<const char*>& words) {
    for (const char* word : words) {
        if (lower.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsTranslationEntry(const ModEntry& entry) {
    static const std::vector<const char*> words = {
        "translat", "tradu",    "dub",     "dublag",  "subtitl",  "legenda", "localiz",
        "locali",   "language", "idioma",  "lingua",  "traduc",   "voice",   "voz",
        "audio pt", "ptbr",     "pt-br",   "pt br",   "portugu",  "brazil",  "brasil",
        "english",  "spanish",  "español", "french",  "français", "german",  "deutsch",
        "italian",  "japanese", "korean",  "chinese", "mandarin"};
    return entry.category == "Translation" ||
           ContainsAny(Lower(entry.name), words) || ContainsAny(Lower(entry.category), words);
}

struct LanguageInfo {
    const char* name;
    std::vector<const char*> words;
};

const std::vector<LanguageInfo>& Languages() {
    static const std::vector<LanguageInfo> languages = {
        {"All languages", {}},
        {"Portuguese", {"portugu", "pt-br", "ptbr", "pt br", "brazil", "brasil", "[pt]", "(pt)"}},
        {"English", {"english", "[en]", "(en)", " eng "}},
        {"Spanish", {"spanish", "espa", "castellano", "[es]", "(es)"}},
        {"French", {"french", "fran", "[fr]", "(fr)"}},
        {"German", {"german", "deutsch", "[de]", "(de)"}},
        {"Italian", {"italian", "italiano", "[it]", "(it)"}},
        {"Japanese", {"japanese", "nihongo", "[jp]", "(jp)", "\xE6\x97\xA5\xE6\x9C\xAC"}},
        {"Korean", {"korean", "[ko]", "(ko)", "\xED\x95\x9C\xEA\xB5\xAD"}},
        {"Chinese", {"chinese", "mandarin", "[zh]", "(zh)", "\xE4\xB8\xAD\xE6\x96\x87"}},
    };
    return languages;
}

bool MatchesLanguage(const ModEntry& entry, int language) {
    const auto& languages = Languages();
    if (language <= 0 || language >= static_cast<int>(languages.size())) {
        return true;
    }
    // Entries that name no language at all stay in, so nothing useful disappears.
    const std::string text = " " + Lower(entry.name) + " " + Lower(entry.category) + " ";
    if (ContainsAny(text, languages[static_cast<std::size_t>(language)].words)) {
        return true;
    }
    for (std::size_t i = 1; i < languages.size(); ++i) {
        if (ContainsAny(text, languages[i].words)) {
            return false;
        }
    }
    return true;
}

struct InstalledRecord {
    std::int64_t id = 0;
    std::string name;
    std::string folder;
    std::string author;
    std::string category;
    std::string thumb_url;
    std::int64_t likes = 0;
    std::string kind = "load"; // "load": eden\load; "arc": sdmc\ultimate\mods; "save"
};

struct Queue {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks;
    bool started = false;
    bool stop = false;
};

} // namespace

struct ModStore::Impl : std::enable_shared_from_this<ModStore::Impl> {
    Impl(fs::path local_state_in, std::string title_id_in, std::string game_name_in)
        : local_state(std::move(local_state_in)), title_id(std::move(title_id_in)),
          game_name(std::move(game_name_in)) {}

    fs::path local_state;
    std::string title_id;
    std::string game_name;
    std::atomic<bool> cancel{false};
    std::atomic<int> language{0};

    Queue api_queue;
    Queue thumb_queue;
    Queue install_queue;

    mutable std::mutex mutex; // guards everything below
    StorePhase phase = StorePhase::Searching;
    std::int64_t game_id = 0;
    int total = -1;
    int page = 0;
    bool more = false;
    bool page_loading = false;
    std::vector<std::shared_ptr<ModEntry>> listing;
    std::map<std::int64_t, std::shared_ptr<ModEntry>> by_id;
    std::vector<InstalledRecord> installed;
    std::set<std::string> disabled;
    std::map<std::int64_t, std::vector<std::uint8_t>> thumbs;
    std::set<std::int64_t> thumb_requested;
    std::map<std::int64_t, ModDetails> details;
    std::set<std::int64_t> details_requested;

    // ---- Threads ----

    void Post(Queue& queue, std::function<void()> task) {
        {
            const std::lock_guard<std::mutex> lock(queue.mutex);
            queue.tasks.push_back(std::move(task));
            if (!queue.started) {
                queue.started = true;
                // The thread owns a reference, so the store can go away while it finishes.
                std::thread([self = shared_from_this(), &queue] { self->Loop(queue); }).detach();
            }
        }
        queue.wake.notify_one();
    }

    void Loop(Queue& queue) {
        try {
            const ScopedApartment apartment;
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue.mutex);
                    queue.wake.wait(lock, [&] { return queue.stop || !queue.tasks.empty(); });
                    if (queue.stop) {
                        return;
                    }
                    task = std::move(queue.tasks.front());
                    queue.tasks.pop_front();
                }
                try {
                    task();
                } catch (const winrt::hresult_error& error) {
                    Diagnostic("UI mods task failed " + winrt::to_string(error.message()));
                } catch (const std::exception& error) {
                    Diagnostic(std::string("UI mods task failed ") + error.what());
                }
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("UI mods worker failed " + winrt::to_string(error.message()));
        }
    }

    void StopQueue(Queue& queue) {
        {
            const std::lock_guard<std::mutex> lock(queue.mutex);
            queue.stop = true;
        }
        queue.wake.notify_all();
    }

    void Stop() {
        cancel.store(true);
        StopQueue(api_queue);
        StopQueue(thumb_queue);
        StopQueue(install_queue);
    }

    // ---- The mods.json file ----

    fs::path RecordFile() const {
        return local_state / "library" / (title_id + ".mods.json");
    }

    void LoadRecordFile() {
        try {
            std::ifstream in(RecordFile(), std::ios::binary);
            if (!in) {
                return;
            }
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            JsonObject root;
            if (!ParseObject(text, root)) {
                return;
            }
            game_id = static_cast<std::int64_t>(NumberAt(root, L"game_id", 0.0));
            for (const IJsonValue& value : ArrayAt(root, L"disabled")) {
                if (value.ValueType() == JsonValueType::String) {
                    disabled.insert(winrt::to_string(value.GetString()));
                }
            }
            for (const IJsonValue& value : ArrayAt(root, L"installed")) {
                if (value.ValueType() != JsonValueType::Object) {
                    continue;
                }
                const JsonObject item = value.GetObject();
                InstalledRecord record;
                record.id = static_cast<std::int64_t>(NumberAt(item, L"id", 0.0));
                record.name = StringAt(item, L"name");
                record.folder = StringAt(item, L"folder");
                record.author = StringAt(item, L"author");
                record.category = StringAt(item, L"category");
                record.thumb_url = StringAt(item, L"thumb");
                record.likes = static_cast<std::int64_t>(NumberAt(item, L"likes", 0.0));
                record.kind = StringAt(item, L"kind");
                if (record.kind.empty()) {
                    record.kind = "load";
                }
                if (record.id != 0 && !record.folder.empty()) {
                    installed.push_back(std::move(record));
                }
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("UI mods record file unreadable " + winrt::to_string(error.message()));
        }
    }

    // Called with `mutex` held.
    void SaveRecordFileLocked() const {
        try {
            JsonObject root;
            root.SetNamedValue(L"game_id",
                               JsonValue::CreateNumberValue(static_cast<double>(game_id)));
            root.SetNamedValue(L"game_name",
                               JsonValue::CreateStringValue(winrt::to_hstring(game_name)));
            JsonArray off;
            for (const std::string& name : disabled) {
                off.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
            }
            root.SetNamedValue(L"disabled", off);
            JsonArray list;
            for (const InstalledRecord& record : installed) {
                JsonObject item;
                item.SetNamedValue(L"id",
                                   JsonValue::CreateNumberValue(static_cast<double>(record.id)));
                item.SetNamedValue(L"name",
                                   JsonValue::CreateStringValue(winrt::to_hstring(record.name)));
                item.SetNamedValue(
                    L"folder", JsonValue::CreateStringValue(winrt::to_hstring(record.folder)));
                item.SetNamedValue(
                    L"author", JsonValue::CreateStringValue(winrt::to_hstring(record.author)));
                item.SetNamedValue(L"category", JsonValue::CreateStringValue(
                                                    winrt::to_hstring(record.category)));
                item.SetNamedValue(
                    L"thumb", JsonValue::CreateStringValue(winrt::to_hstring(record.thumb_url)));
                item.SetNamedValue(
                    L"likes", JsonValue::CreateNumberValue(static_cast<double>(record.likes)));
                item.SetNamedValue(L"kind",
                                   JsonValue::CreateStringValue(winrt::to_hstring(record.kind)));
                list.Append(item);
            }
            root.SetNamedValue(L"installed", list);
            std::error_code ec;
            fs::create_directories(RecordFile().parent_path(), ec);
            std::ofstream out(RecordFile(), std::ios::binary | std::ios::trunc);
            out << winrt::to_string(root.Stringify());
            if (!out) {
                Diagnostic("UI mods record file write failed");
            }
        } catch (const winrt::hresult_error& error) {
            Diagnostic("UI mods record file write failed " + winrt::to_string(error.message()));
        }
    }

    // ---- Entries ----

    // Called with `mutex` held. One shared entry per mod id, whichever list it appears in.
    std::shared_ptr<ModEntry> EntryLocked(std::int64_t id, const std::string& name,
                                          const std::string& author, const std::string& category,
                                          const std::string& thumb_url, std::int64_t likes,
                                          std::int64_t downloads) {
        const auto found = by_id.find(id);
        if (found != by_id.end()) {
            if (downloads >= 0) {
                found->second->downloads.store(downloads);
            }
            return found->second;
        }
        auto entry = std::make_shared<ModEntry>();
        entry->id = id;
        entry->name = name;
        entry->author = author;
        entry->category = category;
        entry->thumb_url = thumb_url;
        entry->likes = likes;
        entry->downloads.store(downloads);
        for (const InstalledRecord& record : installed) {
            if (record.id == id) {
                entry->folder = record.folder;
                entry->state.store(static_cast<int>(ModState::Installed));
            }
        }
        by_id.emplace(id, entry);
        return entry;
    }

    // ---- CNX Updater content ----

    // The translations and game mods CNX Updater lists, from the public nx-links index it reads
    // (https://github.com/gamemoddesignbr/nx-links). Each entry is a zip named after a title id, so
    // the ones for this game are the files whose name starts with it. They get negative ids so they
    // never collide with GameBanana's, and they install through the same unpacking and folder rules.
    static constexpr const char* kCnxIndexUrl =
        "https://raw.githubusercontent.com/gamemoddesignbr/nx-links/master/nx-links-v204.json";
    std::map<std::int64_t, std::string> cnx_urls; // guarded by `mutex`
    std::map<std::int64_t, std::uint64_t> cnx_sizes; // guarded by `mutex`
    int cnx_count = 0;                             // guarded by `mutex`

    static std::uint64_t ParseSizeText(const std::string& text) {
        // "6.39 MB", "622 KB", "1.08 GB"
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        const std::string unit = end ? Lower(std::string(end)) : std::string();
        double scale = 1.0;
        if (unit.find("gb") != std::string::npos) {
            scale = 1024.0 * 1024.0 * 1024.0;
        } else if (unit.find("mb") != std::string::npos) {
            scale = 1024.0 * 1024.0;
        } else if (unit.find("kb") != std::string::npos) {
            scale = 1024.0;
        }
        return value > 0.0 ? static_cast<std::uint64_t>(value * scale) : 0;
    }

    void LoadCnx() {
        std::string body;
        JsonObject root;
        if (!HttpGetText(kCnxIndexUrl, body) || !ParseObject(body, root)) {
            Diagnostic(fmt::format("MODS parse source=CNX valid=0 bytes={}", body.size()));
            return;
        }
        const std::string wanted = Lower(title_id);
        struct Found {
            std::string name;
            std::string link;
            std::string size;
            std::string category;
            std::string author;
        };
        std::vector<Found> found;
        std::size_t parsed = 0;
        for (const auto& [section, category] :
             {std::pair<const wchar_t*, const char*>{L"translations", "Translation"},
              std::pair<const wchar_t*, const char*>{L"modifications", "Mod"}}) {
            const JsonObject group = ObjectAt(root, section);
            for (const auto& pair : group) {
                if (pair.Value().ValueType() != JsonValueType::Object) {
                    continue;
                }
                ++parsed;
                const JsonObject item = pair.Value().GetObject();
                if (item.HasKey(L"enabled") && item.GetNamedBoolean(L"enabled", true) == false) {
                    continue;
                }
                const std::string link = StringAt(item, L"link");
                const auto query = link.find_first_of("?#");
                const auto path = link.substr(0, query);
                const std::size_t slash = path.rfind('/');
                const std::string file =
                    Lower(slash == std::string::npos ? path : path.substr(slash + 1));
                const auto installation_record = ObjectAt(item, L"installed");
                const auto hash_file = Lower(StringAt(installation_record, L"hash_file"));
                const auto uninstall = ObjectAt(installation_record, L"uninstall");
                bool title_matches =
                    file.starts_with(wanted) || hash_file.starts_with(wanted + "/");
                for (const auto& entry : uninstall)
                    if (entry.Value().ValueType() == JsonValueType::String) {
                        const auto folder = Lower(winrt::to_string(entry.Value().GetString()));
                        title_matches |= folder == wanted || folder.starts_with(wanted + "/");
                    }
                if (!title_matches || Extension(file) != ".zip") {
                    continue;
                }
                // https://github.com/<owner>/<repo>/releases/... -> the owner is the credit.
                std::string owner = "CNX Updater";
                const std::string marker = "github.com/";
                const std::size_t at = link.find(marker);
                if (at != std::string::npos) {
                    const std::size_t end = link.find('/', at + marker.size());
                    if (end != std::string::npos) {
                        owner = link.substr(at + marker.size(), end - at - marker.size());
                    }
                }
                found.push_back({winrt::to_string(pair.Key()), link, StringAt(item, L"size"),
                                 category, owner});
            }
        }
        Diagnostic(fmt::format("MODS parse source=CNX valid=1 count={}", parsed));
        Diagnostic(fmt::format("MODS match source=CNX title={} count={}", title_id, found.size()));
        // The index is old: keep only what still downloads.
        const std::size_t listed = found.size();
        found.erase(std::remove_if(found.begin(), found.end(),
                                   [](const Found& item) { return !HttpUrlLive(item.link); }),
                    found.end());
        if (found.empty()) {
            Diagnostic(fmt::format("UI mods CNX has nothing live for {} ({} listed)", title_id,
                                   listed));
            return;
        }
        const std::lock_guard<std::mutex> lock(mutex);
        std::int64_t next = -1;
        std::vector<std::shared_ptr<ModEntry>> entries;
        for (const Found& item : found) {
            const std::int64_t id = next--;
            cnx_urls[id] = item.link;
            cnx_sizes[id] = ParseSizeText(item.size);
            // The index title is the game's; make the row say what it is.
            const std::string label = item.category == "Translation"
                                          ? "Translation: " + item.name
                                          : item.name;
            entries.push_back(EntryLocked(id, label, item.author, item.category, std::string(), 0, -1));
        }
        listing.insert(listing.begin(), entries.begin(), entries.end());
        cnx_count = static_cast<int>(entries.size());
        Diagnostic(fmt::format("UI mods CNX {} entries for {}", cnx_count, title_id));
    }

    // ---- Network ----

    void FindGame() {
        std::int64_t known = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            known = game_id;
            phase = known != 0 ? StorePhase::Loading : StorePhase::Searching;
        }
        if (known == 0) {
            const std::string query = StripMarks(game_name);
            std::string body;
            const std::string url = std::string(kApiBase) +
                                    "/Util/Search/Results?_sModelName=Game&_sOrder=best_match"
                                    "&_sSearchString=" +
                                    UrlEncode(query) + "&_nPage=1";
            if (!HttpGetText(url, body)) {
                SetPhase(StorePhase::Offline);
                return;
            }
            JsonObject root;
            if (!ParseObject(body, root)) {
                Diagnostic(fmt::format("MODS parse source=GameBanana/search valid=0 bytes={}",
                                       body.size()));
                SetPhase(StorePhase::Offline);
                return;
            }
            Diagnostic(fmt::format("MODS parse source=GameBanana/search valid=1 count={}",
                                   ArrayAt(root, L"_aRecords").Size()));
            std::int64_t first = 0;
            std::int64_t switch_id = 0;
            for (const IJsonValue& value : ArrayAt(root, L"_aRecords")) {
                if (value.ValueType() != JsonValueType::Object) {
                    continue;
                }
                const JsonObject record = value.GetObject();
                const auto id = static_cast<std::int64_t>(NumberAt(record, L"_idRow", 0.0));
                std::string name = StringAt(record, L"_sName");
                while (!name.empty() && name.back() == ' ') {
                    name.pop_back();
                }
                if (id == 0) {
                    continue;
                }
                if (first == 0) {
                    first = id;
                }
                if (switch_id == 0 && EndsWithSwitch(name)) {
                    switch_id = id;
                }
            }
            known = switch_id != 0 ? switch_id : first;
            Diagnostic(fmt::format("MODS match source=GameBanana/search title={} game_id={}",
                                   title_id, known));
            if (known == 0) {
                Diagnostic("UI mods game not found on GameBanana: " + game_name);
                SetPhase(StorePhase::NoGame);
                return;
            }
            {
                const std::lock_guard<std::mutex> lock(mutex);
                game_id = known;
                phase = StorePhase::Loading;
                SaveRecordFileLocked();
            }
            Diagnostic(fmt::format("UI mods game {} -> GameBanana id {}{}", title_id, known,
                                   switch_id != 0 ? " (Switch)" : ""));
        }
        LoadPage(1);
    }

    void SetPhase(StorePhase value) {
        const std::lock_guard<std::mutex> lock(mutex);
        phase = (value == StorePhase::NoGame || value == StorePhase::Offline) && cnx_count > 0
                    ? StorePhase::Ready
                    : value;
    }

    void LoadPage(int number) {
        std::int64_t id = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            id = game_id;
            page_loading = true;
        }
        const std::string url =
            fmt::format("{}/Mod/Index?_nPage={}&_nPerpage={}&_aFilters%5BGeneric_Game%5D={}"
                        "&_sSort=Generic_MostDownloaded",
                        kApiBase, number, kPerPage, id);
        std::string body;
        JsonObject root;
        if (!HttpGetText(url, body) || !ParseObject(body, root)) {
            Diagnostic(
                fmt::format("MODS parse source=GameBanana/index valid=0 bytes={}", body.size()));
            const std::lock_guard<std::mutex> lock(mutex);
            page_loading = false;
            if (listing.empty()) {
                phase = StorePhase::Offline;
            }
            return;
        }
        const JsonObject metadata = ObjectAt(root, L"_aMetadata");
        const auto count = static_cast<int>(NumberAt(metadata, L"_nRecordCount", -1.0));
        const JsonArray records = ArrayAt(root, L"_aRecords");
        Diagnostic(
            fmt::format("MODS parse source=GameBanana/index valid=1 count={}", records.Size()));
        int received = 0;
        const std::lock_guard<std::mutex> lock(mutex);
        for (const IJsonValue& value : records) {
            if (value.ValueType() != JsonValueType::Object) {
                continue;
            }
            const JsonObject record = value.GetObject();
            const auto mod_id = static_cast<std::int64_t>(NumberAt(record, L"_idRow", 0.0));
            if (mod_id == 0) {
                continue;
            }
            ++received;
            std::string thumb;
            const JsonObject media = ObjectAt(record, L"_aPreviewMedia");
            const JsonArray images = ArrayAt(media, L"_aImages");
            if (images.Size() > 0 && images.GetAt(0).ValueType() == JsonValueType::Object) {
                const JsonObject image = images.GetAt(0).GetObject();
                std::string file = StringAt(image, L"_sFile220");
                if (file.empty()) {
                    file = StringAt(image, L"_sFile");
                }
                const std::string base = StringAt(image, L"_sBaseUrl");
                if (!file.empty() && !base.empty()) {
                    thumb = base + "/" + file;
                }
            }
            const auto entry =
                EntryLocked(mod_id, StringAt(record, L"_sName"),
                            StringAt(ObjectAt(record, L"_aSubmitter"), L"_sName"),
                            StringAt(ObjectAt(record, L"_aRootCategory"), L"_sName") + " " +
                                StringAt(ObjectAt(record, L"_aCategory"), L"_sName"),
                            thumb, static_cast<std::int64_t>(NumberAt(record, L"_nLikeCount", 0.0)),
                            static_cast<std::int64_t>(NumberAt(record, L"_nDownloadCount", -1.0)));
            const bool listed = std::any_of(listing.begin(), listing.end(),
                                            [&](const auto& other) { return other->id == mod_id; });
            if (!listed) {
                listing.push_back(entry);
            }
        }
        page = number;
        if (count >= 0) {
            total = count;
        }
        more = received >= kPerPage && (total < 0 || static_cast<int>(listing.size()) < total);
        page_loading = false;
        phase = StorePhase::Ready;
        Diagnostic(fmt::format("MODS match source=GameBanana/index title={} game_id={} page={} "
                               "count={} listed={}",
                               title_id, id, number, received, listing.size()));
    }

    // The files of a mod, its description and its download count.
    ModDetails FetchDetails(std::int64_t id) {
        ModDetails result;
        std::string body;
        JsonObject root;
        const std::string url =
            fmt::format("{}/Mod/{}?_csvProperties=_sName,_nDownloadCount,_aFiles,_sText", kApiBase,
                        id);
        if (!HttpGetText(url, body) || !ParseObject(body, root)) {
            result.failed = true;
            return result;
        }
        result.downloads = static_cast<std::int64_t>(NumberAt(root, L"_nDownloadCount", -1.0));
        result.description = HtmlToText(StringAt(root, L"_sText"));
        // _aFiles is an array on most mods; keep the download address next to the name.
        std::vector<std::string> urls;
        for (const IJsonValue& value : ArrayAt(root, L"_aFiles")) {
            if (value.ValueType() != JsonValueType::Object) {
                continue;
            }
            const JsonObject file = value.GetObject();
            const std::string name = StringAt(file, L"_sFile");
            const std::string address = StringAt(file, L"_sDownloadUrl");
            if (name.empty() || address.empty()) {
                continue;
            }
            result.files.emplace_back(name,
                                      static_cast<std::uint64_t>(NumberAt(file, L"_nFilesize", 0.0)));
            urls.push_back(address);
        }
        result.loaded = true;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            download_urls[id] = std::move(urls);
        }
        return result;
    }

    std::map<std::int64_t, std::vector<std::string>> download_urls; // guarded by `mutex`

    // ---- Installing ----

    void InstallMod(const std::shared_ptr<ModEntry>& entry) {
        const auto fail = [&](const std::string& why) {
            Diagnostic(fmt::format("UI mods install {} failed: {}", entry->id, why));
            entry->state.store(static_cast<int>(ModState::Failed));
        };
        entry->percent.store(0);
        entry->state.store(static_cast<int>(ModState::Downloading));

        ModDetails info;
        std::vector<std::string> addresses;
        if (entry->id < 0) {
            const std::lock_guard<std::mutex> lock(mutex);
            const auto link = cnx_urls.find(entry->id);
            if (link != cnx_urls.end()) {
                info.loaded = true;
                info.files.emplace_back("cnx.zip", cnx_sizes[entry->id]);
                addresses.push_back(link->second);
                details[entry->id] = info;
            }
        } else {
            info = FetchDetails(entry->id);
            const std::lock_guard<std::mutex> lock(mutex);
            details[entry->id] = info;
            addresses = download_urls[entry->id];
        }
        if (!info.loaded || info.files.empty() || addresses.size() != info.files.size()) {
            fail(info.failed ? "could not read the mod's files" : "the mod has no files");
            return;
        }
        // .zip, .7z and .rar unpack (mod_archive.cpp). GameBanana lists the main file first, so
        // the first one in a supported format wins; later volumes of a split .rar are skipped,
        // since they cannot be read on their own.
        std::size_t chosen = info.files.size();
        for (std::size_t i = 0; i < info.files.size(); ++i) {
            const std::string& name = info.files[i].first;
            if (IsModArchiveExtension(Extension(name)) && !IsLaterRarVolume(name)) {
                chosen = i;
                break;
            }
        }
        if (chosen == info.files.size()) {
            Diagnostic("UI mods install " + std::to_string(entry->id) +
                       " unsupported format " + info.files.front().first);
            entry->state.store(static_cast<int>(ModState::Unsupported));
            return;
        }

        const fs::path work = local_state / "mods_tmp" / std::to_string(entry->id);
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        const auto cleanup = [&] {
            std::error_code remove_error;
            fs::remove_all(work, remove_error);
        };
        const fs::path archive = work / Widen("mod" + Extension(info.files[chosen].first));
        const std::string download_error = HttpDownload(
            addresses[chosen], archive, info.files[chosen].second,
            [&](std::uint64_t done, std::uint64_t size) {
                if (size > 0) {
                    entry->percent.store(static_cast<int>(std::min<std::uint64_t>(
                        done * 100 / size, 100)));
                }
            },
            cancel);
        if (!download_error.empty()) {
            cleanup();
            fail(download_error);
            return;
        }
        entry->percent.store(100);
        entry->state.store(static_cast<int>(ModState::Installing));

        const fs::path unpacked = work / "x";
        const std::string unpack_error =
            ExtractModArchive(archive, unpacked, cancel, kMaxArchiveBytes);
        if (!unpack_error.empty()) {
            cleanup();
            fail(unpack_error);
            return;
        }
        const std::string folder = SanitizeFolderName(entry->name, entry->id);
        const fs::path load_dir = local_state / "eden" / "load" / Widen(title_id);
        const bool smash = title_id == kSmashTitle;
        std::string kind = "load";
        std::string where;
        std::string error;
        fs::path layered_root;
        fs::path arc_root;
        const bool has_layered = FindModRoot(unpacked, layered_root);
        const bool has_arc = smash && FindArcRoot(unpacked, arc_root);
        if (has_arc) {
            // ARCropolis mod, maybe with a Skyline plugin of its own in a romfs beside it.
            if (!EnsureArcropolis(error)) {
                cleanup();
                fail("could not set up ARCropolis: " + error);
                return;
            }
            const fs::path mods = SdmcDir() / "ultimate" / "mods";
            std::error_code remove_error;
            fs::remove_all(mods / Widen("." + folder), remove_error);
            if (!PlaceFolder(arc_root, mods / Widen(folder), error)) {
                cleanup();
                fail("cannot place the ARCropolis mod: " + error);
                return;
            }
            kind = "arc";
            where = "sdmc\\ultimate\\mods\\" + folder;
            if (has_layered && fs::exists(layered_root, remove_error)) {
                if (!PlaceFolder(layered_root, load_dir / Widen(folder), error)) {
                    cleanup();
                    fail("cannot place the mod: " + error);
                    return;
                }
                where += " + eden\\load\\" + title_id + "\\" + folder;
            }
        } else {
            fs::path root = layered_root;
            bool found = has_layered;
            if (!found) {
                // Many 60 fps / resolution mods are only .pchtxt or .ips patches with no folder
                // around them; Eden loads those from the mod's exefs folder.
                root = work / "loose";
                found = GatherLoosePatches(unpacked, root / "exefs");
            }
            if (found) {
                if (!PlaceFolder(root, load_dir / Widen(folder), error)) {
                    cleanup();
                    fail("cannot place the mod: " + error);
                    return;
                }
                where = "eden\\load\\" + title_id + "\\" + folder;
            } else if (LooksLikeSaveMod(entry->name, entry->category)) {
                if (!InstallSave(unpacked, where, error)) {
                    cleanup();
                    fail("cannot install the save: " + error);
                    return;
                }
                kind = "save";
            } else {
                cleanup();
                fail(smash ? "the archive has no ARCropolis (fighter, ui, sound...), romfs or "
                             "exefs folder"
                           : "the archive has no romfs, exefs or cheats folder");
                return;
            }
        }
        cleanup();
        {
            const std::lock_guard<std::mutex> lock(mutex);
            entry->folder = folder;
            installed.erase(std::remove_if(installed.begin(), installed.end(),
                                           [&](const InstalledRecord& record) {
                                               return record.id == entry->id;
                                           }),
                            installed.end());
            installed.push_back({entry->id, entry->name, folder, entry->author, entry->category,
                                 entry->thumb_url, entry->likes, kind});
            disabled.erase(folder);
            SaveRecordFileLocked();
        }
        entry->state.store(static_cast<int>(ModState::Installed));
        Diagnostic(fmt::format("UI mods install {} ok ({}) -> {}", entry->id, kind, where));
    }

    static fs::path SdmcDir() {
        return Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    }

    // Skyline (exefs) and the ARCropolis plugin (romfs) as Eden mods of Smash Ultimate, and
    // ARCropolis' own resources on the SD card. Downloaded once, the first time a Smash mod
    // needs them.
    bool EnsureArcropolis(std::string& error) {
        const fs::path load_dir = local_state / "eden" / "load" / Widen(std::string(kSmashTitle));
        const fs::path skyline = load_dir / kSkylineFolder;
        const fs::path arcropolis = load_dir / kArcropolisFolder;
        const fs::path sdmc = SdmcDir();
        std::error_code ec;
        const bool have_skyline = fs::exists(skyline / "exefs" / "subsdk9", ec) &&
                                  fs::exists(skyline / "exefs" / "main.npdm", ec);
        const bool have_arcropolis =
            fs::exists(arcropolis / "romfs" / "skyline" / "plugins" / "libarcropolis.nro", ec) &&
            fs::exists(sdmc / "ultimate" / "arcropolis", ec);
        if (have_skyline && have_arcropolis) {
            return true;
        }
        const fs::path work = local_state / "mods_tmp" / "arcropolis";
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        const auto fetch = [&](const char* url, const std::string& name, fs::path& out) {
            const fs::path archive = work / Widen(name + ".zip");
            std::string problem = HttpDownload(
                url, archive, 0, [](std::uint64_t, std::uint64_t) {}, cancel);
            if (problem.empty()) {
                out = work / Widen(name);
                problem = ExtractModArchive(archive, out, cancel, kMaxArchiveBytes);
            }
            if (!problem.empty()) {
                error = name + ": " + problem;
                return false;
            }
            return true;
        };
        bool ok = true;
        fs::path unpacked;
        if (!have_skyline) {
            ok = fetch(kSkylineUrl, "skyline", unpacked);
            if (ok && !fs::exists(unpacked / "exefs" / "subsdk9", ec)) {
                error = "skyline: the download has no exefs\\subsdk9";
                ok = false;
            }
            ok = ok && PlaceFolder(unpacked / "exefs", skyline / "exefs", error);
            if (ok) {
                Diagnostic("UI mods ARCropolis: Skyline installed");
            }
        }
        if (ok && !have_arcropolis) {
            ok = fetch(kArcropolisUrl, "arcropolis", unpacked);
            const fs::path romfs =
                unpacked / "atmosphere" / "contents" / Widen(std::string(kSmashTitle)) / "romfs";
            if (ok && !fs::exists(romfs / "skyline" / "plugins" / "libarcropolis.nro", ec)) {
                error = "arcropolis: the download has no libarcropolis.nro";
                ok = false;
            }
            ok = ok && PlaceFolder(romfs, arcropolis / "romfs", error);
            if (ok && fs::exists(unpacked / "ultimate", ec)) {
                ok = MergeCopy(unpacked / "ultimate", sdmc / "ultimate", error);
            }
            if (ok) {
                fs::create_directories(sdmc / "ultimate" / "mods", ec);
                Diagnostic("UI mods ARCropolis: plugin installed");
            }
        }
        fs::remove_all(work, ec);
        return ok;
    }

    // Replaces the game's save with the one in `unpacked`, after copying the current save to
    // LocalState\mods_save_backup\<TITLEID>\<time>.
    bool InstallSave(const fs::path& unpacked, std::string& where, std::string& error) {
        char* parse_end = nullptr;
        const std::uint64_t title = std::strtoull(title_id.c_str(), &parse_end, 16);
        if (parse_end == title_id.c_str() || title == 0) {
            error = "unknown title id";
            return false;
        }
        fs::path target;
        try {
            target = CurrentSaveDir(title);
        } catch (const std::exception& problem) {
            error = std::string("cannot read the profiles: ") + problem.what();
            return false;
        }
        if (target.empty()) {
            error = "there is no profile yet; boot the game once first";
            return false;
        }
        const std::set<std::wstring> current = EntryNames(target);
        fs::path root;
        if (!FindSaveRoot(unpacked, current, root)) {
            error = "the download has no save files";
            return false;
        }
        std::error_code ec;
        fs::path backup;
        if (DirectoryHasEntries(target)) {
            backup = local_state / "mods_save_backup" / Widen(title_id) /
                     Widen(std::to_string(static_cast<long long>(std::time(nullptr))));
            if (!MergeCopy(target, backup, error)) {
                error = "cannot back up the current save: " + error;
                return false;
            }
            for (fs::directory_iterator it(target, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code remove_error;
                fs::remove_all(it->path(), remove_error);
            }
        }
        if (!CopySave(root, target, error)) {
            if (!backup.empty()) {
                std::string restore_error;
                CopySave(backup, target, restore_error);
            }
            return false;
        }
        where = Common::FS::PathToUTF8String(target) +
                (backup.empty() ? std::string(" (no previous save)")
                                : " (backup in " + Common::FS::PathToUTF8String(backup) + ")");
        return true;
    }
};

ModStore::ModStore(fs::path local_state, std::string title_id, std::string game_name)
    : impl_(std::make_shared<Impl>(std::move(local_state), std::move(title_id),
                                   std::move(game_name))) {
    impl_->LoadRecordFile();
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->api_queue, [impl] { impl->LoadCnx(); });
    impl->Post(impl->api_queue, [impl] { impl->FindGame(); });
}

ModStore::~ModStore() {
    impl_->Stop();
}

StorePhase ModStore::Phase() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->phase;
}

int ModStore::TotalCount() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->total;
}

int ModStore::InstalledCount() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return static_cast<int>(impl_->installed.size());
}

std::vector<std::shared_ptr<ModEntry>> ModStore::List(ModFilter filter) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::shared_ptr<ModEntry>> result;
    if (filter == ModFilter::Installed) {
        for (const InstalledRecord& record : impl_->installed) {
            result.push_back(impl_->EntryLocked(record.id, record.name, record.author,
                                                record.category, record.thumb_url, record.likes,
                                                -1));
        }
        return result;
    }
    static const std::vector<const char*> graphics = {
        "graphic", "texture", "skin", "visual", "shader", "resolution", "model", "effect",
        "light",   "video",   "perform", "reshade"};
    static const std::vector<const char*> interface_words = {"interface", "ui", "hud", "menu",
                                                       "font",      "button", "prompt", "icon"};
    static const std::vector<const char*> gameplay = {"gameplay", "cheat", "difficult", "balanc",
                                                      "mechanic", "tweak", "script"};
    for (const auto& entry : impl_->listing) {
        bool keep = true;
        switch (filter) {
        case ModFilter::Graphics:
            keep = CategoryMatches(entry->category, graphics);
            break;
        case ModFilter::Interface:
            keep = CategoryMatches(entry->category, interface_words);
            break;
        case ModFilter::Gameplay:
            keep = CategoryMatches(entry->category, gameplay);
            break;
        case ModFilter::Translations:
            keep = IsTranslationEntry(*entry) &&
                   MatchesLanguage(*entry, impl_->language.load());
            break;
        default:
            break;
        }
        if (keep) {
            result.push_back(entry);
        }
    }
    return result;
}

int ModStore::LanguageCount() {
    return static_cast<int>(Languages().size());
}

const char* ModStore::LanguageName(int index) {
    const auto& languages = Languages();
    return index >= 0 && index < static_cast<int>(languages.size())
               ? languages[static_cast<std::size_t>(index)].name
               : "";
}

int ModStore::Language() const {
    return impl_->language.load();
}

void ModStore::SetLanguage(int index) {
    impl_->language.store(std::clamp(index, 0, LanguageCount() - 1));
}

bool ModStore::MoreAvailable() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->more;
}

bool ModStore::PageLoading() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->page_loading;
}

void ModStore::LoadMore() {
    int next = 0;
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->more || impl_->page_loading || impl_->phase != StorePhase::Ready) {
            return;
        }
        impl_->page_loading = true;
        next = impl_->page + 1;
    }
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->api_queue, [impl, next] { impl->LoadPage(next); });
}

void ModStore::Retry() {
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->phase != StorePhase::Offline) {
            return;
        }
        impl_->phase = StorePhase::Searching;
    }
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->api_queue, [impl] { impl->FindGame(); });
}

void ModStore::RequestThumb(const std::shared_ptr<ModEntry>& entry) {
    if (entry->thumb_url.empty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->thumb_requested.insert(entry->id).second) {
            return;
        }
    }
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->thumb_queue, [impl, entry] {
        if (impl->cancel.load()) {
            return;
        }
        std::vector<std::uint8_t> bytes;
        if (HttpGetBytes(entry->thumb_url, bytes)) {
            const std::lock_guard<std::mutex> lock(impl->mutex);
            impl->thumbs[entry->id] = std::move(bytes);
        }
    });
}

bool ModStore::TakeThumb(const std::shared_ptr<ModEntry>& entry, std::vector<std::uint8_t>& bytes) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->thumbs.find(entry->id);
    if (found == impl_->thumbs.end()) {
        return false;
    }
    bytes = std::move(found->second);
    impl_->thumbs.erase(found);
    return true;
}

void ModStore::Install(const std::shared_ptr<ModEntry>& entry) {
    const ModState state = entry->State();
    if (state == ModState::Downloading || state == ModState::Installing ||
        state == ModState::Installed || state == ModState::Unsupported) {
        return;
    }
    entry->percent.store(0);
    entry->state.store(static_cast<int>(ModState::Downloading));
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->install_queue, [impl, entry] { impl->InstallMod(entry); });
}

bool ModStore::IsEnabled(const ModEntry& entry) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->disabled.count(entry.folder) == 0;
}

bool ModStore::ToggleEnabled(const std::shared_ptr<ModEntry>& entry) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (entry->State() != ModState::Installed || entry->folder.empty()) {
        return true;
    }
    std::string kind = "load";
    for (const InstalledRecord& record : impl_->installed) {
        if (record.id == entry->id) {
            kind = record.kind;
        }
    }
    if (kind == "save") {
        return true; // a save is not something Eden turns on and off
    }
    bool enabled = true;
    if (impl_->disabled.erase(entry->folder) == 0) {
        impl_->disabled.insert(entry->folder);
        enabled = false;
    }
    if (kind == "arc") {
        // ARCropolis skips mod folders whose name starts with a dot.
        const fs::path mods = Impl::SdmcDir() / "ultimate" / "mods";
        const fs::path on = mods / Widen(entry->folder);
        const fs::path off = mods / Widen("." + entry->folder);
        std::error_code ec;
        fs::rename(enabled ? off : on, enabled ? on : off, ec);
        if (ec) {
            Diagnostic("UI mods ARCropolis rename failed " + ec.message());
        }
    }
    impl_->SaveRecordFileLocked();
    Diagnostic(fmt::format("UI mods {} {}", entry->folder, enabled ? "on" : "off"));
    return enabled;
}

void ModStore::RequestDetails(const std::shared_ptr<ModEntry>& entry) {
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->details_requested.insert(entry->id).second) {
            return;
        }
    }
    if (entry->id < 0) {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        ModDetails result;
        result.loaded = true;
        result.description =
            "From the public CNX Updater index (nx-links). " +
            std::string(entry->category == "Translation"
                            ? "A community translation for this game."
                            : "A community modification for this game.") +
            " Credit goes to its author and to CNX Updater.";
        const auto size = impl_->cnx_sizes.find(entry->id);
        result.files.emplace_back("cnx.zip", size != impl_->cnx_sizes.end() ? size->second : 0);
        impl_->details[entry->id] = std::move(result);
        return;
    }
    const std::shared_ptr<Impl> impl = impl_;
    impl->Post(impl->api_queue, [impl, entry] {
        ModDetails result = impl->FetchDetails(entry->id);
        const std::lock_guard<std::mutex> lock(impl->mutex);
        if (result.downloads >= 0) {
            entry->downloads.store(result.downloads);
        }
        if (result.failed) {
            // Allow another try the next time the sheet opens.
            impl->details_requested.erase(entry->id);
        }
        impl->details[entry->id] = std::move(result);
    });
}

ModDetails ModStore::Details(const std::shared_ptr<ModEntry>& entry) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->details.find(entry->id);
    return found != impl_->details.end() ? found->second : ModDetails{};
}

void ApplyDisabledMods(const fs::path& local_state) {
    // Keep the real filesystem alive: its directories retain a reference to it.
    static FileSys::RealVfsFilesystem mod_filesystem;
    std::string selected;
    std::ifstream game_file(local_state / "game.txt");
    std::getline(game_file, selected);
    while (!selected.empty() && (selected.back() == '\r' || selected.back() == ' ')) {
        selected.pop_back();
    }
    auto game_folders = ExternalGameFolders(false);
    if (!selected.empty() && selected != "none") {
        const fs::path game{Common::FS::ToU8String(selected)};
        game_folders.push_back((game.is_absolute() ? game : local_state / game).parent_path());
    }
    std::vector<FileSys::VirtualDir> roots;
    for (const auto& path : ModLoadPaths(local_state, game_folders)) {
        std::error_code path_error;
        if (!fs::is_directory(path, path_error)) {
            continue;
        }
        if (auto root = mod_filesystem.OpenDirectory(Common::FS::PathToUTF8String(path),
                                                     FileSys::OpenMode::Read)) {
            Diagnostic("MODS load root " + Common::FS::PathToUTF8String(path));
            roots.push_back(std::move(root));
        }
    }
    FileSys::PatchManager::SetAdditionalModLoadRoots(std::move(roots));
    std::error_code ec;
    fs::directory_iterator it(local_state / "library", ec);
    if (ec) {
        return;
    }
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string file = winrt::to_string(it->path().filename().wstring());
        constexpr std::string_view suffix = ".mods.json";
        if (file.size() != 16 + suffix.size() || file.compare(16, suffix.size(), suffix) != 0) {
            continue;
        }
        const std::string title_text = file.substr(0, 16);
        char* parse_end = nullptr;
        const std::uint64_t title = std::strtoull(title_text.c_str(), &parse_end, 16);
        if (parse_end == title_text.c_str() || title == 0) {
            continue;
        }
        try {
            std::ifstream in(it->path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            JsonObject root;
            if (!ParseObject(text, root)) {
                continue;
            }
            std::vector<std::string> names;
            for (const IJsonValue& value : ArrayAt(root, L"disabled")) {
                if (value.ValueType() == JsonValueType::String) {
                    names.push_back(winrt::to_string(value.GetString()));
                }
            }
            if (!names.empty()) {
                Settings::values.disabled_addons[title] = names;
                Diagnostic(fmt::format("UI mods disabled for {}: {}", title_text, names.size()));
            }
        } catch (const winrt::hresult_error&) {
            continue;
        }
    }
}

} // namespace EdenXbox::Ui
