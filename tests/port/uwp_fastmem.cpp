// SPDX-FileCopyrightText: Copyright 2026 NXbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include "common/demand_commit.h"

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using DWORD = std::uint32_t;
using LONG = long;
using HANDLE = void*;
constexpr DWORD PAGE_NOACCESS = 1, PAGE_READONLY = 2, PAGE_READWRITE = 4;
constexpr DWORD MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_RELEASE = 0x8000;
constexpr DWORD MEM_DECOMMIT = 0x4000, MEM_RESERVE_PLACEHOLDER = 0x40000;
constexpr DWORD MEM_REPLACE_PLACEHOLDER = 0x4000, MEM_PRESERVE_PLACEHOLDER = 2;
constexpr DWORD MEM_COALESCE_PLACEHOLDERS = 1, SEC_COMMIT = 0x8000000;
constexpr DWORD EXCEPTION_ACCESS_VIOLATION = 0xc0000005;
constexpr LONG EXCEPTION_CONTINUE_SEARCH = 0, EXCEPTION_CONTINUE_EXECUTION = -1;
#define NTAPI
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(-1)
#define UNREACHABLE() std::abort()
struct ApiError {
    std::string api;
    void* address;
    size_t size;
    DWORD protection;
    DWORD error;
};
std::vector<ApiError> api_errors;
void TestLog(const char*, const char*, const char* api, void* address, size_t size,
             DWORD protection, DWORD error) {
    api_errors.push_back({api, address, size, protection, error});
}
template <typename... T>
void TestLog(const char*, T&&...) {}
#define HW_Memory 0
#define LOG_INFO(category, ...) TestLog(__VA_ARGS__)
#define LOG_WARNING(category, ...) TestLog(__VA_ARGS__)
struct EXCEPTION_RECORD {
    DWORD ExceptionCode;
    unsigned NumberParameters;
    std::uintptr_t ExceptionInformation[2];
};
struct EXCEPTION_POINTERS {
    EXCEPTION_RECORD* ExceptionRecord;
};
struct MEMORY_BASIC_INFORMATION {
    size_t RegionSize;
};
using Handler = LONG (*)(EXCEPTION_POINTERS*);
using PFN_VirtualAllocFromApp = void* (*)(void*, size_t, DWORD, DWORD);
using PFN_AddVectoredExceptionHandler = void* (*)(unsigned, Handler);
using PFN_RemoveVectoredExceptionHandler = unsigned (*)(void*);
using PFN_CreateFileMappingFromApp = HANDLE (*)(HANDLE, void*, DWORD, std::uint64_t,
                                                const wchar_t*);
using PFN_VirtualAlloc2 = void* (*)(HANDLE, void*, size_t, DWORD, DWORD, void*, unsigned);
using PFN_MapViewOfFile3 = void* (*)(HANDLE, HANDLE, void*, std::uint64_t, size_t, DWORD, DWORD,
                                     void*, unsigned);
using PFN_UnmapViewOfFile2 = int (*)(HANDLE, void*, DWORD);
using PFN_VirtualProtect = int (*)(void*, size_t, DWORD, DWORD*);

HANDLE GetCurrentProcess() {
    return reinterpret_cast<HANDLE>(-2);
}

