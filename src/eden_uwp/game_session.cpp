// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/nxbox_wait_diag.h"
#include "eden_uwp/firmware.h"
#include "eden_uwp/game_session.h"
#include "eden_uwp/shader_share.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>

#include <fmt/format.h>
#include "common/cpu_features.h"
#include "common/fps_overlay.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/host_memory.h"
#include "common/logging.h"
#include "common/nxbox_jit_cache.h"
#include "common/nxbox_stall.h"
#include "common/nxbox_upload_diag.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/sparse_memory.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/vi/vblank_timing.h"
#include "core/cpu_manager.h"
#include "core/file_sys/nsz.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs_verify.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc/svc_debug_string.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/mii/mii_manager.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "eden_uwp/accounts.h"
#include "eden_uwp/alloc_track.h"
#include "eden_uwp/jit_dualmap_probe.h"
#include "eden_uwp/applet_boot.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/crash_logger.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/diagnostic_report.h"
#include "eden_uwp/error_applet.h"
#include "eden_uwp/game_download.h"
#include "eden_uwp/gamepad.h"
#include "eden_uwp/home_ui_settings.h"
#include "eden_uwp/fps_settings.h"
#include "eden_uwp/http_vfs_file.h"
#include "eden_uwp/mesa_window.h"
#include "eden_uwp/renderer_settings.h"
#include "eden_uwp/save_sync.h"
#include "eden_uwp/setup_ui.h"
#include "eden_uwp/timing_probe.h"
#include "eden_uwp/title_settings.h"
#include "eden_uwp/ui/accounts_screen.h"
#include "eden_uwp/ui/error_overlay.h"
#include "eden_uwp/ui/input.h"
#include "eden_uwp/ui/launch_screen.h"
#include "eden_uwp/ui/library.h"
#include "eden_uwp/ui/library_screen.h"
#include "eden_uwp/ui/mods.h"
#include "eden_uwp/ui/savesync_ui.h"
#include "eden_uwp/usb_library.h"
#include "frontend_common/split_upload.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace EdenXbox {
namespace {
void FlushDeviceApiRing() {
    CollectDeviceApiRing(
        [](const char* name) {
            char value[8192]{};
            const auto size = GetEnvironmentVariableA(name, value, sizeof(value));
            return size && size < sizeof(value) ? std::string(value, size) : std::string{};
        },
        [](const char* name, const std::string& value) {
            DiagnosticReportLines(name, value, Diagnostic);
        });
}

// Host UI requests are exchanged between the dispatcher and the session worker.
struct SessionUi {
    std::shared_ptr<ErrorDisplay> errors = std::make_shared<ErrorDisplay>();
    std::atomic<bool> overlay_open{false};
    std::atomic<bool> guest_home_request{false};
    std::atomic<bool> return_home{false};
};

// The system suspends the app when another app takes the foreground; pause emulation before the
// process is frozen and resume it afterwards.
struct Lifecycle {
    enum Request : int { None, Suspend, Resume };
    std::atomic<int> request{None};
    std::mutex mutex;
    std::optional<winrt::Windows::ApplicationModel::SuspendingDeferral> deferral;
    // Set by the emulation loop once it has paused; the fast-suspend worker waits for it.
    std::atomic<bool> paused_ack{false};
    std::thread worker;

    void CompleteDeferral() {
        std::scoped_lock lock{mutex};
        if (deferral) {
            deferral->Complete();
            deferral.reset();
        }
    }
};

// LocalState\game.txt names the file to boot, relative to LocalState (for example
// "games\\p5r.nsp"), or an absolute removable-drive path. If LocalState\game.url also exists, that URL is downloaded to that file first
// (resuming a partial download), unless the game was just chosen from the library: game.url then
// belongs to whatever game.txt named before, and downloading it over the chosen file would replace
// the player's game. Without game.txt the bundled homebrew boots.
// Converts a remote .nsz straight into an .nsp, reading the source over HTTP: the console's
// storage cannot hold a large game's .nsz and its .nsp at once (Breath of the Wild: 9.6 + 13.5
// GB). The .nsp goes where it fits, LocalState first, then a removable drive's NXbox\games.
// On success `target` names the .nsp; an .nsp already converted in either place is reused.
bool StreamNszToNsp(const std::string& url, std::filesystem::path& target,
                    const std::function<void(const std::filesystem::path&)>& remember_target) {
    namespace fs = std::filesystem;
    fs::path name = target.filename();
    name.replace_extension(L".nsp");
    std::vector<fs::path> places{target.parent_path()};
    for (const fs::path& folder : ExternalGameFolders()) {
        places.push_back(folder);
    }
    for (const fs::path& place : places) {
        if (Common::FS::IsFile(place / name) && Common::FS::GetSize(place / name) > 0) {
            target = place / name;
            remember_target(target);
            Diagnostic("NSZ_STREAM_COMPLETE already converted " + target.string());
            return true;
        }
    }
    const FileSys::VirtualFile source = OpenHttpFile(url);
    if (!source) {
        return false;
    }
    // An .nsp runs about 1.4 times its .nsz for the games measured; 1.5 leaves a margin. A
    // conversion that still runs out of space fails cleanly and removes its partial file.
    const std::uint64_t needed = static_cast<std::uint64_t>(source->GetSize()) * 3 / 2;
    // A partial .nsz from an earlier plain download is given up for the .nsp, so its space
    // counts as free (it held 3.9 GB of the 16.6 GB the removable drive had for BotW).
    std::optional<fs::path> folder;
    for (std::size_t i = 0; i < places.size() && !folder; ++i) {
        const auto free = FreeSpace(places[i]);
        const fs::path stale = places[i] / target.filename();
        const std::uint64_t stale_size =
            Common::FS::IsFile(stale) ? Common::FS::GetSize(stale) : 0;
        if (free && *free + stale_size >= needed) {
            folder = places[i];
            if (stale_size != 0 && Common::FS::RemoveFile(stale)) {
                Diagnostic(fmt::format("NSZ_STREAM removed the partial .nsz ({} MiB)",
                                       stale_size >> 20));
            }
        }
    }
    if (!folder) {
        Diagnostic(fmt::format("NSZ_STREAM_NO_SPACE need={} MiB", needed >> 20));
        return false;
    }
    const fs::path out = *folder / name;
    fs::path partial = out;
    partial += L".partial";
    remember_target(out);
    Diagnostic("NSZ_STREAM_TARGET " + out.string());
    FileSys::RealVfsFilesystem vfs;
    FileSys::VirtualFile output =
        vfs.CreateFile(Common::FS::PathToUTF8String(partial), FileSys::OpenMode::ReadWrite);
    if (!output) {
        Diagnostic("NSZ_STREAM_FAILED cannot create " + partial.string());
        return false;
    }
    auto sample_at = std::chrono::steady_clock::now();
    u64 sample_bytes = 0;
    const auto progress = [&](u64 done, u64 total) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - sample_at).count();
        if (elapsed >= 5.0) {
            Diagnostic(fmt::format("NSZ_STREAM {}/{} MiB {:.1f} MiB/s", done >> 20, total >> 20,
                                   static_cast<double>(done - sample_bytes) / (1 << 20) / elapsed));
            sample_at = now;
            sample_bytes = done;
        }
    };
    std::string error;
    const bool converted = FileSys::ConvertNszToNsp(source, output, progress, &error);
    output.reset();
    if (!converted || !Common::FS::RenameFile(partial, out)) {
        void(Common::FS::RemoveFile(partial));
        Diagnostic("NSZ_STREAM_FAILED " + (error.empty() ? std::string("rename") : error));
        return false;
    }
    target = out;
    Diagnostic(fmt::format("NSZ_STREAM_COMPLETE {} MiB", Common::FS::GetSize(out) >> 20));
    return true;
}

