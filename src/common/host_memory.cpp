// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include <iterator>
#include "common/container/unordered_map.h"
#include <boost/icl/separate_interval_set.hpp>
#include <windows.h>
#include "common/dynamic_library.h"
#ifdef YUZU_UWP_APPCONTAINER
#include "common/nxbox_stall.h"
#endif

#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <boost/icl/interval_set.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "common/scope_exit.h"

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/random.h>
#include <mach/vm_map.h>
#include <mach/mach.h>
#elif defined(__FreeBSD__)
#include <sys/shm.h>
#elif defined(__OPENORBIS__)
#include <orbis/libkernel.h>
#endif

// FreeBSD
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
// Solaris 11 and illumos
#ifndef MAP_ALIGNED_SUPER
#define MAP_ALIGNED_SUPER 0
#endif
// macOS
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#endif // ^^^ POSIX ^^^

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string_view>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/demand_commit.h"
#include "common/free_region_manager.h"
#include "common/host_memory.h"
#include "common/logging.h"

#if defined(__ANDROID__) && __ANDROID_API__ < 30
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static int memfd_create(const char* name, unsigned int flags) {
    return syscall(__NR_memfd_create, name, flags);
}
#endif

namespace Common {

[[maybe_unused]] constexpr size_t PageAlignment = 0x1000;
[[maybe_unused]] constexpr size_t HugePageSize = 0x200000;

#ifdef _WIN32

// Manually imported for MinGW compatibility
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

// On the Xbox/UWP AppContainer the placeholder virtual-memory APIs used for the emulated DRAM must go
// through the sandbox-legal *FromApp variants. The signatures of VirtualAlloc2FromApp and
// MapViewOfFile3FromApp match their base versions (we just resolve a different export); only the file
// mapping differs (CreateFileMappingFromApp has no extended-parameter form, so it takes the page
// protection OR'd with SEC_COMMIT directly). VirtualFree/VirtualFreeEx/UnmapViewOfFile2/CloseHandle
// are permitted in the AppContainer and are left unchanged.
//
// Auto-detected from the Windows SDK app-family partition (a UWP/WindowsStore CMake target defines
// WINAPI_FAMILY == WINAPI_FAMILY_APP); force it for a desktop compile/run check with
// -DYUZU_UWP_APPCONTAINER. The *FromApp exports also exist on desktop Win10+, so the forced build is
// runtime-valid there too.
// VirtualProtectFromApp is resolved dynamically (like the other kernelbase entry points below)
// rather than called directly: a direct call would pull in the UWP/onecore import library, whereas
// GetProcAddress keeps this translation unit free of link-time dependencies on the target's link set.
// The plain desktop path keeps calling VirtualProtect directly, so it is byte-for-byte unchanged.
#if defined(YUZU_UWP_APPCONTAINER) || (defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP)
#define HOST_MEMORY_USE_FROM_APP 1
#define HOST_MEMORY_VIRTUAL_PROTECT pfn_VirtualProtectFromApp
#else
#define HOST_MEMORY_VIRTUAL_PROTECT VirtualProtect
#endif

using PFN_CreateFileMapping2 = _Ret_maybenull_ HANDLE(WINAPI*)(
    _In_ HANDLE File, _In_opt_ SECURITY_ATTRIBUTES* SecurityAttributes, _In_ ULONG DesiredAccess,
    _In_ ULONG PageProtection, _In_ ULONG AllocationAttributes, _In_ ULONG64 MaximumSize,
    _In_opt_ PCWSTR Name,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_VirtualAlloc2 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress, _In_ SIZE_T Size,
    _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_MapViewOfFile3 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_ HANDLE FileMapping, _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress,
    _In_ ULONG64 Offset, _In_ SIZE_T ViewSize, _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_UnmapViewOfFile2 = BOOL(WINAPI*)(_In_ HANDLE Process, _In_ PVOID BaseAddress,
                                           _In_ ULONG UnmapFlags);

// AppContainer-legal file mapping. Unlike CreateFileMapping2 it has no extended-parameter form; the
// page protection carries SEC_COMMIT directly (the same shape as classic CreateFileMapping).
using PFN_CreateFileMappingFromApp = _Ret_maybenull_ HANDLE(WINAPI*)(
    _In_ HANDLE File, _In_opt_ PSECURITY_ATTRIBUTES SecurityAttributes, _In_ ULONG PageProtection,
    _In_ ULONG64 MaximumSize, _In_opt_ PCWSTR Name);

// VirtualProtectFromApp — identical shape to VirtualProtect; resolved by name under the AppContainer.
using PFN_VirtualProtect = BOOL(WINAPI*)(_In_ LPVOID Address, _In_ SIZE_T Size,
                                         _In_ DWORD NewProtection, _Out_ PDWORD OldProtection);

// VirtualAllocFromApp — reserves the private DRAM backing and demand-commits its pages (see below).
using PFN_VirtualAllocFromApp = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_opt_ PVOID BaseAddress, _In_ SIZE_T Size, _In_ ULONG AllocationType, _In_ ULONG Protection);

// Vectored-exception-handler APIs. Resolved by name (not called directly) so the binary does not hard-
// import api-ms-win-core-errorhandling-l1-1-1.dll — an api-set the Xbox AppContainer loader does not
// expose, which would make the app fail to ACTIVATE (no process, no crash dump). Kernelbase exports
// both by name, so they resolve at runtime even though the api-set is absent.
using PFN_AddVectoredExceptionHandler =
    PVOID(WINAPI*)(_In_ ULONG First, _In_ PVECTORED_EXCEPTION_HANDLER Handler);
using PFN_RemoveVectoredExceptionHandler = ULONG(WINAPI*)(_In_ PVOID Handle);

template <typename T>
static void GetFuncAddress(Common::DynamicLibrary& dll, const char* name, T& pfn) {
    if (!dll.GetSymbol(name, &pfn)) {
        LOG_CRITICAL(HW_Memory, "Failed to load {}", name);
    }
}

#ifdef HOST_MEMORY_USE_FROM_APP
// The VEH first handles lazy regional sections (backing and aliases). If their probe
// fails, it retains the existing private demand-commit backing for page-table accesses.
// Dynarmic's generated-code fallback is separate: x64 uses SEH/unwind tables.
namespace {
void* g_section_owner{};
bool (*g_section_fault)(void*, size_t, size_t){};
std::atomic<u8*> g_backing_base{nullptr};
std::atomic<size_t> g_backing_size{0};
PFN_VirtualAllocFromApp g_pfn_virtual_alloc_from_app{nullptr};
PFN_AddVectoredExceptionHandler g_pfn_add_veh{nullptr};
PFN_RemoveVectoredExceptionHandler g_pfn_remove_veh{nullptr};
void* g_backing_veh{nullptr};
std::atomic<std::uint64_t> g_backing_committed{0};

LONG NTAPI BackingDemandCommitHandler(EXCEPTION_POINTERS* ep) {
    if (ep == nullptr || ep->ExceptionRecord == nullptr ||
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_section_fault) {
        return g_section_fault(g_section_owner, ep->ExceptionRecord->ExceptionInformation[1],
                               ep->ExceptionRecord->ExceptionInformation[0])
                   ? EXCEPTION_CONTINUE_EXECUTION
                   : EXCEPTION_CONTINUE_SEARCH;
    }
    u8* const base = g_backing_base.load(std::memory_order_acquire);
    const size_t size = g_backing_size.load(std::memory_order_acquire);
    if (base == nullptr || g_pfn_virtual_alloc_from_app == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto chunk = DemandCommit::GetChunk(reinterpret_cast<std::uintptr_t>(base), size,
                                              ep->ExceptionRecord->ExceptionInformation[1],
                                              ep->ExceptionRecord->ExceptionInformation[0]);
    if (!chunk) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_pfn_virtual_alloc_from_app(base + chunk->offset, chunk->size, MEM_COMMIT,
                                     PAGE_READWRITE)) {
        g_backing_committed.fetch_add(chunk->size, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

std::uint64_t HostMemoryCommittedBytes() {
    return g_backing_committed.load(std::memory_order_relaxed);
}
#endif

#ifndef HOST_MEMORY_USE_FROM_APP
class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
        , process{GetCurrentProcess()}
        , kernelbase_dll("Kernelbase")
    {}

    bool Init() {
        if (!kernelbase_dll.IsOpen()) {
            LOG_CRITICAL(HW_Memory, "Failed to load Kernelbase.dll");
            return false;
        }
        GetFuncAddress(kernelbase_dll, "CreateFileMapping2", pfn_CreateFileMapping2);
        GetFuncAddress(kernelbase_dll, "VirtualAlloc2", pfn_VirtualAlloc2);
        GetFuncAddress(kernelbase_dll, "MapViewOfFile3", pfn_MapViewOfFile3);
        GetFuncAddress(kernelbase_dll, "UnmapViewOfFile2", pfn_UnmapViewOfFile2);

        // Allocate the linear DRAM backing.
        // Desktop: a file-mapping section aliased into both the linear backing and the fastmem arena.
        if (!pfn_CreateFileMapping2 || !pfn_VirtualAlloc2 || !pfn_MapViewOfFile3 || !pfn_UnmapViewOfFile2) {
            LOG_CRITICAL(HW_Memory, "Failed to find functions for virtual allocs");
            return false;
        }

        // Allocate backing file map
        backing_handle = pfn_CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_WRITE | FILE_MAP_READ, PAGE_READWRITE, SEC_COMMIT, backing_size, nullptr, nullptr, 0);
        if (!backing_handle) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate {} MiB of backing memory", backing_size >> 20);
            return false;
        }
        // Allocate a virtual memory for the backing file map as placeholder
        backing_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, backing_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} MiB of virtual memory", backing_size >> 20);
            return false;
        }
        // Map backing placeholder
        void* const ret = pfn_MapViewOfFile3(backing_handle, process, backing_base, 0, backing_size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (ret != backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to map {} MiB of virtual memory", backing_size >> 20);
            return false;
        }
        // Allocate virtual address placeholder
        virtual_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, virtual_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!virtual_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} GiB of virtual memory", virtual_size >> 30);
            return false;
        }
        return true;
    }

    ~Impl() {
        Release();
    }

    void* Allocate(size_t size) {
        auto* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (ptr == nullptr) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, error {}", size, GetLastError());
        }
        return ptr;
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        std::unique_lock lock{placeholder_mutex};
        if (!IsNiechePlaceholder(virtual_offset, length)) {
            Split(virtual_offset, length);
        }
        ASSERT(placeholders.find({virtual_offset, virtual_offset + length}) == placeholders.end());
        TrackPlaceholder(virtual_offset, host_offset, length);

        MapView(virtual_offset, host_offset, length);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        std::scoped_lock lock{placeholder_mutex};

        // Unmap until there are no more placeholders
        while (UnmapOnePlaceholder(virtual_offset, length)) {
        }
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        DWORD new_flags{};
        if (read && write) {
            new_flags = PAGE_READWRITE;
        } else if (read && !write) {
            new_flags = PAGE_READONLY;
        } else if (!read && !write) {
            new_flags = PAGE_NOACCESS;
        } else {
            UNIMPLEMENTED_MSG("Protection flag combination read={} write={}", read, write);
        }
        const size_t virtual_end = virtual_offset + length;

        std::scoped_lock lock{placeholder_mutex};
        auto [it, end] = placeholders.equal_range({virtual_offset, virtual_end});
        while (it != end) {
            const size_t offset = (std::max)(it->lower(), virtual_offset);
            const size_t protect_length = (std::min)(it->upper(), virtual_end) - offset;
            DWORD old_flags{};
            if (!HOST_MEMORY_VIRTUAL_PROTECT(virtual_base + offset, protect_length, new_flags,
                                             &old_flags)) {
                LOG_CRITICAL(HW_Memory, "Failed to change virtual memory protect rules");
            }
            ++it;
        }
    }

    void EnableDirectMappedAddress() {
        // TODO
        UNREACHABLE();
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{};
    u8* virtual_base{};

private:
    /// Release all resources in the object
    void Release() {
        if (!placeholders.empty()) {
            for (const auto& placeholder : placeholders) {
                if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder.lower(),
                                          MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_CRITICAL(HW_Memory, "Failed to unmap virtual memory placeholder");
                }
            }
            Coalesce(0, virtual_size);
        }
        if (virtual_base) {
            if (!VirtualFree(virtual_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free virtual memory");
            }
        }
        if (backing_base) {
            if (!pfn_UnmapViewOfFile2(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                LOG_CRITICAL(HW_Memory, "Failed to unmap backing memory placeholder");
            }
            if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free backing memory");
            }
        }
        if (!CloseHandle(backing_handle)) {
            LOG_CRITICAL(HW_Memory, "Failed to free backing memory file handle");
        }
    }

    /// Unmap one placeholder in the given range (partial unmaps are supported)
    /// Return true when there are no more placeholders to unmap
    bool UnmapOnePlaceholder(size_t virtual_offset, size_t length) {
        const auto it = placeholders.find({virtual_offset, virtual_offset + length});
        const auto begin = placeholders.begin();
        const auto end = placeholders.end();
        if (it == end) {
            return false;
        }
        const size_t placeholder_begin = it->lower();
        const size_t placeholder_end = it->upper();
        const size_t unmap_begin = (std::max)(virtual_offset, placeholder_begin);
        const size_t unmap_end = (std::min)(virtual_offset + length, placeholder_end);
        ASSERT(unmap_begin >= placeholder_begin && unmap_begin < placeholder_end);
        ASSERT(unmap_end <= placeholder_end && unmap_end > placeholder_begin);

        const auto host_pointer_it = placeholder_host_pointers.find(placeholder_begin);
        ASSERT(host_pointer_it != placeholder_host_pointers.end());
        const size_t host_offset = host_pointer_it->second;

        const bool split_left = unmap_begin > placeholder_begin;
        const bool split_right = unmap_end < placeholder_end;

        if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder_begin,
                                  MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to unmap placeholder");
        }
        // If we have to remap memory regions due to partial unmaps, we are in a data race as
        // Windows doesn't support remapping memory without unmapping first. Avoid adding any extra
        // logic within the panic region described below.

        // Panic region, we are in a data race right now
        if (split_left || split_right) {
            Split(unmap_begin, unmap_end - unmap_begin);
        }
        if (split_left) {
            MapView(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            MapView(unmap_end, host_offset + unmap_end - placeholder_begin,
                    placeholder_end - unmap_end);
        }
        // End panic region

        size_t coalesce_begin = unmap_begin;
        if (!split_left) {
            // Try to coalesce pages to the left
            coalesce_begin = it == begin ? 0 : std::prev(it)->upper();
            if (coalesce_begin != placeholder_begin) {
                Coalesce(coalesce_begin, unmap_end - coalesce_begin);
            }
        }
        if (!split_right) {
            // Try to coalesce pages to the right
            const auto next = std::next(it);
            const size_t next_begin = next == end ? virtual_size : next->lower();
            if (placeholder_end != next_begin) {
                // We can coalesce to the right
                Coalesce(coalesce_begin, next_begin - coalesce_begin);
            }
        }
        // Remove and reinsert placeholder trackers
        UntrackPlaceholder(it);
        if (split_left) {
            TrackPlaceholder(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            TrackPlaceholder(unmap_end, host_offset + unmap_end - placeholder_begin,
                             placeholder_end - unmap_end);
        }
        return true;
    }

    void MapView(size_t virtual_offset, size_t host_offset, size_t length) {
        if (!pfn_MapViewOfFile3(backing_handle, process, virtual_base + virtual_offset, host_offset,
                                length, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            LOG_CRITICAL(HW_Memory, "Failed to map placeholder");
        }
    }

    void Split(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to split placeholder");
        }
    }

    void Coalesce(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            LOG_CRITICAL(HW_Memory, "Failed to coalesce placeholders");
        }
    }

    void TrackPlaceholder(size_t virtual_offset, size_t host_offset, size_t length) {
        placeholders.insert({virtual_offset, virtual_offset + length});
        placeholder_host_pointers.emplace(virtual_offset, host_offset);
    }

    void UntrackPlaceholder(boost::icl::separate_interval_set<size_t>::iterator it) {
        placeholder_host_pointers.erase(it->lower());
        placeholders.erase(it);
    }

    /// Return true when a given memory region is a "nieche" and the placeholders don't have to be
    /// split.
    bool IsNiechePlaceholder(size_t virtual_offset, size_t length) const {
        const auto it = placeholders.upper_bound({virtual_offset, virtual_offset + length});
        if (it != placeholders.end() && it->lower() == virtual_offset + length) {
            return it == placeholders.begin() ? virtual_offset == 0
                                              : std::prev(it)->upper() == virtual_offset;
        }
        return false;
    }

    HANDLE process{};        ///< Current process handle
    HANDLE backing_handle{}; ///< File based backing memory

    DynamicLibrary kernelbase_dll;
    PFN_CreateFileMapping2 pfn_CreateFileMapping2{};
    PFN_VirtualAlloc2 pfn_VirtualAlloc2{};
    PFN_MapViewOfFile3 pfn_MapViewOfFile3{};
    PFN_UnmapViewOfFile2 pfn_UnmapViewOfFile2{};

    std::mutex placeholder_mutex;                                 ///< Mutex for placeholders
    boost::icl::separate_interval_set<size_t> placeholders;       ///< Mapped placeholders
    ::Common::unordered_map<size_t, size_t> placeholder_host_pointers; ///< Placeholder backing offset
};