namespace Mock {
struct Section {
    int fd;
    size_t size;
    size_t views{};
};
struct Segment {
    size_t length;
    Section* section{};
    size_t offset{};
    DWORD mapping_protection{};
};
std::map<size_t, Segment> segments;
std::map<void*, size_t> reservations;
std::set<std::string> missing;
std::map<std::string, int> calls;
std::string fail_name;
int fail_at{};
size_t committed{}, sections{}, peak{};
bool force_noaccess_alias{};
DWORD last_error = 5;
Handler handler{};
bool Fail(const char* name) {
    const int call = ++calls[name];
    if (fail_name == name && call == fail_at) {
        last_error = 87;
        return true;
    }
    return false;
}
int Protection(DWORD flags) {
    return flags == PAGE_READWRITE  ? PROT_READ | PROT_WRITE
           : flags == PAGE_READONLY ? PROT_READ
                                    : PROT_NONE;
}
void Store(u8* address, u8 value) {
    auto source = segments.upper_bound(reinterpret_cast<size_t>(address));
    assert(source != segments.begin());
    --source;
    auto* section = source->second.section;
    assert(section);
    const size_t offset = reinterpret_cast<size_t>(address) - source->first + source->second.offset;
    for (const auto& [base, view] : segments) {
        if (view.section == section && offset >= view.offset && offset - view.offset < view.length)
            reinterpret_cast<u8*>(base)[offset - view.offset] = value;
    }
}
void Reset() {
    assert(reservations.empty() && segments.empty() && sections == 0 && committed == 0);
    missing.clear();
    calls.clear();
    fail_name.clear();
    fail_at = 0;
    peak = 0;
    last_error = 5;
    force_noaccess_alias = false;
    api_errors.clear();
    assert(!handler);
}
void* Reserve(HANDLE process, void* base, size_t size, DWORD type, DWORD flags, void*, unsigned) {
    assert(process == GetCurrentProcess() && !base &&
           type == (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER));
    assert(flags == PAGE_NOACCESS);
    if (Fail("reserve"))
        return nullptr;
    void* ptr = mmap(nullptr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(ptr != MAP_FAILED);
    reservations.emplace(ptr, size);
    segments.emplace(reinterpret_cast<size_t>(ptr), Segment{size});
    return ptr;
}
HANDLE Create(HANDLE file, void*, DWORD flags, std::uint64_t size, const wchar_t*) {
    assert(file == INVALID_HANDLE_VALUE && flags == (PAGE_READWRITE | SEC_COMMIT));
    assert(size <= 65536); // Never create a guest-sized committed section.
    if (Fail("create"))
        return nullptr;
    char path[] = "/tmp/nxbox-section-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    assert(ftruncate(fd, size) == 0);
    committed += size;
    ++sections;
    peak = std::max(peak, committed);
    return new Section{fd, static_cast<size_t>(size)};
}
void* Map(HANDLE handle, HANDLE process, void* base, std::uint64_t offset, size_t length,
          DWORD type, DWORD flags, void*, unsigned) {
    assert(process == GetCurrentProcess() && type == MEM_REPLACE_PLACEHOLDER &&
           (flags == PAGE_READWRITE || flags == PAGE_READONLY || flags == PAGE_NOACCESS));
    if (force_noaccess_alias && offset == 4096)
        flags = PAGE_NOACCESS;
    auto* section = static_cast<Section*>(handle);
    assert(offset % 4096 == 0 && offset + length <= section->size);
    auto it = segments.find(reinterpret_cast<size_t>(base));
    assert(it != segments.end() && !it->second.section && it->second.length == length);
    if (Fail("map"))
        return nullptr;
    // macOS/arm64 has 16 KiB host pages. Native aligned views are real shared
    // mappings; subpage Windows views are emulated while retaining strict bounds.
    const size_t host_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    if (reinterpret_cast<size_t>(base) % host_page == 0 && offset % host_page == 0 &&
        length % host_page == 0) {
        assert(mmap(base, length, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, section->fd,
                    offset) == base);
    } else {
        const size_t low = reinterpret_cast<size_t>(base) & ~(host_page - 1);
        const size_t high =
            (reinterpret_cast<size_t>(base) + length + host_page - 1) & ~(host_page - 1);
        assert(mprotect(reinterpret_cast<void*>(low), high - low, PROT_READ | PROT_WRITE) == 0);
        std::vector<u8> copy(length);
        assert(pread(section->fd, copy.data(), length, offset) == static_cast<ssize_t>(length));
        std::memcpy(base, copy.data(), length);
    }
    it->second.section = section;
    it->second.offset = offset;
    it->second.mapping_protection = flags;
    ++section->views;
    return base;
}
int Unmap(HANDLE process, void* base, DWORD flags) {
    assert(process == GetCurrentProcess() && flags == MEM_PRESERVE_PLACEHOLDER);
    auto it = segments.find(reinterpret_cast<size_t>(base));
    assert(it != segments.end() && it->second.section);
    if (Fail("unmap"))
        return false;
    --it->second.section->views;
    it->second.section = nullptr;
    return true;
}
int Protect(void* base, size_t size, DWORD flags, DWORD* old) {
    if (Fail("protect"))
        return false;
    const size_t address = reinterpret_cast<size_t>(base);
    auto it = segments.upper_bound(address);
    if (it == segments.begin()) {
        last_error = 87;
        return false;
    }
    --it;
    const auto& view = it->second;
    // FromApp cannot widen beyond the access granted at mapping time, or span
    // views.
    if (!view.section || size > view.length || address - it->first > view.length - size ||
        (Protection(flags) & ~Protection(view.mapping_protection)) != 0) {
        last_error = 87;
        return false;
    }
    *old = view.mapping_protection;
    return true; // Host 16 KiB pages cannot enforce individual Windows 4 KiB
                 // permissions.
}

void* PrivateAlloc(void* base, size_t size, DWORD flags, DWORD protection) {
    assert(protection == PAGE_READWRITE);
    if (Fail("private"))
        return nullptr;
    if (flags == MEM_RESERVE) {
        assert(!base);
        void* ptr = mmap(nullptr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
        assert(ptr != MAP_FAILED);
        reservations.emplace(ptr, size);
        segments.emplace(reinterpret_cast<size_t>(ptr), Segment{size});
        return ptr;
    }
    assert(flags == MEM_COMMIT && base);
    assert(mprotect(base, size, PROT_READ | PROT_WRITE) == 0);
    return base;
}
void* Add(unsigned first, Handler cb) {
    assert(first == 1 && !handler);
    if (Fail("veh"))
        return nullptr;
    handler = cb;
    return reinterpret_cast<void*>(1);
}
unsigned Remove(void* token) {
    assert(token && handler);
    handler = nullptr;
    return 1;
}
} // namespace Mock

DWORD GetLastError() {
    return Mock::last_error;
}
int CloseHandle(HANDLE handle) {
    auto* section = static_cast<Mock::Section*>(handle);
    assert(section && section->views == 0);
    close(section->fd);
    Mock::committed -= section->size;
    --Mock::sections;
    delete section;
    return true;
}
int VirtualFree(void* base, size_t size, DWORD flags) {
    using namespace Mock;
    const size_t address = reinterpret_cast<size_t>(base);
    if (flags == MEM_RELEASE) {
        assert(size == 0);
        auto owner = reservations.find(base);
        assert(owner != reservations.end());
        auto it = segments.find(address);
        assert(it != segments.end() && it->second.length == owner->second && !it->second.section);
        munmap(base, owner->second);
        segments.erase(it);
        reservations.erase(owner);
        return true;
    }
    if (flags == MEM_DECOMMIT) {
        assert(mmap(base, size, PROT_NONE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) == base);
        return true;
    }
    if (Fail("free"))
        return false;
    if (flags == (MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        auto it = segments.upper_bound(address);
        assert(it != segments.begin());
        --it;
        const size_t start = it->first, finish = start + it->second.length;
        assert(!it->second.section && address >= start && address + size <= finish);
        // Windows splitting requires one boundary to match the placeholder.
        assert(start == address || address + size == finish);
        segments.erase(it);
        if (start < address)
            segments.emplace(start, Segment{address - start});
        segments.emplace(address, Segment{size});
        if (address + size < finish)
            segments.emplace(address + size, Segment{finish - address - size});
        return true;
    }
    assert(flags == (MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS));
    auto begin = segments.find(address);
    assert(begin != segments.end());
    auto end = begin;
    size_t cursor = address;
    while (cursor < address + size) {
        assert(end != segments.end() && end->first == cursor && !end->second.section);
        cursor += end->second.length;
        ++end;
    }
    assert(cursor == address + size);
    segments.erase(begin, end);
    segments.emplace(address, Segment{size});
    return true;
}
size_t VirtualQuery(void* base, MEMORY_BASIC_INFORMATION* out, size_t) {
    auto it = Mock::segments.find(reinterpret_cast<size_t>(base));
    if (it == Mock::segments.end())
        return 0;
    out->RegionSize = it->second.length;
    return sizeof(*out);
}

namespace Common {
constexpr size_t PageAlignment = 4096;
enum class MemoryPermission : u32 { Read = 1, Write = 2, ReadWrite = 3 };
MemoryPermission operator&(MemoryPermission a, MemoryPermission b) {
    return static_cast<MemoryPermission>(static_cast<u32>(a) & static_cast<u32>(b));
}
bool True(MemoryPermission p) {
    return static_cast<u32>(p) != 0;
}
class DynamicLibrary {
public:
    explicit DynamicLibrary(const char*) {}
};
template <typename T, typename U>
void Assign(T& target, U value) {
    if constexpr (std::is_same_v<T, U>)
        target = value;
    else
        std::abort();
}
template <typename T>
void GetFuncAddress(DynamicLibrary&, const char* name, T& pointer) {
    if (Mock::missing.contains(name)) {
        pointer = nullptr;
        return;
    }
    if (std::string_view{name} == "CreateFileMappingFromApp")
        Assign(pointer, &Mock::Create);
    else if (std::string_view{name} == "VirtualAlloc2FromApp")
        Assign(pointer, &Mock::Reserve);
    else if (std::string_view{name} == "MapViewOfFile3FromApp")
        Assign(pointer, &Mock::Map);
    else if (std::string_view{name} == "UnmapViewOfFile2")
        Assign(pointer, &Mock::Unmap);
    else if (std::string_view{name} == "VirtualProtectFromApp")
        Assign(pointer, &Mock::Protect);
    else if (std::string_view{name} == "VirtualAllocFromApp")
        Assign(pointer, &Mock::PrivateAlloc);
    else if (std::string_view{name} == "AddVectoredExceptionHandler")
        Assign(pointer, &Mock::Add);
    else if (std::string_view{name} == "RemoveVectoredExceptionHandler")
        Assign(pointer, &Mock::Remove);
    else
        std::abort();
}
class HostMemory {
public:
    class Impl;
};
// PRODUCTION_HANDLER
// PRODUCTION_ALLOCATOR
} // namespace Common

bool Fault(void* address, size_t access = 1) {
    EXCEPTION_RECORD record{
        EXCEPTION_ACCESS_VIOLATION, 2, {access, reinterpret_cast<size_t>(address)}};
    EXCEPTION_POINTERS ep{&record};
    assert(Mock::handler);
    const bool handled = Mock::handler(&ep) == EXCEPTION_CONTINUE_EXECUTION;
    if (handled) {
        auto it = Mock::segments.upper_bound(reinterpret_cast<size_t>(address));
        assert(it != Mock::segments.begin());
        const auto& view = std::prev(it)->second;
        if (view.section) {
            assert(view.mapping_protection != PAGE_NOACCESS);
            assert(access != 1 || view.mapping_protection == PAGE_READWRITE);
        }
    }
    return handled;
}

int main() { setenv("NXBOX_FASTMEM", "1", 1); // UWP fastmem is opt-in
    using Common::g_backing_committed;
    using Common::HostMemory;
    using Common::MemoryPermission;
    constexpr size_t region = 65536;
    Mock::Reset();
    // Verify the strict mock reproduces the Xbox error for the original probe
    // pattern.
    {
        void* base =
            Mock::Reserve(GetCurrentProcess(), nullptr, region,
                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        auto handle = Mock::Create(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                   region, nullptr);
        assert(Mock::Map(handle, GetCurrentProcess(), base, 0, region, MEM_REPLACE_PLACEHOLDER,
                         PAGE_NOACCESS, nullptr, 0) == base);
        DWORD old{};
        assert(!Mock::Protect(base, 4096, PAGE_READWRITE, &old) && GetLastError() == 87);
        assert(Mock::Unmap(GetCurrentProcess(), base, MEM_PRESERVE_PLACEHOLDER));
        CloseHandle(handle);
        assert(VirtualFree(base, 0, MEM_RELEASE));
    }
    Mock::Reset();
    {
        HostMemory::Impl memory(4 * region, 32 * region);
        assert(memory.Init() && memory.virtual_base);
        assert(Mock::committed == 0 && Mock::peak == region && g_backing_committed == 0);
        auto* backing = memory.backing_base;
        auto* arena = memory.virtual_base;
        // Neither mapping nor zeroing an untouched whole heap commits it.
        memory.Map(0, 0, 4 * region, MemoryPermission::ReadWrite);
        memory.Map(8 * region, 0, region, MemoryPermission::ReadWrite);
        memory.Clear(0, 4 * region, 0);
        assert(Mock::committed == 0);
        assert(Fault(arena + 4096));
        Mock::Store(arena + 4096, 71);
        assert(backing[4096] == 71 && Mock::committed == region);
        assert(Fault(arena + 8 * region + 4096));
        assert(arena[8 * region + 4096] == 71);
        assert(Fault(backing + 4096));      // Concurrent demand fault already satisfied.
        assert(!Fault(arena + 4096, 8));    // Execute fault must propagate.
        assert(!Fault(arena + 7 * region)); // Guest hole must reach Dynarmic SEH.
        assert(!Fault(arena + 32 * region));
        // Partial clear preserves neighboring bytes and mirrors.
        Mock::Store(backing + 8192, 42);
        memory.Clear(4096, 4096, 0);
        assert(backing[4096] == 0 && backing[8192] == 42 && arena[8192] == 42);
        memory.Protect(8192, 4096, true, false, false);
        memory.Clear(0, region, 0);
        assert(Mock::committed == 0 && g_backing_committed == 0);
        assert(!Fault(arena + 8192)); // Write to read-only page is not demand commit.
        assert(Fault(arena + 8192, 0));
        assert(arena[8192] == 0);
        assert(Fault(arena + 8 * region));
        assert(arena[8 * region + 8192] == 0);
        // Partial unmap splits records and shifts physical offsets/protection metadata.
        memory.Unmap(4096, 4096);
        assert(!Fault(arena + 4096));
        assert(Fault(arena));
        assert(!Fault(arena + 8192));
        assert(Fault(arena + 8192, 0));
        memory.Map(4096, region + 12288, 4096, MemoryPermission::ReadWrite);
        assert(Fault(arena + 4096));
        Mock::Store(arena + 4096, 99);
        assert(backing[region + 12288] == 99);
        // Clear crossing boundaries must not destroy bytes outside the range.
        memory.Clear(region - 4096, 8192, 0);
        assert(backing[region + 12288] == 99);
        memory.Clear(0, 4 * region, 0);
        assert(Mock::committed == 0 && g_backing_committed == 0);
        memory.Unmap(0, 32 * region);
        assert(!Fault(arena));
        memory.Map(0, 4096, 4096, MemoryPermission::ReadWrite);
        memory.Map(8192, region, 4096, MemoryPermission::Read);
        memory.Protect(0, 12288, false, false, false);
        assert(!Fault(arena, 0) && !Fault(arena + 4096, 0) && !Fault(arena + 8192, 0));
        memory.Protect(0, 12288, true, true, false);
        assert(Fault(arena) && !Fault(arena + 4096) && Fault(arena + 8192));
        memory.Unmap(0, 32 * region);
        memory.Clear(0, 4 * region, 0);
        // Nonzero fill retains byte semantics of the previous memset path.
        memory.Clear(region, 3 * region, 0x1234);
        assert(Mock::committed == 3 * region);
        assert(backing[region] == 0x34 && backing[4 * region - 1] == 0x34);
    }
    Mock::Reset();
    // Every optional export missing must select the private demand-backed fallback.
    for (const char* symbol :
         {"CreateFileMappingFromApp", "VirtualAlloc2FromApp", "MapViewOfFile3FromApp",
          "UnmapViewOfFile2", "VirtualProtectFromApp"}) {
        Mock::missing.insert(symbol);
        {
            HostMemory::Impl memory(4 * region, 32 * region);
            assert(memory.Init() && !memory.virtual_base);
            assert(Fault(memory.backing_base));
            memory.backing_base[0] = 1;
            memory.Clear(0, region, 0);
            assert(Fault(memory.backing_base));
            assert(memory.backing_base[0] == 0);
        }
        Mock::Reset();
    }
    // Every individual preflight API call can fail without leaking reservations/commit.
    for (const char* api : {"reserve", "create", "map", "protect", "free", "unmap", "veh"}) {
        // First collect the exact successful preflight call count.
        int count;
        {
            HostMemory::Impl memory(4 * region, 32 * region);
            assert(memory.Init());
            count = Mock::calls[api];
        }
        Mock::Reset();
        for (int failure = 1; failure <= count; ++failure) {
            Mock::fail_name = api;
            Mock::fail_at = failure;
            {
                HostMemory::Impl memory(4 * region, 32 * region);
                assert(memory.Init() && !memory.virtual_base);
                assert(Mock::committed == 0);
                if (std::string_view{api} == "protect") {
                    assert(!api_errors.empty());
                    const auto& failure_log = api_errors.back();
                    assert(failure_log.api == "VirtualProtectFromApp");
                    assert(failure_log.address && failure_log.size == 4096);
                    assert(failure_log.protection ==
                           (failure == 1 ? PAGE_READONLY : PAGE_READWRITE));
                    assert(failure_log.error == 87);
                }
                assert(Fault(memory.backing_base));
            }
            Mock::Reset();
        }
    }
    for (const char* api : {"create", "map"}) {
        {
            HostMemory::Impl memory(4 * region, 32 * region);
            assert(memory.Init());
            memory.Map(0, 4096, 8192, MemoryPermission::ReadWrite);
            Mock::fail_name = api;
            // For map, allow the backing view and fail the alias view.
            Mock::fail_at = Mock::calls[api] + (std::string_view{api} == "map" ? 2 : 1);
            assert(!Fault(memory.virtual_base));
            // A failed alias operation leaves page-table access to the backing
            // available.
            assert(Fault(memory.backing_base + 4096));
            // Failed alias APIs disable this view; a new guest mapping can try
            // again.
            if (std::string_view{api} != "create")
                assert(!Fault(memory.virtual_base));
            memory.Map(0, 4096, 8192, MemoryPermission::ReadWrite);
            assert(Fault(memory.virtual_base));
            Mock::Store(memory.virtual_base, 33);
            assert(memory.backing_base[4096] == 33);
            memory.Protect(0, 8192, true, false, false);
            assert(!Fault(memory.virtual_base));
            assert(!Fault(memory.virtual_base + 4096));
            assert(Fault(memory.virtual_base, 0));
            assert(memory.virtual_base[0] == 33);
            // A RO view must be remapped before a later permission upgrade.
            const int maps = Mock::calls["map"];
            memory.Protect(0, 4096, true, true, false);
            assert(Fault(memory.virtual_base));
            assert(Mock::calls["map"] > maps);
            assert(!Fault(memory.virtual_base + 4096));
            assert(Fault(memory.virtual_base + 4096, 0));
            assert(Mock::calls["protect"] == 2); // Only compatible probe transitions.
        }
        Mock::Reset();
    }
    {
        HostMemory::Impl memory(4 * region, 32 * region);
        assert(memory.Init());
        memory.Map(0, 0, region, MemoryPermission::ReadWrite);
        std::vector<std::thread> workers;
        for (unsigned core = 0; core < 8; ++core) {
            workers.emplace_back([&] {
                for (int i = 0; i < 100; ++i)
                    assert(Fault(memory.virtual_base));
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(Mock::committed == region && g_backing_committed == region);
    }
    Mock::Reset();
    for (const char* symbol :
         {"VirtualAllocFromApp", "AddVectoredExceptionHandler", "RemoveVectoredExceptionHandler"}) {
        Mock::missing.insert(symbol);
        bool threw = false;
        try {
            HostMemory::Impl memory(4 * region, 32 * region);
            memory.Init();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        assert(threw);
        Mock::Reset();
    }
    setenv("NXBOX_FASTMEM", "0", 1);
    {
        HostMemory::Impl memory(4 * region, 32 * region);
        assert(memory.Init() && !memory.virtual_base && Mock::calls["reserve"] == 0);
    }
    unsetenv("NXBOX_FASTMEM");
    Mock::Reset();
    std::cout << "UWP fastmem allocator checks passed\n";
}