// A removable drive can come back under another letter (the console swapped E: and F: after a
// reset): find the same NXbox\games\... file on the drives present now.
std::optional<std::filesystem::path> FindOnRemovableDrives(const std::filesystem::path& missing) {
    const std::wstring text = missing.wstring();
    std::wstring lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c) { return c < 128 ? static_cast<wchar_t>(std::tolower(c)) : c; });
    constexpr std::wstring_view marker = L"\\nxbox\\games\\";
    const auto at = lower.find(marker);
    if (at == std::wstring::npos) {
        return std::nullopt;
    }
    const std::filesystem::path tail{text.substr(at + marker.size())};
    for (const auto& folder : ExternalGameFolders()) {
        const auto candidate = folder / tail;
        if (candidate != missing && Common::FS::IsFile(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string ResolveGamePath(const std::string& bundled, bool chosen_in_library) {
    namespace fs = std::filesystem;
    const fs::path local(
        winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
    const auto read_line = [](const fs::path& file) {
        std::string line;
        std::ifstream in(file);
        std::getline(in, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        return line;
    };
    if (!fs::exists(local / "game.txt")) {
        return bundled;
    }
    const std::string selected = read_line(local / "game.txt");
    if (selected.empty() || selected == "none") {
        return bundled;
    }
    const fs::path selected_path{Common::FS::ToU8String(selected)};
    fs::path target = selected_path.is_absolute() ? selected_path : local / selected_path;
    const auto remember_target = [&local](const fs::path& path) {
        std::ofstream out(local / "game.txt", std::ios::trunc);
        out << Common::FS::PathToUTF8String(path) << '\n';
        out.flush();
        if (!out) {
            throw std::runtime_error("cannot persist the download target");
        }
    };
    if (chosen_in_library && fs::exists(local / "game.url")) {
        Diagnostic("GAME_URL ignored, the game was chosen in the library");
    }
    const std::string url = !chosen_in_library && fs::exists(local / "game.url")
                                ? read_line(local / "game.url")
                                : std::string{};
    const auto is_nsz = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text.size() > 4 && text.ends_with(".nsz");
    };
    if (!url.empty() && is_nsz(url) && is_nsz(target.string())) {
        if (StreamNszToNsp(url, target, remember_target)) {
            Diagnostic("GAME_TARGET " + target.string());
            return Common::FS::PathToUTF8String(target);
        }
        Diagnostic("NSZ_STREAM_UNAVAILABLE falling back to downloading the .nsz");
    }
    if (!url.empty() && !DownloadFile(url, target, remember_target)) {
        // The download source can go away (the LAN host was reinstalled); a copy that is
        // already on the console is still usable.
        if (!Common::FS::IsFile(target) || Common::FS::GetSize(target) == 0) {
            return bundled;
        }
        Diagnostic("GAME_DOWNLOAD_UNAVAILABLE using the local copy");
    }
    // A big game from NXbox Pack is on the console as parts until its first launch.
    SplitUpload::JoinSplitUploads(target.parent_path(),
                                  [](const std::string& line) { Diagnostic(line); });
    if (!Common::FS::IsFile(target)) {
        if (const auto moved = FindOnRemovableDrives(target)) {
            Diagnostic("GAME_MOVED " + target.string() + " -> " + moved->string());
            target = *moved;
        }
    }
    if (!Common::FS::IsFile(target)) {
        Diagnostic("GAME_MISSING " + target.string());
        return bundled;
    }
    Diagnostic("GAME_TARGET " + target.string());
    return Common::FS::PathToUTF8String(target);
}

// Writes the game the player chose in the library into LocalState\game.txt, in the format
// ResolveGamePath reads: a path relative to LocalState. A path outside LocalState is written as it
// is, which ResolveGamePath resolves just as well.
void RememberChosenGame(const std::filesystem::path& local, const std::string& chosen) {
    namespace fs = std::filesystem;
    const fs::path game(std::wstring_view(winrt::to_hstring(chosen)));
    fs::path selected = game.lexically_relative(local);
    if (selected.empty() || selected.begin()->native() == L"..") {
        selected = game;
    }
    std::ofstream(local / "game.txt", std::ios::trunc) << Common::FS::PathToUTF8String(selected) << '\n';
    Diagnostic("GAME_CHOSEN " + selected.string());
}

// LocalState\eden_settings.txt: "label=value" lines applied over the defaults, by the same labels
// as Eden's qt-config (e.g. accelerate_astc=0), so settings can be tried on the console without a
// rebuild. The save sync needs the active profile (current_user) before the game boots, so this
// runs once ahead of the boot when a sync is due and again in RunGame; applying it twice is
// harmless.
void ApplyEdenSettingsFile(const std::filesystem::path& settings_file) {
    std::ifstream settings_in(settings_file);
    std::string line;
    while (std::getline(settings_in, line)) {
        const auto entry = TitleSettings::ParseLine(line);
        if (!entry) {
            continue;
        }
        const auto& [label, value] = *entry;
        if (label == kOfficialSwitchInterfaceKey || label == kShowFpsKey ||
            label == "button_layout") {
            // Frontend preferences have dedicated readers; layout is global and normalized
            // once per session so per-title files cannot disagree with the physical mapping.
            continue;
        }
        if (label == "resolution_setup" && !TitleSettings::ParseResolution(value)) {
            Diagnostic(fmt::format("SETTING {}={} invalid", label, value));
            continue;
        }
        bool applied = false;
        for (auto& [category, settings] : Settings::values.linkage.by_category) {
            for (Settings::BasicSetting* setting : settings) {
                // Preserve Eden's canonical "backend" label and accept the member name for NXbox.
                if (setting->GetLabel() == label ||
                    (label == "renderer_backend" && setting->GetLabel() == "backend")) {
                    setting->LoadString(value);
                    applied = true;
                }
            }
        }
        Diagnostic(fmt::format("SETTING {}={} {}", label, value, applied ? "applied" : "unknown"));
    }
    Accounts::Apply(std::filesystem::path(winrt::to_string(
        winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())));
}

void ApplyEdenSettingsFile() {
    const std::filesystem::path local_state(std::wstring_view(
        winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
    char show_fps[16]{};
    GetEnvironmentVariableA("NXBOX_SHOW_FPS", show_fps, sizeof(show_fps));
    Common::FpsOverlay::Initialize(ReadShowFps(local_state), show_fps);
    ApplyEdenSettingsFile(
        std::filesystem::path(winrt::to_string(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
        "eden_settings.txt");
}

// NXBOX_READ_CHECK=1: reads 64 random 1 MiB chunks of the game file through IOFile (the
// CreateFileFromAppW path used on removable drives) and through a WinRT StorageFile stream, and
// compares them. A mismatch would mean the emulator was fed wrong bytes from the drive, which
// would explain guest code writing to unmapped memory.
void CheckGameReads(const std::string& path) {
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Streams;
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile);
    if (!file.IsOpen()) {
        Diagnostic("READ_CHECK cannot open " + path);
        return;
    }
    const u64 size = file.GetSize();
    const auto storage = AwaitBounded(
        StorageFile::GetFileFromPathAsync(winrt::to_hstring(path)), std::chrono::seconds(10));
    if (!storage) {
        Diagnostic("READ_CHECK StorageFile unavailable");
        return;
    }
    const auto stream = AwaitBounded(storage->OpenReadAsync(), std::chrono::seconds(10));
    if (!stream) {
        Diagnostic("READ_CHECK stream unavailable");
        return;
    }
    constexpr u32 Chunk = 1u << 20;
    std::vector<u8> a(Chunk);
    u64 seed = 0x9E3779B97F4A7C15ull;
    int mismatches = 0;
    int compared = 0;
    for (int i = 0; i < 64 && size > Chunk; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        // Half the samples beyond 4 GiB when the file is that large.
        const u64 base = (i % 2 == 1 && size > (5ull << 30)) ? (4ull << 30) : 0;
        const u64 offset = (base + seed % (size - base - Chunk)) & ~u64{0xFFF};
        if (!file.Seek(static_cast<s64>(offset)) || file.ReadSpan(std::span<u8>(a)) != Chunk) {
            Diagnostic(fmt::format("READ_CHECK ioread failed at {:#x}", offset));
            ++mismatches;
            continue;
        }
        stream->Seek(offset);
        Buffer buffer{Chunk};
        const auto read =
            AwaitBounded(stream->ReadAsync(buffer, Chunk, InputStreamOptions::None),
                         std::chrono::seconds(30));
        if (!read || read->Length() != Chunk) {
            Diagnostic(fmt::format("READ_CHECK winrt read failed at {:#x}", offset));
            continue;
        }
        ++compared;
        if (std::memcmp(a.data(), read->data(), Chunk) != 0) {
            ++mismatches;
            Diagnostic(fmt::format("READ_CHECK mismatch at {:#x}", offset));
        }
    }
    Diagnostic(fmt::format("READ_CHECK {} size={} compared={} mismatches={}", path, size, compared,
                           mismatches));
}

// NXBOX_VMMAP=1: every 30 s, walks the process address space and logs the committed bytes by
// kind (private, mapped, image) and the twelve largest committed regions, to see what is filling
// the app's memory limit.
void LogVirtualMemoryMap() {
    struct Region {
        std::uintptr_t base;
        std::uint64_t committed;
        DWORD type;
    };
    std::vector<Region> regions;
    std::uint64_t by_type[3]{0, 0, 0};
    std::map<std::uintptr_t, std::uint64_t> by_allocation;
    std::uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION info{};
    Region current{0, 0, 0};
    while (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == sizeof(info)) {
        const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.State == MEM_COMMIT) {
            const int slot = info.Type == MEM_PRIVATE ? 0 : info.Type == MEM_MAPPED ? 1 : 2;
            by_type[slot] += info.RegionSize;
            by_allocation[reinterpret_cast<std::uintptr_t>(info.AllocationBase)] += info.RegionSize;
            if (current.committed != 0 && current.type == info.Type &&
                current.base + current.committed == begin) {
                current.committed += info.RegionSize;
            } else {
                if (current.committed != 0) {
                    regions.push_back(current);
                }
                current = {begin, info.RegionSize, info.Type};
            }
        } else if (current.committed != 0) {
            regions.push_back(current);
            current = {0, 0, 0};
        }
        const auto next = begin + info.RegionSize;
        if (next <= address) {
            break;
        }
        address = next;
    }
    if (current.committed != 0) {
        regions.push_back(current);
    }
    std::sort(regions.begin(), regions.end(),
              [](const Region& a, const Region& b) { return a.committed > b.committed; });
    std::string top;
    for (std::size_t i = 0; i < regions.size() && i < 12; ++i) {
        top += fmt::format(" {:x}:{}MiB:{}", regions[i].base, regions[i].committed >> 20,
                           regions[i].type == MEM_PRIVATE ? "priv"
                                                           : regions[i].type == MEM_MAPPED ? "map"
                                                                                           : "img");
    }
    Diagnostic(fmt::format("VMMAP private={}MiB mapped={}MiB image={}MiB top:{}", by_type[0] >> 20,
                           by_type[1] >> 20, by_type[2] >> 20, top));
    // The same committed bytes grouped by the reservation they belong to (one VirtualAlloc, one
    // mapped view): the guest DRAM backing, each JIT code cache, driver heaps and so on.
    std::vector<std::pair<std::uint64_t, std::uintptr_t>> by_base;
    for (const auto& [base, bytes] : by_allocation) {
        by_base.emplace_back(bytes, base);
    }
    std::sort(by_base.rbegin(), by_base.rend());
    std::string groups;
    for (std::size_t i = 0; i < by_base.size() && i < 24; ++i) {
        groups += fmt::format(" {:x}={}MiB", by_base[i].second, by_base[i].first >> 20);
    }
    Diagnostic(fmt::format("VMGROUPS count={} top:{}", by_base.size(), groups));
}

// Kept free of objects with destructors so MSVC accepts the structured exception handler.
bool CopyStackSlice(std::uintptr_t from, void* to, std::size_t bytes) {
    // Stay inside the committed region: a faulting copy is caught here, but the crash logger's
    // vectored handler still records each first-chance fault, and at 8 ms per sample that flood
    // stalled the game being measured.
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(from), &region, sizeof(region)) == 0 ||
        region.State != MEM_COMMIT)
        return false;
    const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (from + bytes > end)
        return false;
    __try {
        std::memcpy(to, reinterpret_cast<const void*>(from), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// NXBOX_SAMPLER=1: samples the emulated GPU thread every 8 ms (suspend, read the instruction
// pointer and a slice of the stack, resume) and logs the most frequent return addresses as
// module+RVA every 10 s, to be symbolized against the build's PDB. Nothing is allocated while the
// thread is suspended, so the sampler cannot deadlock on a lock the sampled thread holds.
void RunStackSampler(const std::atomic<bool>& closed) {
    struct Range {
        std::uintptr_t begin = 0, end = 0;
        const char* name = "";
    };
    const auto range_of = [](const wchar_t* module, const char* name) {
        Range range;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(module));
        if (base != 0) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            range = {base, base + nt->OptionalHeader.SizeOfImage, name};
        }
        return range;
    };
    const std::array<Range, 2> ranges{range_of(nullptr, "exe"),
                                      range_of(L"libgallium_wgl.dll", "gallium")};
    HANDLE threads[6]{};
    std::uint32_t thread_ids[6]{};
    std::map<std::string, unsigned> leaf, frames, inner, paths;
    unsigned samples = 0;
    auto window_start = std::chrono::steady_clock::now();
    while (!closed.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        for (int which = 0; which < 6; ++which) {
            std::uint32_t id = 0;
            if (which == 0) {
                id = NxboxStall::gpu_thread_id.load();
            } else if (which >= 2) {
                id = NxboxStall::cpu_thread_ids[static_cast<std::size_t>(which - 2)].load();
            } else {
                // The thread running Mesa's d3d12 draws publishes its id on its first draw.
                char value[16]{};
                if (GetEnvironmentVariableA("NXBOX_D3D12_DRIVER_TID", value, sizeof(value)) != 0) {
                    id = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
                }
            }
            if (id == 0) {
                continue;
            }
            HANDLE& thread = threads[which];
            if (thread != nullptr && thread_ids[which] != id) {
                CloseHandle(thread);
                thread = nullptr;
            }
            if (thread == nullptr) {
                thread_ids[which] = id;
                thread = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                    id);
                if (thread == nullptr) {
                    Diagnostic(fmt::format("SAMPLER OpenThread failed {}", GetLastError()));
                    continue;
                }
            }
            std::array<std::uintptr_t, 2048> stack{};
            std::uintptr_t rip = 0;
            bool have = false;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(thread, &context)) {
                    rip = context.Rip;
                    // Near the base of the stack a long copy faults: halve it until one works.
                    bool copied = false;
                    for (std::size_t bytes = sizeof(stack); bytes >= 256 && !copied; bytes /= 2) {
                        stack.fill(0);
                        copied = CopyStackSlice(context.Rsp, stack.data(), bytes);
                    }
                    if (!copied) {
                        stack.fill(0);
                    }
                    have = true;
                }
                ResumeThread(thread);
            }
            if (!have) {
                continue;
            }
            ++samples;
            static constexpr const char* labels[]{"gpu:", "drv:", "cpu0:", "cpu1:", "cpu2:", "cpu3:"};
            const char* label = labels[which];
            const auto describe = [&](std::uintptr_t address) -> std::string {
                for (const Range& range : ranges) {
                    if (address >= range.begin && address < range.end) {
                        return fmt::format("{}{}+{:x}", label, range.name, address - range.begin);
                    }
                }
                return {};
            };
            if (auto text = describe(rip); !text.empty()) {
                ++leaf[text];
            } else {
                // A system library: name it, since the PDBs only cover the app and Mesa.
                std::string module = "other";
                HMODULE owner = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(rip), &owner) &&
                    owner != nullptr) {
                    wchar_t path[260]{};
                    if (GetModuleFileNameW(owner, path, 260) != 0) {
                        std::wstring full(path);
                        const auto slash = full.find_last_of(L'\\');
                        module = winrt::to_string(
                            slash == std::wstring::npos ? full : full.substr(slash + 1));
                    }
                    module += fmt::format("+{:x}", rip - reinterpret_cast<std::uintptr_t>(owner));
                }
                ++leaf[std::string(label) + module];
            }
            std::set<std::string> seen;
            bool innermost = true;
            std::string path;
            unsigned depth = 0;
            for (const auto word : stack) {
                if (auto text = describe(word); !text.empty()) {
                    if (innermost) {
                        ++inner[text];
                        innermost = false;
                    }
                    // The call path: the first ten app/Mesa return addresses above the leaf.
                    if (depth < 10 && seen.find(text) == seen.end()) {
                        path += text.substr(text.find(':') + 1) + "<";
                        ++depth;
                    }
                    seen.insert(std::move(text));
                }
            }
            if (!path.empty()) {
                ++paths[std::string(label) + path];
            }
            for (const auto& text : seen) {
                ++frames[text];
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start >= std::chrono::seconds(10)) {
            const auto top = [](const std::map<std::string, unsigned>& counts, std::size_t limit) {
                std::vector<std::pair<unsigned, std::string>> sorted;
                for (const auto& [text, count] : counts) {
                    sorted.emplace_back(count, text);
                }
                std::sort(sorted.rbegin(), sorted.rend());
                std::string out;
                for (std::size_t i = 0; i < sorted.size() && i < limit; ++i) {
                    out += fmt::format(" {}={}", sorted[i].second, sorted[i].first);
                }
                return out;
            };
            Diagnostic(fmt::format("SAMPLER samples={} leaf:{}", samples, top(leaf, 15)));
            Diagnostic(fmt::format("SAMPLER samples={} inner:{}", samples, top(inner, 20)));
            Diagnostic(fmt::format("SAMPLER samples={} paths:{}", samples, top(paths, 8)));
            Diagnostic(fmt::format("SAMPLER samples={} frames:{}", samples, top(frames, 40)));
            // The busy GPU thread is crowded out of the mixed lists by idle guest cores.
            std::map<std::string, unsigned> gpu_frames;
            for (const auto& [text, count] : frames) {
                if (text.starts_with("gpu:"))
                    gpu_frames.emplace(text, count);
            }
            Diagnostic(fmt::format("SAMPLER samples={} gpu_frames:{}", samples, top(gpu_frames, 80)));
            leaf.clear();
            frames.clear();
            inner.clear();
            paths.clear();
            samples = 0;
            window_start = now;
        }
    }
    for (HANDLE handle : threads) {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }
}

void RunGame(MesaWindow& window, const std::string& bundled_path, const std::atomic<bool>& closed,
             const std::shared_ptr<XboxGamepad>& gamepad, Lifecycle& lifecycle,
             bool chosen_in_library, const std::string& protocol_path, Ui::LaunchScreen* splash,
             Ui::LaunchStatus& launch_status, SessionUi& session_ui) {
    Diagnostic("GAME_BEGIN");
    // The default timer tick is about 15.6 ms: every host sleep and timed wait rounds up to it. Ask
    // for 1 ms through ntdll (it may be refused inside the sandbox; the result is logged).
    {
        using SetResolution = long(__stdcall*)(unsigned long, unsigned char, unsigned long*);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto set = ntdll ? reinterpret_cast<SetResolution>(
                                     GetProcAddress(ntdll, "NtSetTimerResolution"))
                               : nullptr;
        unsigned long actual = 0;
        const long status = set ? set(10000, 1, &actual) : -1;
        Diagnostic(fmt::format("TIMER_RESOLUTION status={:#x} actual={} x100ns", status, actual));
    }
    if (const char* track = std::getenv("NXBOX_ALLOC_TRACK"); track != nullptr && track[0] == '1') {
        NxboxAllocTrackEnable();
    }
    const auto memory_stage = [](const char* stage) {
        Diagnostic(fmt::format("MEM {} commit={} MiB limit={} MiB", stage,
                               winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20,
                               winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20));
    };
    memory_stage("begin");
    // NXBOX_JIT_DUALMAP_PROBE=1: one-shot check of section aliasing for the JIT cache (logged only).
    if (const char* probe = std::getenv("NXBOX_JIT_DUALMAP_PROBE");
        probe != nullptr && probe[0] == '1') {
        NxboxJitDualMapProbe();
    }
    // The app's memory limit is the wall BotW hits while loading the world. Ask the shell for more;
    // it may refuse (the result and the limit afterwards are logged either way).
    for (const std::uint64_t gib : {10ULL, 8ULL, 7ULL, 6ULL}) {
        bool granted = false;
        try {
            granted = winrt::Windows::System::MemoryManager::TrySetAppMemoryUsageLimit(gib << 30);
        } catch (const winrt::hresult_error& error) {
            Diagnostic(fmt::format("MEMORY_LIMIT request {} GiB threw {:#x}", gib,
                                   static_cast<std::uint32_t>(error.code())));
        }
        Diagnostic(fmt::format("MEMORY_LIMIT request {} GiB granted={} limit_now={} MiB", gib,
                               granted,
                               winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20));
        if (granted) {
            break;
        }
    }
    const bool boot_home_menu = protocol_path == SwitchHomeBootTarget;
    const bool boot_mii_edit = protocol_path == SwitchMiiEditBootTarget;
    const bool boot_firmware_applet = boot_home_menu || boot_mii_edit;
    const auto firmware_program_id = boot_mii_edit ? Service::AM::AppletProgramId::MiiEdit
                                                   : Service::AM::AppletProgramId::QLaunch;
    const auto firmware_applet_id =
        boot_mii_edit ? Service::AM::AppletId::MiiEdit : Service::AM::AppletId::QLaunch;
    std::string path =
        boot_firmware_applet
            ? std::string{}
            : (protocol_path.empty() ? ResolveGamePath(bundled_path, chosen_in_library)
                                     : protocol_path);
    SCOPE_EXIT {
        if (splash)
            splash->Release();
    };
    if (splash) {
        splash->InitializeOffscreen();
    }
    if (const char* check = std::getenv("NXBOX_READ_CHECK"); check != nullptr && check[0] == '1') {
        CheckGameReads(path);
    }
    {
        // LocalState\\log_filter.txt (for example "*:Debug") raises the Eden log verbosity for a
        // run.
        std::ifstream in(
            std::filesystem::path(winrt::to_string(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
            "log_filter.txt");
        std::string filter;
        std::string flush;
        if (std::getline(in, filter)) {
            while (!filter.empty() && (filter.back() == '\r' || filter.back() == ' ')) {
                filter.pop_back();
            }
            if (!filter.empty()) {
                Settings::values.log_filter.SetValue(filter);
            }
            // A second line "flush" writes every log line through immediately, so a hung guest
            // still leaves its last activity in the file.
            if (std::getline(in, flush) && flush.rfind("flush", 0) == 0) {
                Settings::values.log_flush_line.SetValue(true);
            }
            // A third line "gldebug" turns on the OpenGL debug callback.
            std::string gl_debug;
            if (std::getline(in, gl_debug) && gl_debug.rfind("gldebug", 0) == 0) {
                Settings::values.renderer_debug.SetValue(true);
            }
        }
    }
    Common::Log::Initialize();
    FileSys::ResetRomfsVerification();
    Settings::values.renderer_backend = window.UsesNativeD3D12()
                                            ? Settings::RendererBackend::D3D12
                                            : Settings::RendererBackend::OpenGL_GLSL;
    // NXBOX_SHADER_BACKEND=spirv feeds Mesa SPIR-V instead of GLSL text, which skips the GLSL front
    // end and the intermediate representation it keeps for every program.
    // It is the default: it costs about 40% less memory per program, which is what keeps Breath of
    // the Wild under the 5 GiB limit. NXBOX_SHADER_BACKEND=glsl restores the GLSL text path.
    {
        const char* backend = std::getenv("NXBOX_SHADER_BACKEND");
        if (!window.UsesNativeD3D12() && (backend == nullptr || _stricmp(backend, "glsl") != 0)) {
            Settings::values.renderer_backend = Settings::RendererBackend::OpenGL_SPIRV;
        }
    }
    // XAudio2 is the audio path available to UWP; NXBOX_AUDIO=null (LocalState\nxbox_env.txt)
    // falls back to silence.
    {
        const char* audio = std::getenv("NXBOX_AUDIO");
        Settings::values.sink_id = audio != nullptr && std::string_view{audio} == "null"
                                       ? Settings::AudioEngine::Null
                                       : Settings::AudioEngine::XAudio2;
    }
    Settings::values.cpuopt_fastmem = false;
    Settings::values.cpuopt_fastmem_exclusives = false;
    Settings::values.use_asynchronous_shaders = false;
    // Mesa d3d12: the GPU ASTC decoder leaves garbage in the lower mip levels (sparkling dots and
    // moire on every distant texture in P5R), and GPU video decoding (d3d11va) freezes the intro
    // movie on a frame. Decode both on the CPU, which renders cleanly on the Xbox.
    // On D3D12 the ASTC decode runs on the texture worker instead of the GPU thread: on the
    // Series X it cut the upload stall in Wonder by 84% and removed the 1 s level-load hitch, with
    // the same fps and image. NXBOX_ASTC_SYNC=1 restores the inline decode.
    Settings::values.accelerate_astc.SetValue(window.UsesNativeD3D12()
                                                  ? Settings::AstcDecodeMode::CpuAsynchronous
                                                  : Settings::AstcDecodeMode::Cpu);
    Settings::values.nvdec_emulation.SetValue(Settings::NvdecEmulation::Cpu);
    Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res1X);
    // The firmware error applet never draws on the Xbox: P5R opens it from the network prompt,
    // the game stops presenting and memory climbs past the 5 GiB limit. The HLE applet logs the
    // error and returns to the game.
    Settings::values.error_applet_mode.SetValue(Settings::AppletMode::HLE);
    ApplyEdenSettingsFile();
    Settings::values.xbox_button_icons.SetValue(
        RendererSettings::ReadXboxButtonIcons(std::filesystem::path(winrt::to_string(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()))));
    // Mods the player turned off in the mod store go into Eden's disabled add-ons list.
    Ui::ApplyDisabledMods(std::filesystem::path(winrt::to_string(
        winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())));
    const auto button_layout =
        RendererSettings::ReadButtonLayout(std::filesystem::path(winrt::to_string(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())));
    Settings::values.button_layout.SetValue(button_layout == ButtonLayout::Position ? "position"
                                                                                    : "name");
    gamepad->SetButtonLayout(button_layout);
    XboxGamepad::Configure(gamepad);
    RegisterUnsupportedEngines();
    SCOPE_EXIT {
        Common::Input::UnregisterInputFactory("nxbox");
        Common::Input::UnregisterOutputFactory("nxbox");
        UnregisterUnsupportedEngines();
    };
    std::atomic<bool> guest_exited{false};
    Core::System system{};
    system.Initialize();
    Service::AM::Frontend::FrontendAppletSet frontend_applets;
    frontend_applets.error = std::make_unique<ErrorApplet>(session_ui.errors);
    system.GetFrontendAppletHolder().SetFrontendAppletSet(std::move(frontend_applets));
    // Discard callbacks while the system and its guest applets are still alive.
    SCOPE_EXIT { session_ui.errors->Clear(); };
    memory_stage("initialized");
    system.ApplySettings();
    system.RegisterExitCallback([&] { guest_exited.store(true, std::memory_order_release); });
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    // Updates and DLC are read in place from the game folders (Eden's external content
    // directories) instead of being installed into the NAND: the console's internal storage has
    // no room for second copies, and without this Breath of the Wild ran as 1.0 with its 1.6
    // update and DLC sitting next to it.
    {
        auto& dirs = Settings::values.external_content_dirs;
        dirs.clear();
        // Updates and DLC load by default. NXBOX_UPDATES=0 in LocalState\nxbox_env.txt turns them
        // off (they used to corrupt games until the shared AES contexts of the patched RomFS
        // were made thread-safe).
        const char* updates = std::getenv("NXBOX_UPDATES");
        if (updates != nullptr && updates[0] == '0') {
            Diagnostic("CONTENT_DIRS off (NXBOX_UPDATES=0)");
        } else {
            const auto local_state = std::filesystem::path(winrt::to_string(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
            dirs.push_back(Common::FS::PathToUTF8String(local_state / "games"));
            for (const auto& folder : ExternalGameFolders()) {
                dirs.push_back(Common::FS::PathToUTF8String(folder));
            }
            for (const auto& dir : dirs) {
                Diagnostic("CONTENT_DIR " + dir);
            }
        }
        // NXBOX_DISABLE_DLC=<title id in hex> (LocalState\nxbox_env.txt) runs a game without its
        // DLC, to tell a DLC problem from an update problem.
        if (const char* no_dlc = std::getenv("NXBOX_DISABLE_DLC"); no_dlc != nullptr) {
            const u64 title = std::strtoull(no_dlc, nullptr, 16);
            Settings::values.disabled_addons[title].push_back("DLC");
            Diagnostic(fmt::format("CONTENT_DLC_DISABLED {:016X}", title));
        }
    }
    // The session outlives all CPU interfaces and is flushed only after they stop.
    NxboxJitCache::session.reset();
    SCOPE_EXIT {
        if (NxboxJitCache::session) {
            NxboxJitCache::session->Flush();
            NxboxJitCache::session.reset();
        }
    };
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    if (boot_firmware_applet) {
        const auto firmware = system.GetFileSystemController().GetSystemNANDContents();
        const auto program = firmware->GetEntryRaw(static_cast<u64>(firmware_program_id),
                                                   FileSys::ContentRecordType::Program);
        if (!program) {
            launch_status.phase.store(Ui::LaunchPhase::Failed);
            Diagnostic(boot_mii_edit
                           ? "MII_EDIT_BOOT failed: program is missing from system NAND"
                           : "HOME_BOOT_FAILED qlaunch program is missing from system NAND");
            return;
        }
        path = program->GetFullPath();
        Diagnostic(boot_mii_edit ? "MII_EDIT_BOOT firmware MiiEdit selected"
                                 : "HOME_BOOT firmware qlaunch selected");
    }
    {
        // Read the actual boot package for library, game.txt and protocol launches. Release this
        // metadata loader before reserving guest memory and constructing the game renderer.
        const auto file = Core::GetGameFileFromPath(system.GetFilesystem(), path);
        const auto loader = Loader::GetLoader(system, file);
        u64 title_id = 0;
        if (loader && loader->ReadProgramId(title_id) == Loader::ResultStatus::Success &&
            title_id) {
            const auto local_state = std::filesystem::path(winrt::to_string(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
            const auto title = fmt::format("{:016X}", title_id);
            ApplyEdenSettingsFile(TitleSettings::File(local_state, title));
            const char* jit_cache = std::getenv("NXBOX_JIT_CACHE");
            if (jit_cache && std::string_view{jit_cache} == "1") {
                try {
                    NxboxJitCache::session = std::make_shared<NxboxJitCache::Store>(
                        local_state / "jitcache" / (title + ".bin"), title_id, Diagnostic);
                } catch (...) {
                    Diagnostic("JIT_CACHE phase=unavailable hits=0 misses=0 bytes=0");
                }
            }
            // The crash log is never rotated, so one file names the game behind each process
            // across a whole testing session; the diagnostic log only keeps the last few games.
            CrashEvent("game title=" + title);
            Diagnostic(fmt::format("RESOLUTION title={} resolution_setup={}", title,
                                   static_cast<int>(Settings::values.resolution_setup.GetValue())));
        } else {
            CrashEvent("game title=unknown");
            Diagnostic(fmt::format("RESOLUTION title=unknown resolution_setup={}",
                                   static_cast<int>(Settings::values.resolution_setup.GetValue())));
        }
    }
    // The window API is selected before guest loading and cannot change while owning CoreWindow.
    // The environment override wins over later settings-file application.
    const char* renderer_override = std::getenv("NXBOX_RENDERER");
    if (renderer_override && _stricmp(renderer_override, "d3d12") == 0) {
        Settings::values.renderer_backend = Settings::RendererBackend::D3D12;
    }
    if (window.UsesNativeD3D12() !=
        (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::D3D12)) {
        throw std::runtime_error("Renderer backend changed after window creation; use global "
                                 "renderer_backend or NXBOX_RENDERER and restart");
    }
    // Apply after global and per-title settings, before renderer/cache construction.
    if (const char* reactive = std::getenv("NXBOX_REACTIVE_FLUSH"); reactive != nullptr) {
        Settings::values.use_reactive_flushing.SetValue(reactive[0] == '1');
    }
    if (NxboxUpload::ForceAstcSync()) {
        Settings::values.accelerate_astc.SetValue(Settings::AstcDecodeMode::Cpu);
    }
    Diagnostic(fmt::format("UPLOAD_POLICY astc={} asynchronous_shaders={} astc_sync={} reactive_flush={}",
                           static_cast<int>(Settings::values.accelerate_astc.GetValue()),
                           Settings::values.use_asynchronous_shaders.GetValue(),
                           NxboxUpload::ForceAstcSync(),
                           Settings::values.use_reactive_flushing.GetValue()));
    Diagnostic(fmt::format(
        "TIMING_POLICY use_multi_core={} use_speed_limit={} speed_limit={} "
        "fast_cpu_time={} sync_core_speed={} use_vsync={} use_video_framerate={} sink_id={} "
        "native_clock={} button_layout={}",
        Settings::values.use_multi_core.GetValue(), Settings::values.use_speed_limit.GetValue(),
        Settings::SpeedLimit(), static_cast<int>(Settings::values.cpu_clock.GetValue()),
        Settings::values.sync_core_speed.GetValue(),
        static_cast<int>(Settings::values.vsync_mode.GetValue()),
        Settings::values.use_video_framerate.GetValue(),
        static_cast<int>(Settings::values.sink_id.GetValue()), Common::g_wall_clock.IsNative(),
        Settings::values.button_layout.GetValue()));
    system.GetUserChannel().clear();
    Diagnostic("GAME_LOADING");
    // Same parameters the desktop frontend uses to boot an application. With a zeroed applet id the
    // applet manager treats the game as a library applet, sends it ChangeIntoForeground instead of
    // FocusStateChanged, and the guest waits for that message forever.
    Service::AM::FrontendAppletParameters parameters{
        .applet_id = Service::AM::AppletId::Application,
        .applet_type = Service::AM::AppletType::Application,
    };
    if (boot_home_menu) {
        parameters.program_id = static_cast<u64>(Service::AM::AppletProgramId::QLaunch);
        parameters.applet_id = Service::AM::AppletId::QLaunch;
        parameters.applet_type = Service::AM::AppletType::SystemApplet;
    }
    if (boot_mii_edit) {
        // Mirror the desktop standalone LibraryAppletParameters.
        parameters = {
            .program_id = static_cast<u64>(firmware_program_id),
            .applet_id = firmware_applet_id,
            .applet_type = Service::AM::AppletType::LibraryApplet,
        };
    }
    system.GetFrontendAppletHolder().SetCurrentAppletId(
        boot_firmware_applet ? firmware_applet_id : Service::AM::AppletId::None);
    memory_stage("before_load");
    if (NxboxJitCache::session) {
        // A loader without an NSO ID must not inherit the previous game's metadata.
        system.SetApplicationProcessBuildID({});
    }
    const auto result = system.Load(window, path, parameters);
    memory_stage("after_load");
    if (result != Core::SystemResultStatus::Success) {
        auto problem = LibraryContent::Problem::None;
        const auto status = static_cast<u32>(result);
        const auto loader_base = static_cast<u32>(Core::SystemResultStatus::ErrorLoader);
        if (status >= loader_base)
            problem = LibraryContent::LoadProblem(
                static_cast<Loader::ResultStatus>(status - loader_base));
        if (result == Core::SystemResultStatus::ErrorSystemFiles ||
            result == Core::SystemResultStatus::ErrorSharedFont)
            problem = LibraryContent::Problem::MissingFirmware;
        // Refine ambiguous no-ExeFS/no-RomFS results using actual CNMT content, never the filename.
        if (!boot_firmware_applet && problem != LibraryContent::Problem::MissingKeys) {
            const auto content_problem = Ui::LibraryFileProblem(std::filesystem::u8path(path));
            if (content_problem != LibraryContent::Problem::None)
                problem = content_problem;
        }
        launch_status.failure.store(LibraryContent::Label(problem));
        launch_status.phase.store(Ui::LaunchPhase::Failed);
        if (boot_mii_edit)
            Diagnostic("MII_EDIT_BOOT failed: load status=" +
                       std::to_string(static_cast<int>(result)));
        Diagnostic("GAME_LOAD_FAILED status=" + std::to_string(static_cast<int>(result)));
        return;
    }
    if (boot_mii_edit)
        Diagnostic("MII_EDIT_BOOT loaded");
    if (NxboxJitCache::session) {
        const auto& build = system.GetApplicationProcessBuildID();
        try {
            NxboxJitCache::Bytes identity;
            if (std::any_of(build.begin(), build.end(), [](auto byte) { return byte != 0; })) {
                NxboxJitCache::Put(identity, system.GetApplicationProcessProgramID());
                identity.insert(identity.end(), build.begin(), build.end());
            }
            NxboxJitCache::session->Open(std::move(identity));
        } catch (...) {
            NxboxJitCache::session->Open({});
        }
    }
    SCOPE_EXIT {
        FlushDeviceApiRing();
        void(system.Pause());
        session_ui.errors->Clear();
        system.ShutdownMainProcess();
        Diagnostic("GAME_STOPPED");
    };
    Kernel::Svc::SetDebugStringObserver([](std::string_view message) {
        if (message.starts_with("NXBOX_PADDLE_"))
            Diagnostic(std::string(message));
    });
    SCOPE_EXIT {
        Kernel::Svc::SetDebugStringObserver(nullptr);
    };
    // Load (and precompile) the per-title shader cache on the renderer's own context, before the
    // GPU thread takes it: a second GL context cannot be made current on the Xbox's Mesa.
    // Without this every session compiled each shader the first time it appeared, stalling for
    // seconds at scene changes. NXBOX_SHADER_CACHE=0 in LocalState\nxbox_env.txt disables it.
    Ui::Pixels launch_pixels;
    auto last_launch_frame = std::chrono::steady_clock::time_point{};
    const auto present_launch = [&](bool force) {
        if (!splash || closed.load())
            return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_launch_frame < std::chrono::milliseconds(250))
            return;
        splash->Draw(launch_status);
        splash->ReadFrame(launch_pixels);
        window.PresentLaunchFrame(launch_pixels);
        last_launch_frame = now;
    };
    const char* shader_cache = std::getenv("NXBOX_SHADER_CACHE");
    if (Settings::values.use_disk_shader_cache.GetValue() &&
        !(shader_cache != nullptr && std::string_view{shader_cache} == "0")) {
        const auto title_id = system.GetApplicationProcessProgramID();
        if (splash) {
            launch_status.phase.store(Ui::LaunchPhase::Shaders);
            Diagnostic("PROTOCOL_PHASE shaders");
            auto& context = system.Renderer().Context();
            context.MakeCurrent();
            SCOPE_EXIT {
                context.DoneCurrent();
            };
            present_launch(true);
        }
        // The shared copy may be what crashed, so a session after a crash skips it too.
        if (BeginShaderCacheLoad(title_id)) {
            Diagnostic("SHADER_CACHE previous load crashed; cache set aside");
        } else {
            DownloadSharedShaderCache(title_id);
        }
        Diagnostic("SHADER_CACHE loading");
        {
            auto& render_context = system.Renderer().Context();
            render_context.MakeCurrent();
            SCOPE_EXIT {
                render_context.DoneCurrent();
            };
            present_launch(true);
            std::stop_source shader_stop;
            const auto local_state = std::filesystem::path(winrt::to_string(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
            const bool shader_preload =
                window.UsesNativeD3D12() && RendererSettings::ReadShaderPreload(local_state);
            launch_status.shader_preload.store(shader_preload);
            size_t last_logged_value = static_cast<size_t>(-1);
            size_t last_logged_total = static_cast<size_t>(-1);
            const auto progress =
                [&](VideoCore::LoadCallbackStage stage, size_t value, size_t total) {
                    if (closed.load())
                        shader_stop.request_stop();
                    if (stage == VideoCore::LoadCallbackStage::Build) {
                        // Legacy loaders end with value=0 and total=N; preload reports settled
                        // jobs.
                        launch_status.built.store(
                            !shader_preload && total > 0 && value == 0 ? total : value);
                        launch_status.total.store(total);
                        present_launch(false);
                    }
                    // Progress goes to the flushed diagnostic file, so a load that hangs or
                    // dies shows how far it got.
                    // Inline builds report a running count with no total; the final call
                    // reports the total with a count of zero.
                    if (stage == VideoCore::LoadCallbackStage::Build && value % 32 == 0 &&
                        (!shader_preload || value != last_logged_value ||
                         total != last_logged_total)) {
                        last_logged_value = value;
                        last_logged_total = total;
                        // The commit shows whether a hang while loading is memory pressure.
                        const auto commit =
                            winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20;
                        Diagnostic(total == 0
                                       ? fmt::format("SHADER_CACHE built {} commit={} MiB", value,
                                                     commit)
                                       : fmt::format("SHADER_CACHE total {}", total));
                    }
                };
            auto* rasterizer = system.Renderer().ReadRasterizer();
            if (shader_preload) {
                rasterizer->LoadDiskResourcesWithPreload(title_id, shader_stop.get_token(),
                                                         progress, [&] {
                                                             if (closed.load()) {
                                                                 shader_stop.request_stop();
                                                                 return true;
                                                             }
                                                             return gamepad->ShaderSkipPressed();
                                                         });
            } else {
                rasterizer->LoadDiskResources(title_id, shader_stop.get_token(), progress);
            }
            launch_status.shader_preload.store(false);
        }
        EndShaderCacheLoad(title_id);
        Diagnostic("SHADER_CACHE ready");
        memory_stage("shader_cache");
    }
    if (closed.load())
        return;
    if (splash) {
        launch_status.phase.store(Ui::LaunchPhase::Starting);
        Diagnostic("PROTOCOL_PHASE starting");
        auto& context = system.Renderer().Context();
        context.MakeCurrent();
        SCOPE_EXIT {
            context.DoneCurrent();
        };
        present_launch(true);
        // The last splash stays presented until the guest presents its first frame.
        splash->Release();
        launch_pixels = {};
        Diagnostic("PROTOCOL_HANDOFF");
    }
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();
    void(system.Run());
    Diagnostic("GAME_RUNNING");
    memory_stage("loaded");
    if (const char* sampler = std::getenv("NXBOX_SAMPLER"); sampler != nullptr && sampler[0] == '1') {
        std::thread([&closed] { RunStackSampler(closed); }).detach();
    }
    // The guest's own HID resource manager applies Settings::values.players lazily, on its first
    // HID service call. Poll() runs immediately on the host thread regardless of guest timing, so
    // without this the controller stays at its construction default (NpadStyleIndex::None,
    // "Controller type 0 is not supported") until the guest happens to touch HID first.
    system.HIDCore().ReloadInputDevices();
    auto* controller = system.HIDCore().GetEmulatedControllerByIndex(0);
    auto measured_at = std::chrono::steady_clock::now();
    auto measured_frames = window.FrameCount();
    auto last_frame_count = measured_frames;
    auto last_frame_at = std::chrono::steady_clock::now();
    double worst_gap_ms = 0.0;
    int hitches = 0;
    bool paused = false;
    int stall_dumps = 0;
    int idle_windows = 0;
    const auto started_at = std::chrono::steady_clock::now();
    TimingProbe timing_probe;
    const auto timing_now = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    const auto reset_timing_probe = [&] {
        timing_probe.Reset(timing_now(), Common::g_wall_clock.GetCNTPCT(),
                           system.CoreTiming().GetClockTicks());
    };
    reset_timing_probe();
    SCOPE_EXIT {
        lifecycle.CompleteDeferral();
    };
    const auto read_report = [](const char* name) -> std::string {
        char report[8192]{};
        const auto length = GetEnvironmentVariableA(name, report, sizeof(report));
        return length && length < sizeof(report) ? std::string(report, length) : std::string{};
    };
    const auto log_report = [](const char* name, const std::string& value) {
        DiagnosticReportLines(name, value, Diagnostic);
    };
    while (!closed.load(std::memory_order_acquire) &&
           !guest_exited.load(std::memory_order_acquire) &&
           !session_ui.return_home.load(std::memory_order_acquire)) {
        // Neither renderer can reuse a removed device. Native command failure is terminal too;
        // leave through the shutdown/error path instead of running a guest behind a frozen image.
        // The Mesa-only device-loss marker may be stale while the native renderer is active.
        char command_failure[2]{};
        const bool commands_stopped =
            GetEnvironmentVariableA("NXBOX_D3D12_COMMAND_FAILURE", command_failure,
                                    sizeof(command_failure)) == 1 &&
            command_failure[0] == '1';
        char device_lost[2]{};
        char native_active[2]{};
        const bool native_renderer =
            GetEnvironmentVariableA("NXBOX_NATIVE_D3D12_ACTIVE", native_active,
                                    sizeof(native_active)) == 1 &&
            native_active[0] == '1';
        if (commands_stopped || (!native_renderer &&
                                 GetEnvironmentVariableA("NXBOX_D3D12_DEVICE_LOST", device_lost,
                                                         sizeof(device_lost)) == 1 &&
                                 device_lost[0] == '1')) {
            for (const char* name :
                 {"NXBOX_D3D12_DRED", "NXBOX_D3D12_DRED2", "NXBOX_D3D12_PSO_FAIL_FIRST",
                  "NXBOX_D3D12_FIRST_FAILURE", "NXBOX_D3D12_BATCH", "NXBOX_D3D12_SYNC_ERROR"}) {
                log_report(name, read_report(name));
            }
            CollectPsoDxil(read_report, log_report);
            CollectBatchJournals(read_report, log_report);
            CollectDeviceApiRing(read_report, log_report);
            throw std::runtime_error(commands_stopped
                                         ? "D3D12 command recording failed; stopping the game"
                                         : "D3D12 device removed; stopping the game");
        }
        const int request = lifecycle.request.exchange(Lifecycle::None, std::memory_order_acq_rel);
        if (request == Lifecycle::Suspend) {
            if (!paused) {
                void(system.Pause());
                paused = true;
                lifecycle.paused_ack.store(true, std::memory_order_release);
                Diagnostic("GAME_SUSPENDED");
                // Leaving through Home suspends the app: share the shaders met this session.
                UploadSharedShaderCache(system.GetApplicationProcessProgramID());
            }
            lifecycle.CompleteDeferral();
        } else if (request == Lifecycle::Resume && paused) {
            gamepad->ResetHomeButton();
            void(system.Run());
            paused = false;
            measured_at = std::chrono::steady_clock::now();
            measured_frames = window.FrameCount();
            reset_timing_probe();
            Diagnostic("GAME_RESUMED");
        }
        if (paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        timing_probe.Poll(timing_now());
        session_ui.errors->CompleteDismissed();
        gamepad->Poll(*controller, system.HIDCore().GetSupportedStyleTag(),
                      session_ui.overlay_open.load(std::memory_order_acquire) ||
                          session_ui.errors->Pending());
        switch (gamepad->HomeCombo()) {
        case HomeComboAction::Prompt:
            Diagnostic("HOME_COMBO action=Prompt");
            session_ui.overlay_open.store(true, std::memory_order_release);
            break;
        case HomeComboAction::GoHome:
            Diagnostic("HOME_COMBO action=GoHome");
            session_ui.return_home.store(true, std::memory_order_release);
            break;
        case HomeComboAction::None:
            break;
        }
        // HOME reaches the guest only when explicitly selected in the emulator overlay.
        if (session_ui.guest_home_request.exchange(false, std::memory_order_acq_rel)) {
            system.GetAppletManager().HomeButtonPressed();
        }
        const auto now = std::chrono::steady_clock::now();
        // Stutter: the longest gap between two presented frames in each window.
        if (const auto count = window.FrameCount(); count != last_frame_count) {
            const double gap =
                std::chrono::duration<double, std::milli>(now - last_frame_at).count();
            if (last_frame_count != 0) {
                worst_gap_ms = std::max(worst_gap_ms, gap);
                hitches += gap > 100.0 ? 1 : 0;
            }
            last_frame_count = count;
            last_frame_at = now;
        }
        static const bool vmmap = [] {
            const char* value = std::getenv("NXBOX_VMMAP");
            return value != nullptr && value[0] == '1';
        }();
        static auto last_vmmap = std::chrono::steady_clock::now();
        if (vmmap && now - last_vmmap >= std::chrono::seconds(30)) {
            last_vmmap = now;
            LogVirtualMemoryMap();
            if (const char* track = std::getenv("NXBOX_ALLOC_TRACK"); track != nullptr && track[0] == '1') {
                Diagnostic("ALLOCS " + NxboxAllocTrackReport());
            }
            Diagnostic(fmt::format("SPARSE committed={}MiB guest_backing_committed={}MiB",
                                   Common::SparseMemory::CommittedBytes() >> 20,
                                   Common::HostMemoryCommittedBytes() >> 20));
            if (auto* process = system.ApplicationProcess()) {
                // What the guest believes it has committed, to tell it apart from host-side use.
                Diagnostic(fmt::format(
                    "GUEST_MEMORY used={}MiB used_non_system={}MiB total={}MiB",
                    process->GetUsedUserPhysicalMemorySize(system.Kernel()) >> 20,
                    process->GetUsedNonSystemUserPhysicalMemorySize(system.Kernel()) >> 20,
                    process->GetTotalUserPhysicalMemorySize(system.Kernel()) >> 20));
            }
        }
        const auto elapsed = std::chrono::duration<double>(now - measured_at).count();
        if (elapsed >= 5.0) {
            const auto timing_sample =
                timing_probe.Read(timing_now(), Common::g_wall_clock.GetCNTPCT(),
                                  system.CoreTiming().GetClockTicks());
            Diagnostic(fmt::format("GAME_TIMING wall_ms={:.1f} clock_mhz={:.4f} guest_mhz={:.4f} "
                                   "polls={} max_poll_gap_ms={:.1f} overlay_open={}",
                                   timing_sample.wall_ms, timing_sample.clock_mhz,
                                   timing_sample.guest_mhz, timing_sample.polls,
                                   timing_sample.max_poll_gap_ms,
                                   session_ui.overlay_open.load(std::memory_order_acquire)));
            reset_timing_probe();
            if (Service::VI::VblankProfileEnabled()) {
                const auto vblank = Service::VI::vblank_timing.ReadAndReset();
                Diagnostic(fmt::format(
                    "VI_TIMING independent={} ticks={} signals={} compositions={} "
                    "late_max_ms={:.3f} signal_gap_max_ms={:.3f} compose_ms={:.3f} "
                    "compose_max_ms={:.3f} swap_negative={} swap_0={} swap_1={} swap_2={} "
                    "swap_3={} swap_4={} swap_extended={}",
                    Service::VI::IndependentVsyncEnabled() && system.IsMulticore(), vblank.ticks,
                    vblank.signals, vblank.compositions, vblank.late_ns_max / 1e6,
                    vblank.signal_gap_ns_max / 1e6, vblank.compose_ns / 1e6,
                    vblank.compose_ns_max / 1e6, vblank.intervals[0], vblank.intervals[1],
                    vblank.intervals[2], vblank.intervals[3], vblank.intervals[4],
                    vblank.intervals[5], vblank.intervals[6]));
            }
            const auto frames = window.FrameCount();
            if (NxboxWait::Enabled()) {
                // Only atomics and recorder-owned copies: never enqueue a GPU task here.
                std::string progress = "WAIT_PROGRESS";
                for (size_t i = 0; i < NxboxWait::counters.size(); ++i) {
                    progress += fmt::format(" {}={}", NxboxWait::CounterNames[i],
                                            NxboxWait::counters[i].load(std::memory_order_relaxed));
                }
                Diagnostic(progress);
                const auto pending = NxboxWait::Pending().Read();
                const auto pending_ms = NxboxWait::NowMs();
                for (size_t kind = 0; kind < NxboxWait::PendingNames.size(); ++kind) {
                    u64 count{}, oldest_ms{};
                    for (const auto& entry : pending.entries) {
                        if (static_cast<size_t>(entry.kind) != kind)
                            continue;
                        ++count;
                        oldest_ms = std::max(oldest_ms, pending_ms - entry.began_ms);
                        if (frames == measured_frames) {
                            Diagnostic(fmt::format(
                                "WAIT_PENDING kind={} token={} key={:#x} tick={} age_ms={}",
                                NxboxWait::PendingNames[kind], entry.token, entry.key, entry.tick,
                                pending_ms - entry.began_ms));
                        }
                    }
                    Diagnostic(fmt::format("WAIT_PENDING_SUMMARY sampled_ms={} kind={} count={} "
                                           "oldest_age_ms={} dropped={}",
                                           pending_ms, NxboxWait::PendingNames[kind], count,
                                           oldest_ms, pending.dropped));
                }
                if (frames == measured_frames) {
                    const auto snapshot = NxboxWait::recorder.Read();
                    const auto sampled_ms = NxboxWait::NowMs();
                    Diagnostic(fmt::format("WAIT_SNAPSHOT sampled_ms={} entries={} dropped={}",
                                           sampled_ms, snapshot.entries.size(), snapshot.dropped));
                    for (const auto& entry : snapshot.entries) {
                        Diagnostic(fmt::format(
                            "WAIT_ENTRY kind={} key={:#x} token={} label={} active={} "
                            "age_ms={} duration_ms={} pc={:#x} lr={:#x} result={:#x} "
                            "args={:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                            NxboxWait::KindNames[static_cast<size_t>(entry.kind)], entry.key,
                            entry.token, entry.label.data(), !entry.ended_ms,
                            sampled_ms - entry.began_ms,
                            (entry.ended_ms ? entry.ended_ms : sampled_ms) - entry.began_ms,
                            entry.pc, entry.lr, entry.result, entry.args[0], entry.args[1],
                            entry.args[2], entry.args[3], entry.args[4], entry.args[5],
                            entry.args[6], entry.args[7]));
                    }
                }
            }
            const auto stats = system.GetAndResetPerfStats();
            Diagnostic(fmt::format("GAME_PRESENT frames={} seconds={:.3f} fps={:.2f} "
                                   "game_fps={:.2f} system_fps={:.2f} "
                                   "frametime_ms={:.2f} speed={:.1f}% memory={} memory_limit={} "
                                   "worst_gap_ms={:.0f} hitches={} tick_ms={}",
                                   frames - measured_frames, elapsed,
                                   (frames - measured_frames) / elapsed, stats.average_game_fps,
                                   stats.system_fps, stats.frametime * 1000.0,
                                   stats.emulation_speed * 100.0,
                                   winrt::Windows::System::MemoryManager::AppMemoryUsage(),
                                   winrt::Windows::System::MemoryManager::AppMemoryUsageLimit(),
                                   worst_gap_ms, hitches,
                                   GetTickCount64()));
            // The patched Mesa publishes its reports in environment variables (pipeline states,
            // root signatures, batch failures, device removal); log each one when it changes.
            {
                const auto collect = [&](const char* name, std::string& previous) {
                    char value[8192] = "";
                    const auto length = GetEnvironmentVariableA(name, value, sizeof(value));
                    if (length && length < sizeof(value) && previous != value) {
                        previous = value;
                        log_report(name, value);
                    }
                };
                static std::array<std::string, 11> last;
                static constexpr std::array<const char*, 11> names{
                    "NXBOX_D3D12_PSO",           "NXBOX_D3D12_PSO_FAIL",
                    "NXBOX_D3D12_ROOTSIG",       "NXBOX_D3D12_REMOVED",
                    "NXBOX_D3D12_BATCH",         "NXBOX_D3D12_RESET",
                    "NXBOX_D3D12_FIRST_FAILURE", "NXBOX_D3D12_PSO_FAIL2",
                    "NXBOX_D3D12_PSO_FALLBACK",  "NXBOX_D3D12_DRED",
                    "NXBOX_D3D12_DRED2"};
                for (std::size_t i = 0; i < names.size(); ++i) {
                    collect(names[i], last[i]);
                }
                // The journal of a list whose Close failed: one variable per 4 KB part.
                for (int part = 0; part < 64; ++part) {
                    char value[8192]{};
                    const auto name = fmt::format("NXBOX_D3D12_BATCH_JOURNAL_{}", part);
                    const auto length = GetEnvironmentVariableA(name.c_str(), value, sizeof(value));
                    if (length == 0 || length >= sizeof(value)) {
                        break;
                    }
                    static std::set<std::string> journal_logged;
                    if (journal_logged.insert(name + value).second) {
                        log_report(name.c_str(), value);
                    }
                }
                static constexpr const char* extended_names[]{
                    "NXBOX_D3D12_PSO_FAIL_FIRST",    "NXBOX_D3D12_PSO_FIX",
                    "NXBOX_D3D12_PSO_DXIL",          "NXBOX_D3D12_SYNC_ERROR",
                    "NXBOX_D3D12_HEAP_POLICY",       "NXBOX_D3D12_DEBUG_STATUS",
                    "NXBOX_D3D12_DEBUG_UNAVAILABLE", "NXBOX_D3D12_LIST_ERROR", "NXBOX_D3D12_CALLS", "NXBOX_D3D12_GPUPROF", "NXBOX_D3D12_FIRST_BAD_BATCH", "NXBOX_D3D12_DRAWTIME",
                    "NXBOX_D3D12_GBV_STATUS", "NXBOX_D3D12_BATCH_TIME",
                    "NXBOX_D3D12_VIDMEM", "NXBOX_D3D12_EVICT",
                    "NXBOX_D3D12_VIEW_CAST", "NXBOX_D3D12_VIEW_CAST_PAIRS",
                    "NXBOX_D3D12_PERF_WAITS", "NXBOX_DXIL_UNSUPPORTED"};
                static std::array<std::string, std::size(extended_names)> extended_last;
                for (std::size_t i = 0; i < std::size(extended_names); ++i) {
                    collect(extended_names[i], extended_last[i]);
                }
                static std::string last_upload_ready;
                collect("NXBOX_GL_UPLOAD_READY", last_upload_ready);
                static std::string last_mesa_upload_ready;
                collect("NXBOX_MESA_UPLOAD_READY", last_mesa_upload_ready);
                static std::string last_srv_swizzle;
                static std::string last_geometry;
                collect("NXBOX_D3D12_GEOMETRY", last_geometry);
                static std::string last_uniform_geometry;
                collect("NXBOX_GL_UBO_GEOMETRY", last_uniform_geometry);
                static std::string first_uniform_geometry;
                collect("NXBOX_GL_UBO_FIRST", first_uniform_geometry);
                collect("NXBOX_D3D12_SRV_SWIZZLE", last_srv_swizzle);
                static std::string last_swizzle_slots;
                collect("NXBOX_D3D12_SWIZZLE_SLOTS", last_swizzle_slots);
                static std::string last_api_ring;
                const auto api_manifest =
                    read_report("NXBOX_D3D12_API_RING") + read_report("NXBOX_D3D12_LIST_CAPTURES");
                if (api_manifest != last_api_ring) {
                    CollectDeviceApiRing(read_report, log_report);
                    last_api_ring = api_manifest;
                }
                static std::map<std::string, std::string> last_journal;
                CollectBatchJournals(read_report, [&](const char* name, const std::string& value) {
                    auto& previous = last_journal[name];
                    if (previous != value) {
                        previous = value;
                        log_report(name, value);
                    }
                });
                static std::map<std::string, std::string> last_dxil;
                CollectPsoDxil(read_report, [&](const char* name, const std::string& value) {
                    auto& previous = last_dxil[name];
                    if (previous != value) {
                        previous = value;
                        log_report(name, value);
                    }
                });
            }
#if NXBOX_STALL_PROFILE
            // Host work in the window, as total/longest milliseconds and call count, so each
            // hitch can be traced to shader builds, texture uploads, decoding or cache eviction.
            std::string stall_line = "GAME_STALL";
            constexpr std::array<std::pair<NxboxStall::Kind, const char*>, 29> stall_kinds{{
                {NxboxStall::Kind::Shader, "shader"},
                {NxboxStall::Kind::Upload, "upload"},
                {NxboxStall::Kind::Convert, "convert"},
                {NxboxStall::Kind::GarbageCollect, "gc"},
                {NxboxStall::Kind::Video, "video"},
                {NxboxStall::Kind::Jit, "jit"},
                {NxboxStall::Kind::JitFlush, "jitflush"},
                {NxboxStall::Kind::JitProtect, "jitprotect"},
                {NxboxStall::Kind::JitTranslate, "jittranslate"},
                {NxboxStall::Kind::JitOptimize, "jitoptimize"},
                {NxboxStall::Kind::JitEmit, "jitemit"},
                {NxboxStall::Kind::JitEmitInsts, "jitemitinsts"},
                {NxboxStall::Kind::JitEmitCapture, "jitemitcapture"},
                {NxboxStall::Kind::JitEmitTerminal, "jitemitterm"},
                {NxboxStall::Kind::JitEmitRanges, "jitemitranges"},
                {NxboxStall::Kind::JitEmitRegister, "jitemitregister"},
                {NxboxStall::Kind::JitEmitPatch, "jitemitpatch"},
                {NxboxStall::Kind::JitEmitWx, "jitemitwx"},
                {NxboxStall::Kind::JitEmitCommit, "jitemitcommit"},
                {NxboxStall::Kind::JitInvalidate, "jitinvalidate"},
                {NxboxStall::Kind::Io, "io"},
                {NxboxStall::Kind::Aes, "aes"},
                {NxboxStall::Kind::GpuBusy, "gpu"},
                {NxboxStall::Kind::GlSync, "glsync"},
                {NxboxStall::Kind::GlFinish, "glfinish"},
                {NxboxStall::Kind::Readback, "readback"},
                {NxboxStall::Kind::Query, "query"},
                {NxboxStall::Kind::Decommit, "decommit"},
                {NxboxStall::Kind::Present, "present"},
            }};
            for (const auto& [kind, name] : stall_kinds) {
                const auto taken = NxboxStall::Take(kind);
                stall_line += fmt::format(" {}={:.0f}/{:.0f}/{}", name, taken.total_us / 1000.0,
                                          taken.max_us / 1000.0, taken.calls);
            }
            // Brackets distinguish raw counts/bytes from duration triples and let existing
            // duration-only stall-report.py readers ignore these fields.
            constexpr std::array<std::pair<NxboxStall::JitEvent, const char*>, 32> jit_events{{
                {NxboxStall::JitEvent::RangeCalls, "jit_inv_calls"},
                {NxboxStall::JitEvent::RangeBytes, "jit_inv_bytes"},
                {NxboxStall::JitEvent::ClearRequests, "jit_clear_req"},
                {NxboxStall::JitEvent::CacheClears, "jit_clears"},
                {NxboxStall::JitEvent::CapacityClears, "jit_clear_capacity"},
                {NxboxStall::JitEvent::ExplicitClears, "jit_clear_explicit"},
                {NxboxStall::JitEvent::RangeBatches, "jit_range_batches"},
                {NxboxStall::JitEvent::FastmemFaults, "jit_fastmem_faults"},
                {NxboxStall::JitEvent::FastmemInvalidations, "jit_fastmem_inv"},
                {NxboxStall::JitEvent::DiskStale, "jit_disk_stale"},
                {NxboxStall::JitEvent::ClearBlocks, "jit_clear_blocks"},
                {NxboxStall::JitEvent::RangeBlocks, "jit_range_blocks"},
                {NxboxStall::JitEvent::EmptyInvalidations, "jit_inv_empty"},
                {NxboxStall::JitEvent::NewPc, "jit_pc_new"},
                {NxboxStall::JitEvent::RepeatPc, "jit_pc_repeat"},
                {NxboxStall::JitEvent::UnknownPc, "jit_pc_unknown"},
                {NxboxStall::JitEvent::PcCompileMax, "jit_pc_max"},
                {NxboxStall::JitEvent::NewKey, "jit_key_new"},
                {NxboxStall::JitEvent::RepeatKey, "jit_key_repeat"},
                {NxboxStall::JitEvent::UnknownKey, "jit_key_unknown"},
                {NxboxStall::JitEvent::SampledNewKey, "jit_sample64_new"},
                {NxboxStall::JitEvent::SampledRepeatKey, "jit_sample64_repeat"},
                {NxboxStall::JitEvent::SampledUnknownKey, "jit_sample64_unknown"},
                {NxboxStall::JitEvent::InstructionInvalidations, "jit_ic"},
                {NxboxStall::JitEvent::PageTableInvalidations, "jit_pt"},
                {NxboxStall::JitEvent::ProtectBytes, "jit_protect_bytes"},
                {NxboxStall::JitEvent::GuestCodeReads, "jit_guest_code_reads"},
                {NxboxStall::JitEvent::IrInstructions, "jit_ir_insts"},
                {NxboxStall::JitEvent::HostBytes, "jit_host_bytes"},
                {NxboxStall::JitEvent::RegAllocLookups, "jit_ra_lookups"},
                {NxboxStall::JitEvent::RegAllocProbes, "jit_ra_probes"},
                {NxboxStall::JitEvent::RegAllocReleases, "jit_ra_releases"},
            }};
            for (const auto& [event, name] : jit_events) {
                stall_line += fmt::format(" {}=[{}]", name, NxboxStall::TakeJit(event));
            }
            for (std::size_t core = 0; core < NxboxStall::JitCaches().size(); ++core) {
                auto& cache = NxboxStall::JitCaches()[core];
                stall_line += fmt::format(" jit_cache{}=[{}/{}]", core,
                                          cache.used.load(std::memory_order_relaxed),
                                          cache.capacity.load(std::memory_order_relaxed));
                stall_line += fmt::format(" jit_cache_peak{}=[{}] jit_compiled{}=[{}]", core,
                                          cache.peak.load(std::memory_order_relaxed), core,
                                          cache.compiled.exchange(0, std::memory_order_relaxed));
                stall_line += fmt::format(" jit_clear_total{}=[{}]", core,
                                          cache.clears.load(std::memory_order_relaxed));
            }
            Diagnostic(stall_line);
#endif
            worst_gap_ms = 0.0;
            hitches = 0;
            measured_at = now;
            idle_windows = frames == measured_frames ? idle_windows + 1 : 0;
            measured_frames = frames;
            // While no frame has appeared, record what every guest thread is doing (state, wait
            // reason and last saved PC/LR) so a stalled boot can be diagnosed.
            // A game that stops presenting after it was running gets the same dump, twice.
            if (((frames == 0 && stall_dumps < 3) || (idle_windows >= 2 && stall_dumps < 6)) &&
                std::chrono::duration<double>(now - started_at).count() > 20.0) {
                ++stall_dumps;
                Diagnostic(fmt::format("GUEST_THREADS idle_windows={} frames={}", idle_windows,
                                       frames));
                if (auto* process = system.ApplicationProcess()) {
                    for (auto& thread : process->GetThreadList()) {
                        const auto& ctx = thread.GetContext();
                        Diagnostic(fmt::format(
                            "GUEST_THREAD id={} core={} prio={} state={} wait={} pc={:#x} lr={:#x} "
                            "sp={:#x}",
                            thread.GetThreadId(), thread.GetActiveCore(), thread.GetPriority(),
                            static_cast<unsigned>(thread.GetState()),
                            static_cast<unsigned>(thread.GetWaitReasonForDebugging()), ctx.pc,
                            ctx.lr, ctx.sp));
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

// Title ID from an nxbox://play?title=<ID> activation; empty when the app was launched normally.
std::mutex g_protocol_mutex;
std::string g_protocol_title;
bool g_protocol_set = false;

std::string TakeProtocolPlayTitle() {
    const std::lock_guard lock(g_protocol_mutex);
    return g_protocol_title;
}

// Looks `title_id` up in the library (scans LocalState\games on a worker thread while this thread
// keeps pumping the window, the same way the library screen does). Returns true and fills
// `choice` with what the library screen would have returned for that game.
bool FindLibraryGame(const winrt::Windows::UI::Core::CoreWindow& window,
                     const std::filesystem::path& local_state, const std::string& title_id,
                     Ui::ChosenGame& choice, Ui::LaunchScreen& splash, Ui::LaunchStatus& status,
                     const std::atomic<bool>& closed) {
    using namespace winrt::Windows::UI::Core;
    Ui::LibraryScan scan(local_state, /*convert_nsz=*/false);
    while (!scan.Finished() && !closed.load()) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        splash.Draw(status);
    }
    if (closed.load())
        return false;
    for (const Ui::GameEntry& game : scan.Take()) {
        if (_stricmp(game.title_id.c_str(), title_id.c_str()) != 0) {
            continue;
        }
        if (game.incomplete) {
            status.failure.store(LibraryContent::Label(game.problem));
            return false;
        }
        choice.path = winrt::to_string(game.path.wstring());
        choice.title_id = game.title_id;
        choice.sync_name =
            SaveSync::GameNameFromNames(game.nacp_names, std::stoull(game.title_id, nullptr, 16));
        choice.display_name = game.name;
        return true;
    }
    return false;
}

} // namespace

// LocalState\move.txt: one "source|destination" pair per line, moved in order before the library
// opens. The console's internal storage and the removable drive are both small, so making room
// for one game means moving another. Each file is copied in 4 MiB chunks through IOFile (the
// CreateFileFromAppW path reaches removable drives), checked by size, and only then deleted at
// its source. Lines that fail stay in move.txt for the next launch.
void ProcessMoves(const std::filesystem::path& local_state) {
    namespace fs = std::filesystem;
    const fs::path list = local_state / "move.txt";
    std::ifstream in(list);
    if (!in) {
        return;
    }
    std::vector<std::string> remaining;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto bar = line.find('|');
        if (bar == std::string::npos) {
            continue;
        }
        const std::string from = line.substr(0, bar);
        const std::string to = line.substr(bar + 1);
        const u64 size = Common::FS::GetSize(from);
        Common::FS::IOFile src(from, Common::FS::FileAccessMode::Read,
                               Common::FS::FileType::BinaryFile);
        const std::string partial = to + ".partial";
        bool ok = src.IsOpen() && size != 0 &&
                  Common::FS::CreateDirs(fs::path{Common::FS::ToU8String(to)}.parent_path());
        if (ok) {
            Common::FS::IOFile dst(partial, Common::FS::FileAccessMode::Write,
                                   Common::FS::FileType::BinaryFile);
            std::vector<u8> chunk(4u << 20);
            u64 done = 0;
            auto sample = std::chrono::steady_clock::now();
            ok = dst.IsOpen();
            while (ok && done < size) {
                const std::size_t want =
                    static_cast<std::size_t>(std::min<u64>(chunk.size(), size - done));
                ok = src.ReadSpan(std::span<u8>(chunk.data(), want)) == want &&
                     dst.WriteSpan(std::span<const u8>(chunk.data(), want)) == want;
                done += want;
                if (std::chrono::steady_clock::now() - sample > std::chrono::seconds(10)) {
                    sample = std::chrono::steady_clock::now();
                    Diagnostic(fmt::format("MOVE {} {}/{} MiB", to, done >> 20, size >> 20));
                }
            }
            ok = ok && dst.Commit();
        }
        src.Close();
        ok = ok && Common::FS::GetSize(partial) == size && Common::FS::RenameFile(partial, to) &&
             Common::FS::RemoveFile(from);
        if (!ok) {
            void(Common::FS::RemoveFile(partial));
            remaining.push_back(line);
        }
        Diagnostic(fmt::format("MOVE {} {} -> {} ({} MiB)", ok ? "ok" : "failed", from, to,
                               size >> 20));
    }
    in.close();
    if (remaining.empty()) {
        void(Common::FS::RemoveFile(list));
    } else {
        std::ofstream out(list, std::ios::trunc);
        for (const auto& pending : remaining) {
            out << pending << '\n';
        }
    }
}

void SetProtocolPlayTitle(std::string title_id) {
    const std::lock_guard lock(g_protocol_mutex);
    if (!g_protocol_set) {
        g_protocol_set = true;
        g_protocol_title = std::move(title_id);
    }
}

static void RunGameViewSession(const winrt::Windows::UI::Core::CoreWindow& window,
                               const std::string& path, bool allow_official_startup,
                               bool& return_home) {
    using namespace winrt::Windows::UI::Core;
    // Prepare the NAND before the library or save sync runs, including on a fresh install.
    ApplyEdenSettingsFile();
    {
        const Service::Account::ProfileManager profiles;
        Service::Mii::MiiManager miis;
        Service::Mii::DatabaseSessionMetadata metadata{};
        const auto result = miis.Initialize(metadata);
        Diagnostic(fmt::format("PREPARED_USER uuid={} mii_result={:#x}",
                               profiles.GetLastOpenedUser().RawString(), result.raw));
    }
    const std::string protocol_title = TakeProtocolPlayTitle();
    const bool protocol_launch = !protocol_title.empty();
    std::atomic<bool> closed{false};
    const auto close_token = window.Closed([&](const auto&, const auto&) { closed.store(true); });
    SCOPE_EXIT {
        window.Closed(close_token);
    };
    Ui::LaunchStatus launch_status;
    std::unique_ptr<Ui::LaunchScreen> splash;
    std::string protocol_path;
    // LocalState\usb_scan_test.txt: a one-off trigger to test the setup screen's render pipeline
    // (Direct2D/DirectWrite on this same CoreWindow, before Mesa/OpenGL takes it over) together
    // with the USB scanner, without wiring either into the real boot flow yet.
    if (!protocol_launch &&
        std::filesystem::exists(
            std::filesystem::path(winrt::to_string(
                winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
            "usb_scan_test.txt")) {
        auto scan_worker = std::async(std::launch::async, [] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            SCOPE_EXIT { winrt::uninit_apartment(); };
            return ScanUsbForGamesAndKeys();
        });
        while (scan_worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(1);
        }
        const auto scan = scan_worker.get();
        ShowSetupScreen(window, "USB drive_found=" + std::string(scan.drive_found ? "yes" : "no") +
                                    " games=" + std::to_string(scan.games.size()));
    }
    // The library: the player picks a game on this same window, drawn with Direct2D/DirectWrite
    // before Mesa/OpenGL takes the window over. The screen releases every graphics object before it
    // returns. A chosen game is written to game.txt and booted by the normal path below; leaving
    // with B returns without booting a game.
    // LocalState\skip_library.txt skips the screen, for unattended runs where nobody can press A.
    //
    // SwitchSaveSync (docs/nxbox-ui.md, Increment 4) hangs off the same flow, and only for a game
    // chosen in the library, because that is where its title ID and NACP names are known:
    //  - an upload still owed from the last session (the app was closed or suspended before the
    //    game's save went to the cloud) is done first, behind a progress screen;
    //  - before the chosen game boots, with sync enabled, its save is reconciled with the cloud;
    //  - after the game closed (the worker below, once Eden released the save files), the Xbox
    //    save is uploaded. The window has no renderer then, so that step has no screen.
    // A marker in LocalState (savesync_pending.txt) is written before the boot and removed once the
    // upload succeeded, so a session that never reached the upload (Home suspends the app and the
    // system may end it; the window can be closed) is finished at the next launch.
    bool chosen_in_library = false;
    bool sync_after_exit = false;
    Ui::SyncGame sync_game;
    const std::filesystem::path local_state(
        winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
    if (!Ui::PrepareAccounts(window, local_state, false))
        return;
    if (protocol_launch && !Ui::PrepareAccounts(window, local_state, true))
        return;
    auto firmware_worker = std::async(std::launch::async, [&local_state] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        SCOPE_EXIT {
            winrt::uninit_apartment();
        };
        PrepareFirmware(local_state);
    });
    while (firmware_worker.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        Sleep(16);
    }
    firmware_worker.get();
    const auto show_failure = [&](Ui::Text message) {
        // Called only after all GL objects have gone away, or before Mesa was constructed.
        splash.reset();
        Ui::LaunchScreen error_screen(window, local_state, protocol_title);
        Ui::Input input(window);
        launch_status.phase.store(Ui::LaunchPhase::Failed);
        while (!closed.load()) {
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            input.Update();
            if (input.Pressed(Ui::Button::B))
                break;
            error_screen.Draw(launch_status, message);
        }
    };
    if (protocol_launch) {
        splash = std::make_unique<Ui::LaunchScreen>(window, local_state, protocol_title);
        splash->Draw(launch_status);
        Diagnostic("PROTOCOL_PHASE lookup");
        Ui::ChosenGame choice;
        if (!FindLibraryGame(window, local_state, protocol_title, choice, *splash, launch_status,
                             closed)) {
            if (!closed.load()) {
                Diagnostic("PROTOCOL_GAME_NOT_FOUND " + protocol_title);
                const auto failure = launch_status.failure.load();
                show_failure(failure == Ui::Text::LaunchFailed ? Ui::Text::LaunchMissing : failure);
            }
            return;
        }
        protocol_path = choice.path;
        splash->SetName(choice.display_name);
        launch_status.phase.store(Ui::LaunchPhase::Keys);
        splash->Draw(launch_status);
        Diagnostic("PROTOCOL_LAUNCH " + protocol_title);
        Diagnostic("PROTOCOL_PHASE keys");
        if (Ui::SyncEnabled()) {
            sync_game.title_id = choice.title_id;
            sync_game.name = choice.sync_name;
            sync_game.display_name = choice.display_name;
            ApplyEdenSettingsFile();
            ApplyEdenSettingsFile(TitleSettings::File(local_state, choice.title_id));
            sync_game.profile_uuid = SaveSync::ActiveProfileUuid();
            splash->Release();
            if (Ui::HasPendingSync(local_state) && Ui::RunPendingSync(window, local_state))
                return;
            if (Ui::RunBootSync(window, local_state, sync_game))
                return;
            splash = std::make_unique<Ui::LaunchScreen>(window, local_state, protocol_title);
            splash->SetName(choice.display_name);
            sync_after_exit = true;
        }
    }
    if (!protocol_launch && std::filesystem::exists(local_state / "move.txt")) {
        // A worker does the copying; the window keeps pumping so the system sees a live app.
        auto mover = std::async(std::launch::async, [&local_state] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            SCOPE_EXIT {
                winrt::uninit_apartment();
            };
            ProcessMoves(local_state);
        });
        while (mover.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            window.Dispatcher().ProcessEvents(
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(16);
        }
    }
    const bool official_startup = !protocol_launch && allow_official_startup &&
                                  UseOfficialSwitchInterface(local_state) &&
                                  GetFirmwareStatus().home_available;
    if (official_startup)
        protocol_path = std::string(SwitchHomeBootTarget);
    if (!protocol_launch && !official_startup &&
        (!allow_official_startup || !std::filesystem::exists(local_state / "skip_library.txt"))) {
        if (Ui::HasPendingSync(local_state)) {
            ApplyEdenSettingsFile(); // the active profile decides which save folder is synced
            if (Ui::RunPendingSync(window, local_state)) {
                return; // the window was closed
            }
        }
        Ui::ChosenGame choice;
        std::string chosen;
        chosen = Ui::RunLibrary(window, &choice);
        if (chosen.empty()) {
            return; // Leaving the library must not boot the previously remembered game.
        }
        if (!Ui::PrepareAccounts(window, local_state, true))
            return;
        if (chosen == SwitchHomeBootTarget) {
            protocol_path = chosen;
        } else if (chosen == SwitchMiiEditBootTarget) {
            protocol_path = chosen;
        } else if (!chosen.empty()) {
            RememberChosenGame(local_state, chosen);
            chosen_in_library = true;
            if (Ui::SyncEnabled()) {
                sync_game.title_id = choice.title_id;
                sync_game.name = choice.sync_name;
                sync_game.display_name = choice.display_name;
                ApplyEdenSettingsFile();
                ApplyEdenSettingsFile(TitleSettings::File(local_state, choice.title_id));
                sync_game.profile_uuid = SaveSync::ActiveProfileUuid();
                if (Ui::RunBootSync(window, local_state, sync_game)) {
                    return; // the window was closed
                }
                sync_after_exit = true;
            }
        }
    }
    if ((official_startup || (!protocol_launch && !chosen_in_library && protocol_path.empty())) &&
        !Ui::PrepareAccounts(window, local_state, true))
        return;
    std::atomic<bool> done{false};
    SessionUi session_ui;
    auto gamepad = std::make_shared<XboxGamepad>();
    // Handled so the system does not treat B or Menu as navigation while a game runs.
    const auto key_down_token = window.KeyDown([gamepad](const auto&, const KeyEventArgs& args) {
        if (gamepad->OnKey(args.VirtualKey(), true))
            args.Handled(true);
    });
    const auto key_up_token = window.KeyUp([gamepad](const auto&, const KeyEventArgs& args) {
        if (gamepad->OnKey(args.VirtualKey(), false))
            args.Handled(true);
    });
    Lifecycle lifecycle;
    using winrt::Windows::ApplicationModel::Core::CoreApplication;
    // NXBOX_FAST_SUSPEND=0 restores the old behaviour (the loop alone completes the deferral).
    char fast_env[2]{};
    const bool fast_suspend = GetEnvironmentVariableA("NXBOX_FAST_SUSPEND", fast_env,
                                                      sizeof(fast_env)) != 1 ||
                              fast_env[0] != '0';
    const auto suspending_token =
        CoreApplication::Suspending([&lifecycle, fast_suspend](const auto&, const auto& args) {
            Diagnostic("SUSPENDING");
            {
                std::scoped_lock lock{lifecycle.mutex};
                lifecycle.deferral = args.SuspendingOperation().GetDeferral();
            }
            lifecycle.paused_ack.store(false, std::memory_order_release);
            lifecycle.request.store(Lifecycle::Suspend, std::memory_order_release);
            if (!fast_suspend) {
                return;
            }
            // The loop may be stuck in system.Pause() waiting for guest cores; never let the
            // OS deadline depend on it: complete the deferral after at most ~1 s regardless.
            if (lifecycle.worker.joinable()) {
                lifecycle.worker.join();
            }
            lifecycle.worker = std::thread([&lifecycle] {
                for (int waited = 0;
                     waited < 200 && !lifecycle.paused_ack.load(std::memory_order_acquire);
                     ++waited) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                Diagnostic(lifecycle.paused_ack.load(std::memory_order_acquire)
                               ? "SUSPEND_DEFERRAL_COMPLETE"
                               : "SUSPEND_DEFERRAL_TIMEOUT");
                lifecycle.CompleteDeferral();
            });
        });
    const auto resuming_token = CoreApplication::Resuming([&lifecycle](const auto&, const auto&) {
        lifecycle.request.store(Lifecycle::Resume, std::memory_order_release);
    });
    SCOPE_EXIT {
        window.KeyDown(key_down_token);
        window.KeyUp(key_up_token);
        CoreApplication::Suspending(suspending_token);
        CoreApplication::Resuming(resuming_token);
        if (lifecycle.worker.joinable()) {
            lifecycle.worker.join();
        }
    };
    // Dev Home installs a package as an App, capped at 1 GiB; games then die in bad_alloc. Only
    // the user can switch it to Game, so say so instead of crashing.
    if (winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() < (2ULL << 30)) {
        Diagnostic(fmt::format("MEMORY_LIMIT app_type_app limit={} MiB",
                               winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20));
        show_failure(Ui::Text::LaunchAppType);
        return;
    }
    // The driver targets the current HDMI surface. Layout remains 16:9 until resize handling lands.
    Ui::Pixels initial_frame;
    if (splash) {
        splash->Draw(launch_status, Ui::Text::LaunchFailed, &initial_frame);
        splash->Release();
    }
    // Settings may have changed in the library. Select the API before creating the window.
    ApplyEdenSettingsFile();
    std::shared_ptr<MesaWindow> graphics;
    try {
        graphics = std::make_shared<MesaWindow>(window, 1920, 1080);
        if (splash)
            graphics->PresentLaunchFrame(initial_frame, true);
    } catch (...) {
        graphics.reset();
        if (!protocol_launch)
            throw;
        Diagnostic("PROTOCOL_GRAPHICS_FAILED");
        show_failure(Ui::Text::LaunchFailed);
        return;
    }
    initial_frame = {};
    std::atomic<bool> exit_sync{false};
    std::mutex exit_sync_mutex;
    SaveSync::SyncProgress exit_sync_progress;
    std::unique_ptr<Ui::Renderer> exit_sync_renderer;
    std::thread worker([&, graphics = std::move(graphics)]() mutable {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        SCOPE_EXIT {
            // Keep the UI dispatcher alive while the graphics driver releases its resources.
            FlushDeviceApiRing();
            graphics.reset();
            FlushDeviceApiRing();
            lifecycle.CompleteDeferral();
            winrt::uninit_apartment();
            session_ui.return_home.store(true, std::memory_order_release);
            done.store(true, std::memory_order_release);
        };
        try {
            RunGame(*graphics, path, closed, gamepad, lifecycle, chosen_in_library, protocol_path,
                    splash.get(), launch_status, session_ui);
            session_ui.return_home.store(true, std::memory_order_release);
            // The game is over and Eden released the save files: this is the safe point to upload.
            if (sync_after_exit) {
                // Release Mesa before the dispatcher takes the window for the small upload toast.
                FlushDeviceApiRing();
                graphics.reset();
                exit_sync.store(true, std::memory_order_release);
                Ui::SyncAfterExitHeadless(
                    local_state, sync_game, [&](const SaveSync::SyncProgress& progress) {
                        const std::lock_guard<std::mutex> lock(exit_sync_mutex);
                        exit_sync_progress = progress;
                    });
            }
        } catch (const winrt::hresult_error& error) {
            launch_status.phase.store(Ui::LaunchPhase::Failed);
            Diagnostic("GAME_FAIL " + winrt::to_string(error.message()));
        } catch (const std::exception& error) {
            launch_status.phase.store(Ui::LaunchPhase::Failed);
            Diagnostic(std::string("GAME_FAIL ") + error.what());
        }
    });
    while (!done.load(std::memory_order_acquire)) {
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        if (closed.load())
            SaveSync::CancelSync();
        if (session_ui.errors->Current().has_value() &&
            !session_ui.return_home.load(std::memory_order_acquire) && !closed.load()) {
            try {
                Ui::RunErrorOverlay(window, *session_ui.errors, closed, session_ui.return_home);
            } catch (const winrt::hresult_error& error) {
                Diagnostic("ERROR_OVERLAY_FAILED " + winrt::to_string(error.message()));
                session_ui.errors->Close();
            } catch (const std::exception& error) {
                Diagnostic(std::string("ERROR_OVERLAY_FAILED ") + error.what());
                session_ui.errors->Close();
            }
        }
        if (session_ui.overlay_open.load(std::memory_order_acquire) &&
            !session_ui.return_home.load(std::memory_order_acquire) && !closed.load()) {
            try {
                Ui::RunSessionOverlay(window, session_ui.return_home,
                                      session_ui.guest_home_request);
            } catch (const winrt::hresult_error& error) {
                Diagnostic("SESSION_OVERLAY_FAILED " + winrt::to_string(error.message()));
            } catch (const std::exception& error) {
                Diagnostic(std::string("SESSION_OVERLAY_FAILED ") + error.what());
            }
            session_ui.overlay_open.store(false, std::memory_order_release);
        }
        if (exit_sync.load(std::memory_order_acquire) && !closed.load()) {
            try {
                if (!exit_sync_renderer) {
                    exit_sync_renderer = std::make_unique<Ui::Renderer>();
                    exit_sync_renderer->Initialize(window);
                }
                SaveSync::SyncProgress progress;
                {
                    const std::lock_guard<std::mutex> lock(exit_sync_mutex);
                    progress = exit_sync_progress;
                }
                Ui::DrawExitSyncToast(*exit_sync_renderer, sync_game, progress);
            } catch (const winrt::hresult_error&) {
                exit_sync_renderer.reset();
            }
        }
        Sleep(1);
    }
    worker.join();
    // A failed launch says why, then goes back to the library: it used to close the app with no
    // message, which players reported as a crash. Home, Mii and the official startup return too;
    // the next pass shows the library instead of booting them again. Only a protocol launch,
    // which another app asked for, ends here.
    const bool failed = launch_status.phase.load() == Ui::LaunchPhase::Failed;
    return_home = session_ui.return_home.load() && !closed.load() && !failed;
    if (failed && !closed.load()) {
        show_failure(launch_status.failure.load());
        return_home = !protocol_launch && !closed.load();
    }
}

void RunGameView(const winrt::Windows::UI::Core::CoreWindow& window, const std::string& path) {
    bool allow_official_startup = true;
    for (;;) {
        bool return_home = false;
        RunGameViewSession(window, path, allow_official_startup, return_home);
        if (!return_home)
            return;
        // All session resources and save uploads are finished before the library takes the window.
        allow_official_startup = false;
    }
}
} // namespace EdenXbox