#else
// UWP backing uses independent 64 KiB pagefile sections. A section is created only on
// first access, so neither a guest mapping nor a zero-fill charges untouched DRAM.
// Closing the last view/handle really releases commit (MEM_RESET/Discard do not).
class HostMemory::Impl {
    static constexpr size_t RegionSize = 64 * 1024;
    struct Mapping {
        size_t host;
        size_t length;
        DWORD protection{};
        bool mapped{};
        bool fault_disabled{};
    };
    struct Region {
        HANDLE handle{};
        std::set<size_t> aliases;
    };

public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}, virtual_size{virtual_size_}, kernelbase_dll{"Kernelbase"},
          regions((backing_size + RegionSize - 1) / RegionSize) {}

    bool Init() {
        GetFuncAddress(kernelbase_dll, "VirtualAllocFromApp", g_pfn_virtual_alloc_from_app);
        GetFuncAddress(kernelbase_dll, "AddVectoredExceptionHandler", g_pfn_add_veh);
        GetFuncAddress(kernelbase_dll, "RemoveVectoredExceptionHandler", g_pfn_remove_veh);
        if (!g_pfn_virtual_alloc_from_app || !g_pfn_add_veh || !g_pfn_remove_veh) {
            throw std::bad_alloc{};
        }
        GetFuncAddress(kernelbase_dll, "CreateFileMappingFromApp", create_section);
        GetFuncAddress(kernelbase_dll, "VirtualAlloc2FromApp", reserve);
        GetFuncAddress(kernelbase_dll, "MapViewOfFile3FromApp", map_view);
        GetFuncAddress(kernelbase_dll, "UnmapViewOfFile2", unmap_view);
        GetFuncAddress(kernelbase_dll, "VirtualProtectFromApp", protect);
        const char* reason = "missing_from_app_export";
        DWORD failure_error = GetLastError();
        const char* option = std::getenv("NXBOX_FASTMEM");
        if (option && std::string_view{option} == "0") {
            reason = "NXBOX_FASTMEM=0";
            failure_error = 0;
        } else if (create_section && reserve && map_view && unmap_view && protect) {
            bool ready{};
            try {
                ready = InitArena();
            } catch (const std::bad_alloc&) {
                ready = false;
            }
            if (ready) {
                g_section_owner = this;
                g_section_fault = [](void* owner, size_t address, size_t access) {
                    return static_cast<Impl*>(owner)->Fault(address, access);
                };
                if (InstallHandler()) {
                    LOG_INFO(HW_Memory,
                             "NXBOX MEMORY mode=fastmem backing=regional_sections "
                             "region_kib=64 arena_gib={} committed_mib=0 probe=passed veh=ready",
                             virtual_size >> 30);
                    return true;
                }
                g_section_fault = nullptr;
                g_section_owner = nullptr;
                reason = "veh_install_failed";
            } else {
                reason = probe_stage;
            }
            failure_error = ready ? GetLastError() : probe_error;
            ReleaseArena();
        }
        LOG_WARNING(HW_Memory,
                    "NXBOX MEMORY mode=page_table fastmem=unavailable reason={} error={}", reason,
                    failure_error);
        backing_base = static_cast<u8*>(
            g_pfn_virtual_alloc_from_app(nullptr, backing_size, MEM_RESERVE, PAGE_READWRITE));
        if (!backing_base) {
            throw std::bad_alloc{};
        }
        g_backing_base.store(backing_base, std::memory_order_release);
        g_backing_size.store(backing_size, std::memory_order_release);
        if (!InstallHandler()) {
            VirtualFree(backing_base, 0, MEM_RELEASE);
            backing_base = nullptr;
            g_backing_base.store(nullptr, std::memory_order_release);
            g_backing_size.store(0, std::memory_order_release);
            throw std::bad_alloc{};
        }
        return true;
    }

    ~Impl() {
        if (g_backing_veh) {
            g_pfn_remove_veh(g_backing_veh);
            g_backing_veh = nullptr;
        }
        g_section_fault = nullptr;
        g_section_owner = nullptr;
        g_backing_base.store(nullptr, std::memory_order_release);
        g_backing_size.store(0, std::memory_order_release);
        if (virtual_base) {
            ReleaseArena();
        } else if (backing_base) {
            VirtualFree(backing_base, 0, MEM_RELEASE);
        }
        g_backing_committed.store(0, std::memory_order_relaxed);
    }

    void* Allocate(size_t) {
        return nullptr;
    } // Init either supplies demand backing or throws.

    void Map(size_t offset, size_t host, size_t length, MemoryPermission permissions) {
        std::scoped_lock lock{mutex};
        UnmapLocked(offset, length);
        while (length) {
            const size_t count = (std::min)(length, RegionSize - host % RegionSize);
            auto next = mappings.lower_bound(offset);
            const size_t hole_begin = next == mappings.begin()
                                          ? 0
                                          : std::prev(next)->first + std::prev(next)->second.length;
            const size_t hole_end = next == mappings.end() ? virtual_size : next->first;
            Split(virtual_base, hole_begin, hole_end, offset, count);
            Mapping mapping{host, count};
            const DWORD flags = True(permissions & MemoryPermission::Write)  ? PAGE_READWRITE
                                : True(permissions & MemoryPermission::Read) ? PAGE_READONLY
                                                                             : PAGE_NOACCESS;
            mapping.protection = flags;
            mappings.emplace(offset, mapping);
            regions[host / RegionSize].aliases.insert(offset);
            offset += count;
            host += count;
            length -= count;
        }
    }

    void Unmap(size_t offset, size_t length) {
        std::scoped_lock lock{mutex};
        UnmapLocked(offset, length);
    }

    void Protect(size_t offset, size_t length, bool read, bool write, bool) {
        std::scoped_lock lock{mutex};
        const DWORD flags = read ? (write ? PAGE_READWRITE : PAGE_READONLY) : PAGE_NOACCESS;
        const size_t limit = offset + length;
        auto it = mappings.upper_bound(offset);
        if (it != mappings.begin())
            --it;
        while (it != mappings.end() && it->first < limit) {
            const auto [start, mapping] = *it;
            const size_t begin = (std::max)(start, offset);
            const size_t end = (std::min)(start + mapping.length, limit);
            if (begin >= end || mapping.protection == flags) {
                ++it;
                continue;
            }
            // A FromApp view cannot gain access beyond its original mapping protection.
            // Replace the affected interval lazily with its final permission. Splitting
            // preserves uniform permissions per view without a permissive mapping window.
            const size_t host = mapping.host + begin - start;
            const auto permissions = write && read ? MemoryPermission::ReadWrite
                                     : read        ? MemoryPermission::Read
                                                   : static_cast<MemoryPermission>(0);
            try {
                Map(begin, host, end - begin, permissions);
            } catch (const std::bad_alloc&) {
                // Placeholder split/coalesce failed: stop serving this interval through the
                // fault handler so accesses fall back to the page table instead of crashing.
                auto failed = mappings.upper_bound(begin);
                if (failed != mappings.begin())
                    (--failed)->second.fault_disabled = true;
                ++alias_failures;
            }
            it = mappings.lower_bound(end);
        }
    }

    // Called while the kernel owns the physical region being cleared. Other physical
    // regions retain their sections and aliases. Partial boundary chunks preserve bytes.
    void Clear(size_t offset, size_t length, u32 value) {
        std::scoped_lock lock{mutex};
        if (!virtual_base) {
            if (value == 0 && ((offset | length) & (PageAlignment - 1)) == 0 &&
                VirtualFree(backing_base + offset, length, MEM_DECOMMIT))
                return;
            std::memset(backing_base + offset, value, length);
            return;
        }
        while (length) {
            const size_t index = offset / RegionSize;
            const size_t count = (std::min)(length, RegionLength(index) - offset % RegionSize);
            if (value == 0 && offset % RegionSize == 0 && count == RegionLength(index)) {
                DropRegion(index);
            } else if (regions[index].handle || value != 0) {
                if (!EnsureRegion(index))
                    throw std::bad_alloc{};
                std::memset(backing_base + offset, value, count);
            }
            offset += count;
            length -= count;
        }
        if (reclaimed_bytes - reported_reclaimed >= 64 * 1024 * 1024) {
            reported_reclaimed = reclaimed_bytes;
            LOG_INFO(HW_Memory,
                     "NXBOX MEMORY backing=regional_sections committed_mib={} reclaimed_mib={} "
                     "backing_faults={} alias_faults={} alias_failures={}",
                     g_backing_committed.load(std::memory_order_relaxed) >> 20,
                     reclaimed_bytes >> 20, backing_faults, alias_faults, alias_failures);
        }
    }

    void EnableDirectMappedAddress() {
        UNREACHABLE();
    }
    const size_t backing_size;
    const size_t virtual_size;
    u8* backing_base{};
    u8* virtual_base{};

private:
    bool InstallHandler() {
        g_backing_committed.store(0, std::memory_order_relaxed);
        g_backing_veh = g_pfn_add_veh(1, BackingDemandCommitHandler);
        return g_backing_veh != nullptr || ApiFailure("AddVectoredExceptionHandler", nullptr, 0, 0);
    }

    size_t RegionLength(size_t index) const {
        return (std::min)(RegionSize, backing_size - index * RegionSize);
    }

    bool ApiFailure(const char* api, void* address, size_t size, DWORD protection) {
        probe_error = GetLastError();
        LOG_WARNING(HW_Memory,
                    "NXBOX MEMORY api_failed stage={} api={} address={} size={:#x} "
                    "protection={:#x} error={}",
                    probe_stage, api, address, size, protection, probe_error);
        return false;
    }

    bool InitArena() {
        probe_stage = "reserve_backing_failed";
        backing_base = static_cast<u8*>(reserve(process, nullptr, backing_size,
                                                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                PAGE_NOACCESS, nullptr, 0));
        if (!backing_base)
            return ApiFailure("VirtualAlloc2FromApp", nullptr, backing_size, PAGE_NOACCESS);
        probe_stage = "reserve_arena_failed";
        virtual_base = static_cast<u8*>(reserve(process, nullptr, virtual_size,
                                                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                PAGE_NOACCESS, nullptr, 0));
        if (!virtual_base)
            return ApiFailure("VirtualAlloc2FromApp", nullptr, virtual_size, PAGE_NOACCESS);
        probe_stage = "split_backing_failed";
        // Keep backing placeholder boundaries fixed. No allocation is needed in the VEH.
        for (size_t offset = 0; offset + RegionSize < backing_size; offset += RegionSize) {
            if (!VirtualFree(backing_base + offset, RegionSize,
                             MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
                return ApiFailure("VirtualFree", backing_base + offset, RegionSize, 0);
        }
        // Exercise the actual section and alias APIs, including a 4 KiB section offset,
        // protection, and destruction/recreation. The probe charges only one 64 KiB chunk.
        if (!EnsureRegion(0))
            return false;
        probe_stage = "split_alias_failed";
        if (!VirtualFree(virtual_base, RegionSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            return ApiFailure("VirtualFree", virtual_base, RegionSize, 0);
        if (!VirtualFree(virtual_base, PageAlignment, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            return ApiFailure("VirtualFree", virtual_base, PageAlignment, 0);
        probe_stage = "map_alias_4k_failed";
        backing_base[PageAlignment] = 0xA5;
        void* alias = map_view(regions[0].handle, process, virtual_base, PageAlignment,
                               PageAlignment, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (alias != virtual_base)
            return ApiFailure("MapViewOfFile3FromApp", virtual_base, PageAlignment, PAGE_READWRITE);
        probe_mapped = true;
        DWORD old{};
        probe_stage = "verify_alias_failed";
        if (static_cast<volatile u8*>(alias)[0] != 0xA5)
            return false;
        probe_stage = "protect_alias_failed";
        if (!protect(alias, PageAlignment, PAGE_READONLY, &old))
            return ApiFailure("VirtualProtectFromApp", alias, PageAlignment, PAGE_READONLY);
        if (!protect(alias, PageAlignment, PAGE_READWRITE, &old))
            return ApiFailure("VirtualProtectFromApp", alias, PageAlignment, PAGE_READWRITE);
        probe_stage = "unmap_alias_failed";
        if (!unmap_view(process, alias, MEM_PRESERVE_PLACEHOLDER))
            return ApiFailure("UnmapViewOfFile2", alias, PageAlignment, 0);
        probe_mapped = false;
        probe_stage = "coalesce_alias_failed";
        if (!VirtualFree(virtual_base, virtual_size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
            return ApiFailure("VirtualFree", virtual_base, virtual_size, 0);
        probe_stage = "reclaim_probe_failed";
        DropRegion(0);
        if (!EnsureRegion(0) || backing_base[PageAlignment] != 0)
            return false;
        DropRegion(0);
        reclaimed_bytes = 0;
        return true;
    }

    bool EnsureRegion(size_t index) {
        auto& region = regions[index];
        if (region.handle)
            return true;
        probe_stage = "create_section_failed";
        HANDLE handle = create_section(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                       RegionLength(index), nullptr);
        if (!handle)
            return ApiFailure("CreateFileMappingFromApp", nullptr, RegionLength(index),
                              PAGE_READWRITE | SEC_COMMIT);
        probe_stage = "map_backing_failed";
        void* view =
            map_view(handle, process, backing_base + index * RegionSize, 0, RegionLength(index),
                     MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (view != backing_base + index * RegionSize) {
            ApiFailure("MapViewOfFile3FromApp", backing_base + index * RegionSize,
                       RegionLength(index), PAGE_READWRITE);
            CloseHandle(handle);
            return false;
        }
        region.handle = handle;
        g_backing_committed.fetch_add(RegionLength(index), std::memory_order_relaxed);
        return true;
    }

    bool Fault(size_t address, size_t access) {
        if (access != 0 && access != 1)
            return false;
        std::scoped_lock lock{mutex};
        const size_t backing_offset = address - reinterpret_cast<size_t>(backing_base);
        if (backing_offset < backing_size) {
            const size_t index = backing_offset / RegionSize;
            // Another core may have satisfied the same fault while we waited for the lock.
            // The linear backing is always RW; only read/write faults within it are claimed.
            ++backing_faults;
            return EnsureRegion(index);
        }
        const size_t offset = address - reinterpret_cast<size_t>(virtual_base);
        if (offset >= virtual_size)
            return false;
        auto it = mappings.upper_bound(offset);
        if (it == mappings.begin())
            return false;
        auto& [start, mapping] = *--it;
        if (offset - start >= mapping.length || mapping.fault_disabled)
            return false;
        const DWORD flags = mapping.protection;
        if (flags == PAGE_NOACCESS || (access == 1 && flags != PAGE_READWRITE))
            return false;
        if (mapping.mapped)
            return true; // Concurrent fault already satisfied.
        if (!EnsureRegion(mapping.host / RegionSize))
            return false;
        if (map_view(regions[mapping.host / RegionSize].handle, process, virtual_base + start,
                     mapping.host % RegionSize, mapping.length, MEM_REPLACE_PLACEHOLDER,
                     mapping.protection, nullptr, 0) != virtual_base + start) {
            mapping.fault_disabled = true;
            ++alias_failures;
            return false;
        }
        mapping.mapped = true;
        ++alias_faults;
        return true;
    }

    void UnmapView(size_t offset, Mapping& mapping) {
        if (mapping.mapped) {
            // Failure leaves live aliases: never close their section or claim reclaim succeeded.
            if (!unmap_view(process, virtual_base + offset, MEM_PRESERVE_PLACEHOLDER)) {
                throw std::bad_alloc{};
            }
            mapping.mapped = false;
        }
    }

    void DropRegion(size_t index) {
        auto& region = regions[index];
        if (!region.handle)
            return;
        for (const size_t offset : region.aliases) {
            UnmapView(offset, mappings.at(offset));
        }
        if (!unmap_view(process, backing_base + index * RegionSize, MEM_PRESERVE_PLACEHOLDER)) {
            ApiFailure("UnmapViewOfFile2", backing_base + index * RegionSize, RegionLength(index),
                       0);
            throw std::bad_alloc{};
        }
        CloseHandle(region.handle);
        region.handle = nullptr;
        g_backing_committed.fetch_sub(RegionLength(index), std::memory_order_relaxed);
        reclaimed_bytes += RegionLength(index);
    }

    void Split(u8* base, size_t hole_begin, size_t hole_end, size_t begin, size_t length) {
        if (begin != hole_begin && !VirtualFree(base + hole_begin, begin - hole_begin,
                                                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            throw std::bad_alloc{};
        if (begin + length != hole_end &&
            !VirtualFree(base + begin, length, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            throw std::bad_alloc{};
    }

    void UnmapLocked(size_t offset, size_t length) {
        const size_t end = offset + length;
        auto it = mappings.upper_bound(offset);
        if (it != mappings.begin())
            --it;
        while (it != mappings.end() && it->first < end) {
            const size_t start = it->first;
            Mapping mapping = it->second;
            const size_t finish = start + mapping.length;
            if (finish <= offset) {
                ++it;
                continue;
            }
            UnmapView(start, it->second);
            regions[mapping.host / RegionSize].aliases.erase(start);
            it = mappings.erase(it);
            const size_t begin = (std::max)(start, offset);
            const size_t stop = (std::min)(finish, end);
            Split(virtual_base, start, finish, begin, stop - begin);
            mapping.mapped = false;
            if (start < begin) {
                Mapping left = mapping;
                left.length = begin - start;
                mappings.emplace(start, left);
                regions[left.host / RegionSize].aliases.insert(start);
            }
            if (stop < finish) {
                Mapping right = mapping;
                right.host += stop - start;
                right.length = finish - stop;
                mappings.emplace(stop, right);
                regions[right.host / RegionSize].aliases.insert(stop);
            }
        }
        // Coalesce the cleared interval with adjacent free placeholders.
        auto next = mappings.lower_bound(offset);
        const size_t begin =
            next == mappings.begin() ? 0 : std::prev(next)->first + std::prev(next)->second.length;
        const size_t stop = next == mappings.end() ? virtual_size : next->first;
        if (stop > begin && !VirtualFree(virtual_base + begin, stop - begin,
                                         MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            // A single placeholder needs no coalescing; Windows may reject that no-op.
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(virtual_base + begin, &info, sizeof(info)) ||
                info.RegionSize != stop - begin)
                throw std::bad_alloc{};
        }
    }

    void ReleaseArena() {
        for (auto& [offset, mapping] : mappings)
            UnmapView(offset, mapping);
        mappings.clear();
        for (auto& region : regions)
            region.aliases.clear();
        if (probe_mapped) {
            unmap_view(process, virtual_base, MEM_PRESERVE_PLACEHOLDER);
            probe_mapped = false;
        }
        for (size_t index = 0; index < regions.size(); ++index)
            DropRegion(index);
        for (auto* base : {virtual_base, backing_base}) {
            if (!base)
                continue;
            const size_t size = base == virtual_base ? virtual_size : backing_size;
            VirtualFree(base, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
            VirtualFree(base, 0, MEM_RELEASE);
        }
        virtual_base = backing_base = nullptr;
    }

    HANDLE process{GetCurrentProcess()};
    DynamicLibrary kernelbase_dll;
    PFN_CreateFileMappingFromApp create_section{};
    PFN_VirtualAlloc2 reserve{};
    PFN_MapViewOfFile3 map_view{};
    PFN_UnmapViewOfFile2 unmap_view{};
    PFN_VirtualProtect protect{};
    std::vector<Region> regions;
    std::map<size_t, Mapping> mappings;
    // Clear may fault on the private fallback backing while holding this lock.
    std::recursive_mutex mutex;
    bool probe_mapped{};
    DWORD probe_error{};
    const char* probe_stage = "section_alias_probe_failed";
    size_t backing_faults{};
    size_t alias_faults{};
    size_t alias_failures{};
    size_t reclaimed_bytes{};
    size_t reported_reclaimed{};
};
#endif

#elif defined(__OPENORBIS__) || defined(__managarm__)
// None of the luxuries of POSIX, all of the suffering
// For managarm: see https://github.com/managarm/managarm/issues/1370
#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef MAP_NOCORE
#define MAP_NOCORE 0
#endif

#ifdef ARCHITECTURE_arm64

static void* ChooseVirtualBase(size_t virtual_size) {
    constexpr uintptr_t Map39BitSize = (1ULL << 39);
    constexpr uintptr_t Map36BitSize = (1ULL << 36);

    // This is not a cryptographic application, we just want something random.
    std::mt19937_64 rng;

    // We want to ensure we are allocating at an address aligned to the L2 block size.
    // For Qualcomm devices, we must also allocate memory above 36 bits.
    const size_t lower = Map36BitSize / HugePageSize;
    const size_t upper = (Map39BitSize - virtual_size) / HugePageSize;
    const size_t range = upper - lower;

    // Try up to 64 times to allocate memory at random addresses in the range.
    for (int i = 0; i < 64; i++) {
        // Calculate a possible location.
        uintptr_t hint_address = ((rng() % range) + lower) * HugePageSize;

        // Try to map.
        // Note: we may be able to take advantage of MAP_FIXED_NOREPLACE here.
        void* map_pointer =
            mmap(reinterpret_cast<void*>(hint_address), virtual_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);

        // If we successfully mapped, we're done.
        if (reinterpret_cast<uintptr_t>(map_pointer) == hint_address) {
            return map_pointer;
        }

        // Unmap if necessary, and try again.
        if (map_pointer != MAP_FAILED) {
            munmap(map_pointer, virtual_size);
        }
    }

    return MAP_FAILED;
}

#else

static void* ChooseVirtualBase(size_t virtual_size) {
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__sun__) || defined(__HAIKU__) || defined(__managarm__) || defined(__AIX__)
    void* virtual_base = mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_ALIGNED_SUPER | MAP_NOCORE, -1, 0);
    if (virtual_base != MAP_FAILED)
        return virtual_base;
#endif
    return mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);
}

#endif

#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
/// Most Unices don't have a portable shm_open (AIX, OpenBSD, NetBSD, Solaris 11, OpenIndiana)
/// Portable implementation of shm_open(SHM_ANON, ...) - roughly equivalent but without
/// OS support - may fail sporadically, beware!
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-";
    char *const limit = name + sizeof(name) - 1;
    *limit = '\0';
    char *start = name + strlen(name);
    for (int tries = 0; tries < 4; tries++) {
        struct timespec tv;
        clock_gettime(CLOCK_REALTIME, &tv);
        unsigned long r = (unsigned long)tv.tv_sec + (unsigned long)tv.tv_nsec;
        for (char *fill = start; fill < limit; r /= 8)
            *fill++ = '0' + (r % 8);
        int fd = shm_open(name, flags, mode);
        if (fd != -1) {
            if (shm_unlink(name) == -1) {
                int tmp = errno;
                close(fd);
                errno = tmp;
                return -1;
            }
            return fd;
        }
        if (errno != EEXIST)
            break;
    }
    return -1;
}
#elif defined(__OpenBSD__)
/// Except OpenBSD which explicitly uses shm_mkstemp instead (as a more secure alternative)
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-XXXXXXXXXX";
    int fd;
    if ((fd = shm_mkstemp(name)) == -1)
        return -1;
    if (shm_unlink(name) == -1) {
        int tmp = errno;
        close(fd);
        errno = tmp;
        return -1;
    }
    return fd;
}
#endif

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
    {}

    bool Init() {
        long page_size = sysconf(_SC_PAGESIZE);
        ASSERT_MSG(page_size == 0x1000, "page size {:#x} is incompatible with 4K paging", page_size);
        // Backing memory initialization
#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__OpenBSD__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__FreeBSD__)
        fd = shm_open(SHM_ANON, O_RDWR, 0600);
#elif defined(__APPLE__) || defined(__managarm__)
        // macOS doesn't have memfd_create, use anonymous temporary file
        char template_path[] = "/tmp/eden_mem_XXXXXX";
        fd = mkstemp(template_path);
        if (fd >= 0) {
            unlink(template_path);
        }
#else
        fd = memfd_create("HostMemory", 0);
#endif
        bool use_anon = false;
        if (fd <= 0) {
            LOG_WARNING(Common_Memory, "memfd_create: {}", strerror(errno));
            use_anon = true;
        }
        if (!use_anon) {
            // Defined to extend the file with zeros
            int ret = ftruncate(fd, backing_size);
            if (ret != 0) {
                LOG_WARNING(Common_Memory, "ftruncate: {} (likely out-of-emory)", strerror(errno));
                use_anon = true;
            }
        }
        if (use_anon) {
            LOG_WARNING(Common_Memory, "Using private mappings instead of shared ones");
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NOCORE, -1, 0));
            if (fd > 0) {
                fd = -1;
                close(fd);
            }
        } else {
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOCORE, fd, 0));
        }
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }

        // Virtual memory initialization
        virtual_base = virtual_map_base = static_cast<u8*>(ChooseVirtualBase(virtual_size));
        if (virtual_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }
#if defined(__linux__)
        madvise(virtual_base, virtual_size, MADV_HUGEPAGE);
#endif
        free_manager.SetAddressSpace(virtual_base, virtual_size);
        return true;
    }

    ~Impl() {
        Release();
    }

    void* Allocate(size_t size) {
        auto* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (ptr == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, {}", size, strerror(errno));
        }
        return ptr;
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // We are removing a placeholder.
        free_manager.AllocateBlock(virtual_base + virtual_offset, length);

        // Deduce mapping protection flags.
        int prot_flags = PROT_NONE;
        if (True(perms & MemoryPermission::Read))
            prot_flags |= PROT_READ;
        if (True(perms & MemoryPermission::Write))
            prot_flags |= PROT_WRITE;
#ifdef ARCHITECTURE_arm64
        if (True(perms & MemoryPermission::Execute))
            prot_flags |= PROT_EXEC;
#endif
        int flags = (fd >= 0 ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED;
        void* ret = mmap(virtual_base + virtual_offset, length, prot_flags, flags, fd, host_offset);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {} {}", strerror(errno), fd);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        // The method name is wrong. We're still talking about the virtual range.
        // We don't want to unmap, we want to reserve this memory.

        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // Merge with any adjacent placeholder mappings.
        auto [merged_pointer, merged_size] =
            free_manager.FreeBlock(virtual_base + virtual_offset, length);

        void* ret = mmap(merged_pointer, merged_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {}", strerror(errno));
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        int flags = PROT_NONE;
        if (read) {
            flags |= PROT_READ;
        }
        if (write) {
            flags |= PROT_WRITE;
        }
#ifdef HAS_NCE
        if (execute) {
            flags |= PROT_EXEC;
        }
#endif
        int ret = mprotect(virtual_base + virtual_offset, length, flags);
        ASSERT_MSG(ret == 0, "mprotect failed: {}", strerror(errno));
    }

    void EnableDirectMappedAddress() {
        virtual_base = nullptr;
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_map_base{reinterpret_cast<u8*>(MAP_FAILED)};

private:
    /// Release all resources in the object
    void Release() {
        if (virtual_map_base != MAP_FAILED) {
            int ret = munmap(virtual_map_base, virtual_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (backing_base != MAP_FAILED) {
            int ret = munmap(backing_base, backing_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (fd != -1) {
            int ret = close(fd);
            ASSERT_MSG(ret == 0, "close failed: {}", strerror(errno));
        }
    }

    void AdjustMap(size_t* virtual_offset, size_t* length) {
        if (virtual_base != nullptr) {
            return;
        }

        // If we are direct mapped, we want to make sure we are operating on a region
        // that is in range of our virtual mapping.
        size_t intended_start = *virtual_offset;
        size_t intended_end = intended_start + *length;
        size_t address_space_start = reinterpret_cast<size_t>(virtual_map_base);
        size_t address_space_end = address_space_start + virtual_size;

        if (address_space_start > intended_end || intended_start > address_space_end) {
            *virtual_offset = 0;
            *length = 0;
        } else {
            *virtual_offset = (std::max)(intended_start, address_space_start);
            *length = (std::min)(intended_end, address_space_end) - *virtual_offset;
        }
    }

    int fd{-1}; // memfd file descriptor, -1 is the error value of memfd_create
    FreeRegionManager free_manager{};
};

#endif // ^^^ POSIX ^^^

HostMemory::HostMemory(size_t backing_size_, size_t virtual_size_)
    : backing_size(backing_size_)
    , virtual_size(virtual_size_)
{
#if defined(__OPENORBIS__) || defined(__managarm__)
    LOG_WARNING(HW_Memory, "Platform doesn't support fastmem");
    backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    virtual_base = nullptr;
#else
    // Try to allocate a fastmem arena.
    impl = std::make_unique<HostMemory::Impl>(AlignUp(backing_size, PageAlignment), AlignUp(virtual_size, PageAlignment) + HugePageSize);
    if (impl->Init()) {
        backing_base = impl->backing_base;
        virtual_base = impl->virtual_base;
        if (virtual_base) {
            // Ensure the virtual base is aligned to the L2 block size.
            virtual_base = reinterpret_cast<u8*>(Common::AlignUp(uintptr_t(virtual_base), HugePageSize));
            virtual_base_offset = virtual_base - impl->virtual_base;
        }
    } else {
        LOG_WARNING(HW_Memory, "Platform can support fastmem, but can't create it");
        fallback_buffer = true;
        backing_base = static_cast<u8*>(impl->Allocate(backing_size));
        virtual_base = nullptr;
        impl.reset();
    }
#endif
}

HostMemory::~HostMemory() {
#ifdef _WIN32
    if (fallback_buffer) {
        VirtualFree(backing_base, backing_size, MEM_RELEASE);
    }
#else
    if (fallback_buffer) {
        munmap(backing_base, backing_size);
    }
#endif
}

HostMemory::HostMemory(HostMemory&&) noexcept = default;

HostMemory& HostMemory::operator=(HostMemory&&) noexcept = default;

void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms, bool separate_heap) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(host_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    ASSERT(host_offset + length <= backing_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Map(virtual_offset + virtual_base_offset, host_offset, length, perms);
#endif
}

void HostMemory::Unmap(size_t virtual_offset, size_t length, bool separate_heap) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Unmap(virtual_offset + virtual_base_offset, length);
#endif
}

void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perm) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    const bool read = True(perm & MemoryPermission::Read);
    const bool write = True(perm & MemoryPermission::Write);
    const bool execute = True(perm & MemoryPermission::Execute);
    impl->Protect(virtual_offset + virtual_base_offset, length, read, write, execute);
#endif
}

void HostMemory::ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value) {
#ifdef HOST_MEMORY_USE_FROM_APP
    if (impl) {
#ifdef YUZU_UWP_APPCONTAINER
        std::optional<NxboxStall::Scope> stall_scope;
        if (fill_value == 0) {
            stall_scope.emplace(NxboxStall::Kind::Decommit);
        }
#endif
        impl->Clear(physical_offset, length, fill_value);
        return;
    }
#endif
    std::memset(backing_base + physical_offset, fill_value, length);
}

void HostMemory::EnableDirectMappedAddress() {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    if (impl) {
        impl->EnableDirectMappedAddress();
        virtual_size += reinterpret_cast<uintptr_t>(virtual_base);
    }
#endif
}

} // namespace Common
