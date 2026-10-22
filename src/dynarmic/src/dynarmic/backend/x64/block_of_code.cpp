// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/block_of_code.h"

#if __has_include("../../../../../common/nxbox_stall.h")
#    include "../../../../../common/nxbox_stall.h"
#endif

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
// On the Xbox/UWP AppContainer the plain Virtual* APIs are unavailable for JIT memory; the
// sandbox-legal *FromApp variants must be used instead, and emitting executable pages additionally
// requires the `codeGeneration` restricted capability in the package manifest. The function shapes
// are identical, so we just select the symbol. RWX is never grantable under the AppContainer, so
// DYNARMIC_UWP_APPCONTAINER always implies DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT (W^X) — see CMakeLists.
#    if defined(DYNARMIC_UWP_APPCONTAINER)
#        define DYNARMIC_VIRTUAL_ALLOC   VirtualAllocFromApp
#        define DYNARMIC_VIRTUAL_PROTECT VirtualProtectFromApp
#    else
#        define DYNARMIC_VIRTUAL_ALLOC   VirtualAlloc
#        define DYNARMIC_VIRTUAL_PROTECT VirtualProtect
#    endif
#else
#    include <sys/mman.h>
#endif

#ifdef __APPLE__
#    include <errno.h>
#    include <fmt/format.h>
#    include <sys/sysctl.h>
#endif

#include <array>
#include <cstdlib>
#include <cstring>
#ifdef DYNARMIC_UWP_APPCONTAINER
#    include <mutex>
#    include <string>
#    include <unordered_map>
#endif

#include "common/assert.h"
#include "dynarmic/mcl/bit.hpp"
#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/a32_jitstate.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/hostloc.h"
#include "dynarmic/backend/x64/perf_map.h"
#include "dynarmic/backend/x64/stack_layout.h"

namespace Dynarmic::Backend::X64 {

const Xbyak::Reg64 BlockOfCode::ABI_JIT_PTR = HostLocToReg64(Dynarmic::Backend::X64::ABI_JIT_PTR);
#ifdef _WIN32
const Xbyak::Reg64 BlockOfCode::ABI_RETURN = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM1 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM1);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM3 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM3);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM4 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM4);
const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> BlockOfCode::ABI_PARAMS = {BlockOfCode::ABI_PARAM1, BlockOfCode::ABI_PARAM2, BlockOfCode::ABI_PARAM3, BlockOfCode::ABI_PARAM4};
#else
const Xbyak::Reg64 BlockOfCode::ABI_RETURN = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN);
const Xbyak::Reg64 BlockOfCode::ABI_RETURN2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM1 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM1);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM3 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM3);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM4 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM4);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM5 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM5);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM6 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM6);
const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> BlockOfCode::ABI_PARAMS = {BlockOfCode::ABI_PARAM1, BlockOfCode::ABI_PARAM2, BlockOfCode::ABI_PARAM3, BlockOfCode::ABI_PARAM4, BlockOfCode::ABI_PARAM5, BlockOfCode::ABI_PARAM6};
#endif

namespace {

constexpr size_t CONSTANT_POOL_SIZE = 2 * 1024 * 1024;
constexpr size_t PRELUDE_COMMIT_SIZE = 16 * 1024 * 1024;

#ifdef DYNARMIC_UWP_APPCONTAINER
size_t JitPageSize() {
    static const size_t page_size = [] {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return static_cast<size_t>(info.dwPageSize);
    }();
    return page_size;
}
#endif

#ifdef DYNARMIC_UWP_APPCONTAINER
// Upstream now uses Xbyak's default allocator (heap memory) and flips protections on it. Under the
// Xbox/UWP AppContainer JIT pages must instead be reserved with VirtualAllocFromApp so that
// VirtualProtectFromApp (codeGeneration capability) may later make them executable, so the port
// keeps a dedicated allocator for that target only.
//
// By default (NXBOX_JIT_DUALMAP=0 turns it off; read once) the cache is instead a pagefile-backed section mapped twice: the RW view is
// what Xbyak stores to (top_), the RX view is what the CPU executes and what every code address refers
// to. Reserve the section and commit its RW view as emission grows. RX aliases share that
// commitment; do not charge it twice. Any failure uses the demand-committed W^X fallback.
bool DualMapRequested() {
    static const bool requested = [] {
        const char* value = std::getenv("NXBOX_JIT_DUALMAP");
        // On by default since the 0.3.698 console run (SM3DW: JIT 8.5 s -> 4.6 s per run); "0" opts out.
        return value == nullptr || !(value[0] == '0' && value[1] == '\0');
    }();
    return requested;
}

struct DualMapViews {
    HANDLE section;
    void* rx;
    size_t committed;
};
std::mutex dualmap_mutex;
std::unordered_map<const uint8_t*, DualMapViews> dualmap_views;  // keyed by the RW view base
// Why the most recent alloc() on this thread did not produce a dual mapping (read right after the
// Xbyak base constructor returns, on the same thread).
thread_local std::string dualmap_reason;

using MapViewOfFile3FromAppFn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                               MEM_EXTENDED_PARAMETER*, ULONG);

std::string DualMapError(const char* what) {
    return std::string{what} + ":" + std::to_string(GetLastError());
}

uint8_t* TryAllocDualMap(size_t size) {
    dualmap_reason.clear();
    if (!DualMapRequested()) {
        dualmap_reason = "flag_off";
        return nullptr;
    }
#ifndef XBYAK_NXBOX_EXEC_DELTA
    dualmap_reason = "xbyak_unpatched";
    return nullptr;
#else
    HANDLE section = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | (NxboxMemory::JitEagerCommit() ? 0 : SEC_RESERVE),
                                              static_cast<ULONG64>(size), nullptr);
    if (section == nullptr) {
        dualmap_reason = DualMapError("create_section_failed");
        return nullptr;
    }
    void* rw = MapViewOfFileFromApp(section, FILE_MAP_WRITE, 0, size);
    if (rw == nullptr) {
        dualmap_reason = DualMapError("map_rw_failed");
        CloseHandle(section);
        return nullptr;
    }
    const size_t initial_commit = NxboxMemory::JitEagerCommit() ? size : std::min(size, PRELUDE_COMMIT_SIZE);
    if (!NxboxMemory::JitEagerCommit() && !DYNARMIC_VIRTUAL_ALLOC(rw, initial_commit, MEM_COMMIT, PAGE_READWRITE)) {
        dualmap_reason = DualMapError("commit_rw_failed");
        UnmapViewOfFile(rw);
        CloseHandle(section);
        return nullptr;
    }
    void* rx = nullptr;
    if (const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll")) {
        if (const auto map3 = reinterpret_cast<MapViewOfFile3FromAppFn>(
                GetProcAddress(kernelbase, "MapViewOfFile3FromApp"))) {
            rx = map3(section, GetCurrentProcess(), nullptr, 0, 0, 0, PAGE_EXECUTE_READ, nullptr, 0);
        }
    }
    if (rx == nullptr) {
        rx = MapViewOfFileFromApp(section, FILE_MAP_EXECUTE | FILE_MAP_READ, 0, size);
    }
    if (rx == nullptr) {
        dualmap_reason = DualMapError("map_rx_failed");
        UnmapViewOfFile(rw);
        CloseHandle(section);
        return nullptr;
    }
    const std::lock_guard lock{dualmap_mutex};
    dualmap_views[static_cast<uint8_t*>(rw)] = DualMapViews{section, rx, initial_commit};
    return static_cast<uint8_t*>(rw);
#endif
}

// The executable view for an RW base returned by TryAllocDualMap, or nullptr.
[[maybe_unused]] void* DualMapExecView(const uint8_t* rw, size_t& committed) {
    const std::lock_guard lock{dualmap_mutex};
    const auto it = dualmap_views.find(rw);
    if (it == dualmap_views.end())
        return nullptr;
    committed = it->second.committed;
    return it->second.rx;
}

bool FreeDualMap(uint8_t* rw) {
    DualMapViews views{};
    {
        const std::lock_guard lock{dualmap_mutex};
        const auto it = dualmap_views.find(rw);
        if (it == dualmap_views.end()) {
            return false;
        }
        views = it->second;
        dualmap_views.erase(it);
    }
    UnmapViewOfFile(views.rx);
    UnmapViewOfFile(rw);
    CloseHandle(views.section);
    return true;
}

class UwpXbyakAllocator : public Xbyak::Allocator {
public:
    uint8_t* alloc(size_t size) override {
        if (uint8_t* mapped = TryAllocDualMap(size)) {
            return mapped;
        }
        void* p = DYNARMIC_VIRTUAL_ALLOC(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
        if (p == nullptr) {
            using Xbyak::Error;
            XBYAK_THROW(Xbyak::ERR_CANT_ALLOC);
        }
        return static_cast<uint8_t*>(p);
    }

    void free(uint8_t* p) override {
        if (FreeDualMap(p)) {
            return;
        }
        VirtualFree(static_cast<void*>(p), 0, MEM_RELEASE);
    }

    bool useProtect() const override { return false; }
};

// This is threadsafe as Xbyak::Allocator does not contain any state; it is a pure interface.
UwpXbyakAllocator s_allocator;
#endif

#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
void ProtectMemory(const void* base, size_t size, bool is_executable) {
#    if NXBOX_STALL_PROFILE
    const NxboxStall::Scope scope{NxboxStall::Kind::JitProtect};
    NxboxStall::AddJit(NxboxStall::JitEvent::ProtectBytes, size);
#    endif
#    ifdef _WIN32
    DWORD oldProtect = 0;
    // The is_executable→PAGE_EXECUTE_READ transition is the call that requires the `codeGeneration`
    // capability under the AppContainer (VirtualProtectFromApp); the W^X invariant means we only ever
    // hold RW or RX, never RWX.
#        ifdef DYNARMIC_UWP_APPCONTAINER
    if (!DYNARMIC_VIRTUAL_PROTECT(const_cast<void*>(base), size, is_executable ? PAGE_EXECUTE_READ : PAGE_READWRITE, &oldProtect)) {
        UNREACHABLE_MSG("NXBOX JIT protection failed: error={}", GetLastError());
    }
    if (is_executable && !FlushInstructionCache(GetCurrentProcess(), base, size)) {
        UNREACHABLE_MSG("NXBOX JIT instruction cache flush failed: error={}", GetLastError());
    }
#        else
    DYNARMIC_VIRTUAL_PROTECT(const_cast<void*>(base), size, is_executable ? PAGE_EXECUTE_READ : PAGE_READWRITE, &oldProtect);
#        endif
#    else
    static const size_t pageSize = sysconf(_SC_PAGESIZE);
    const size_t iaddr = reinterpret_cast<size_t>(base);
    const size_t roundAddr = iaddr & ~(pageSize - static_cast<size_t>(1));
    const int mode = is_executable ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE);
    mprotect(reinterpret_cast<void*>(roundAddr), size + (iaddr - roundAddr), mode);
#    endif
}
#endif

static const HostFeature features = []() {
    HostFeature f{};
#ifdef DYNARMIC_ENABLE_CPU_FEATURE_DETECTION
    using Cpu = Xbyak::util::Cpu;
    Xbyak::util::Cpu cpu_info{};
    if (cpu_info.has(Cpu::tSSSE3)) f |= HostFeature::SSSE3;
    if (cpu_info.has(Cpu::tSSE41)) f |= HostFeature::SSE41;
    if (cpu_info.has(Cpu::tSSE42)) f |= HostFeature::SSE42;
    if (cpu_info.has(Cpu::tAVX)) f |= HostFeature::AVX;
    if (cpu_info.has(Cpu::tAVX2)) f |= HostFeature::AVX2;
    if (cpu_info.has(Cpu::tAVX512F)) f |= HostFeature::AVX512F;
    if (cpu_info.has(Cpu::tAVX512CD)) f |= HostFeature::AVX512CD;
    if (cpu_info.has(Cpu::tAVX512VL)) f |= HostFeature::AVX512VL;
    if (cpu_info.has(Cpu::tAVX512BW)) f |= HostFeature::AVX512BW;
    if (cpu_info.has(Cpu::tAVX512DQ)) f |= HostFeature::AVX512DQ;
    if (cpu_info.has(Cpu::tAVX512_BITALG)) f |= HostFeature::AVX512BITALG;
    if (cpu_info.has(Cpu::tAVX512VBMI)) f |= HostFeature::AVX512VBMI;
    if (cpu_info.has(Cpu::tPCLMULQDQ)) f |= HostFeature::PCLMULQDQ;
    if (cpu_info.has(Cpu::tF16C)) f |= HostFeature::F16C;
    if (cpu_info.has(Cpu::tFMA)) f |= HostFeature::FMA;
    if (cpu_info.has(Cpu::tAESNI)) f |= HostFeature::AES;
    if (cpu_info.has(Cpu::tSHA)) f |= HostFeature::SHA;
    if (cpu_info.has(Cpu::tPOPCNT)) f |= HostFeature::POPCNT;
    if (cpu_info.has(Cpu::tBMI1)) f |= HostFeature::BMI1;
    if (cpu_info.has(Cpu::tBMI2)) f |= HostFeature::BMI2;
    if (cpu_info.has(Cpu::tLZCNT)) f |= HostFeature::LZCNT;
    if (cpu_info.has(Cpu::tGFNI)) f |= HostFeature::GFNI;
    if (cpu_info.has(Cpu::tWAITPKG)) f |= HostFeature::WAITPKG;
    if (cpu_info.has(Cpu::tBMI2)) {
        // BMI2 instructions such as pdep and pext have been very slow up until Zen 3.
        // Check for Zen 3 or newer by its family (0x19).
        // See also: https://en.wikichip.org/wiki/amd/cpuid
        if (cpu_info.has(Cpu::tAMD)) {
            std::array<u32, 4> data{};
            cpu_info.getCpuid(1, data.data());
            const u32 family_base = mcl::bit::get_bits<8, 11>(data[0]);
            const u32 family_extended = mcl::bit::get_bits<20, 27>(data[0]);
            const u32 family = family_base + family_extended;
            if (family >= 0x19)
                f |= HostFeature::FastBMI2;
        } else {
            f |= HostFeature::FastBMI2;
        }
    }
    return f;
#endif
}();
HostFeature GetHostFeatures() {
    return features;
}

#ifdef __APPLE__
bool IsUnderRosetta() {
    int result = 0;
    size_t result_size = sizeof(result);
    if (sysctlbyname("sysctl.proc_translated", &result, &result_size, nullptr, 0) == -1) {
        if (errno != ENOENT)
            fmt::print("IsUnderRosetta: Failed to detect Rosetta state, assuming not under Rosetta");
        return false;
    }
    return result != 0;
}
#endif

}  // anonymous namespace

#ifdef DYNARMIC_UWP_APPCONTAINER
// Runs as a default member initializer, i.e. after the Xbyak base (and its allocation) exists and before
// constant_pool is constructed. Installs the RW->RX delta so that, from the very first emitted byte,
// getCode()/getCurr() are executable addresses while stores keep going to the RW view.
bool BlockOfCode::ConfigureWx() {
    reserved_memory.Set(maxSize_);
    bool mapped = false;
    std::string reason;
#ifdef XBYAK_NXBOX_EXEC_DELTA
    if (const void* rx = DualMapExecView(top_, committed_size)) {
        setExecDelta(static_cast<const u8*>(rx) - top_);
        code_memory.Set(committed_size);
        mapped = true;
        reason = NxboxMemory::JitEagerCommit() ? "eager_commit" : "demand_commit";
    }
#endif
    if (!mapped) {
        reason = dualmap_reason.empty() ? "unknown" : dualmap_reason;
    }
    LOG_INFO(Core_ARM, "NXBOX JIT_WX mode={} reason={}", mapped ? "dualmap" : "protect", reason);
    return mapped;
}
#endif

BlockOfCode::BlockOfCode(RunCodeCallbacks cb, JitStateInfo jsi, size_t total_code_size, std::function<void(BlockOfCode&)> rcp)
    : Xbyak::CodeGenerator(total_code_size
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
        , Xbyak::DontSetProtectRWE
#else
        , nullptr //Allow RWE
#endif
#ifdef DYNARMIC_UWP_APPCONTAINER
        , &s_allocator)
#else
        , nullptr)
#endif
    , constant_pool(*this, CONSTANT_POOL_SIZE)
    , jsi(jsi)
    , cb(std::move(cb))
{
#ifdef DYNARMIC_UWP_APPCONTAINER
    static const bool logged = [] {
        LOG_INFO(Core_ARM, "NXBOX JIT_MODE wx protection=page-granular");
        return true;
    }();
    (void)logged;
#endif
    EnableWriting();
#ifdef DYNARMIC_UWP_APPCONTAINER
    EnsureMemoryCommitted(PRELUDE_COMMIT_SIZE - size_);
#else
    EnsureMemoryCommitted(PRELUDE_COMMIT_SIZE);
#endif
    GenRunCode(rcp);
}

bool BlockOfCode::HasHostFeature(HostFeature feature) const noexcept {
    return (GetHostFeatures() & feature) == feature;
}

void BlockOfCode::PreludeComplete() {
#ifdef DYNARMIC_UWP_APPCONTAINER
    // Cache rewinds and incoming block patches must never make the dispatcher NX.
    EnsureMemoryCommitted(JitPageSize());
    align(JitPageSize());
#endif
    prelude_complete = true;
    code_begin = getCurr();
    DisableWriting();
}

void BlockOfCode::EnableWriting() {
#ifdef DYNARMIC_UWP_APPCONTAINER
    if (writing)
        return;
    writing = true;
    append_begin = size_;
    if (dual_map)
        return;  // The RW view is always writable; nothing to reopen.
    // The unused suffix is already RW. Only the shared tail page can be RX.
    if (size_ < executable_end)
        PrepareWriteRange(getCurr(), 1);
#elif defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
#    ifdef _WIN32
    ProtectMemory(getCode(), committed_size, false);
#    else
    ProtectMemory(getCode(), maxSize_, false);
#    endif
#endif
}

void BlockOfCode::DisableWriting() {
#ifdef DYNARMIC_UWP_APPCONTAINER
    if (!writing)
        return;
    if (dual_map) {
        // No protection change: the bytes are already visible through the RX view. Only the
        // instruction cache has to learn about the span just written and the patched ranges.
        if (size_ > append_begin)
            RecordWriteRange(append_begin, size_);
#    if NXBOX_STALL_PROFILE
        const NxboxStall::Scope scope{NxboxStall::Kind::JitProtect};
#    endif
        for (const auto& range : write_ranges) {
            if (!FlushInstructionCache(GetCurrentProcess(), getCode() + range.begin, range.end - range.begin)) {
                UNREACHABLE_MSG("NXBOX JIT instruction cache flush failed: error={}", GetLastError());
            }
        }
        write_ranges.clear();
        writing = false;
        return;
    }
    if (size_ > append_begin) {
        RecordWriteRange(append_begin, size_);
        executable_end = std::max(executable_end, (size_ + JitPageSize() - 1) & ~(JitPageSize() - 1));
    }
    for (const auto& range : write_ranges) {
        ProtectMemory(top_ + range.begin, range.end - range.begin, true);
    }
    write_ranges.clear();
    writing = false;
#elif defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
#    ifdef _WIN32
    ProtectMemory(getCode(), committed_size, true);
#    else
    ProtectMemory(getCode(), maxSize_, true);
#    endif
#endif
}

#ifdef DYNARMIC_UWP_APPCONTAINER
void BlockOfCode::RecordWriteRange(size_t begin, size_t end) {
    // Protect mode works in whole pages; dual-map mode only flushes the exact bytes.
    const size_t mask = dual_map ? 0 : JitPageSize() - 1;
    begin &= ~mask;
    end = (end + mask) & ~mask;
    auto it = write_ranges.begin();
    while (it != write_ranges.end() && it->end < begin)
        ++it;
    while (it != write_ranges.end() && it->begin <= end) {
        begin = std::min(begin, it->begin);
        end = std::max(end, it->end);
        it = write_ranges.erase(it);
    }
    write_ranges.insert(it, WriteRange{begin, end});
}
#endif

void BlockOfCode::PrepareWriteRange([[maybe_unused]] const void* address, [[maybe_unused]] size_t size) {
#ifdef DYNARMIC_UWP_APPCONTAINER
    if (!writing)
        UNREACHABLE_MSG("NXBOX JIT write outside emission");
    // `address` is an executable address; getCode() == top_ unless the buffer is dual-mapped.
    const size_t offset = static_cast<const u8*>(address) - getCode();
    if (offset > committed_size || size > committed_size - offset)
        UNREACHABLE_MSG("NXBOX JIT write outside committed cache");
    if (dual_map) {
        if (size != 0)
            RecordWriteRange(offset, offset + size);
        return;
    }
    if (size == 0 || offset >= executable_end)
        return;
    const size_t mask = JitPageSize() - 1;
    const size_t begin = offset & ~mask;
    const size_t end = std::min(executable_end, (offset + size + mask) & ~mask);
    // Do not reopen pages already touched in this emission/invalidation batch.
    size_t cursor = begin;
    for (const auto& range : write_ranges) {
        if (range.end <= cursor)
            continue;
        if (range.begin >= end)
            break;
        if (cursor < range.begin)
            ProtectMemory(top_ + cursor, range.begin - cursor, false);
        cursor = std::max(cursor, range.end);
    }
    if (cursor < end)
        ProtectMemory(top_ + cursor, end - cursor, false);
    RecordWriteRange(begin, end);
#endif
}

void BlockOfCode::ClearCache() {
    ASSERT(prelude_complete);
#ifdef DYNARMIC_UWP_APPCONTAINER
    // Retired code becomes the writable append suffix; the prelude stays RX.
    const size_t begin = static_cast<const u8*>(code_begin) - getCode();
    if (dual_map) {
        // Nothing to reprotect. Stale RX bytes are overwritten and re-flushed as they are re-emitted.
        write_ranges.clear();
        append_begin = begin;
    } else if (executable_end > begin) {
        ProtectMemory(top_ + begin, executable_end - begin, false);
    }
    executable_end = begin;
    append_begin = begin;
#endif
    SetCodePtr(code_begin);
}

size_t BlockOfCode::SpaceRemaining() const {
    ASSERT(prelude_complete);
    const u8* current_ptr = getCurr<const u8*>();
    // getCurr() is an executable address, so compare against the executable end of the buffer.
    const u8* const end_ptr = getCode() + maxSize_;
    if (current_ptr >= end_ptr)
        return 0;
    return end_ptr - current_ptr;
}

void BlockOfCode::EnsureMemoryCommitted([[maybe_unused]] size_t codesize) {
#ifdef _WIN32
#    ifdef DYNARMIC_UWP_APPCONTAINER
    if (size_ > maxSize_ || codesize > maxSize_ - size_)
        UNREACHABLE_MSG("NXBOX JIT code cache exhausted");
    if (committed_size < size_ + codesize) {
        const size_t mask = JitPageSize() - 1;
        const size_t next_size = std::min(maxSize_, (std::max(size_ + codesize, committed_size + PRELUDE_COMMIT_SIZE) + mask) & ~mask);
#    if NXBOX_STALL_PROFILE
        const NxboxStall::Scope commit_scope{NxboxStall::Kind::JitEmitCommit};
#    endif
        // Never recommit old RX pages with RW permissions.
        if (!DYNARMIC_VIRTUAL_ALLOC(top_ + committed_size, next_size - committed_size, MEM_COMMIT, PAGE_READWRITE))
            UNREACHABLE_MSG("NXBOX JIT commit failed: error={}", GetLastError());
        committed_size = next_size;
        code_memory.Set(committed_size);
    }
#    else
    if (committed_size < size_ + codesize) {
        committed_size = std::min<size_t>(maxSize_, committed_size + codesize);
#        ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
        // W^X: commit read-write only; ProtectMemory() flips pages to RX before execution.
        DYNARMIC_VIRTUAL_ALLOC(top_, committed_size, MEM_COMMIT, PAGE_READWRITE);
#        else
        // RWX fast path — desktop only. Never compiled under DYNARMIC_UWP_APPCONTAINER, which
        // forces DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT on (the AppContainer never grants RWX).
        VirtualAlloc(top_, committed_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#        endif
    }
#    endif
#endif
}

HaltReason BlockOfCode::RunCode(void* jit_state, CodePtr code_ptr) const {
    return run_code(jit_state, code_ptr);
}

HaltReason BlockOfCode::StepCode(void* jit_state, CodePtr code_ptr) const {
    return step_code(jit_state, code_ptr);
}

void BlockOfCode::ReturnFromRunCode(bool mxcsr_already_exited) {
    size_t index = 0;
    if (mxcsr_already_exited)
        index |= MXCSR_ALREADY_EXITED;
    jmp(return_from_run_code[index]);
}

void BlockOfCode::ForceReturnFromRunCode(bool mxcsr_already_exited) {
    size_t index = FORCE_RETURN;
    if (mxcsr_already_exited)
        index |= MXCSR_ALREADY_EXITED;
    jmp(return_from_run_code[index]);
}

void BlockOfCode::GenRunCode(std::function<void(BlockOfCode&)> rcp) {
    Xbyak::Label return_to_caller, return_to_caller_mxcsr_already_exited;

    align();
    run_code = getCurr<RunCodeFuncType>();

    // This serves two purposes:
    // 1. It saves all the registers we as a callee need to save.
    // 2. It aligns the stack so that the code the JIT emits can assume
    //    that the stack is appropriately aligned for CALLs.
    ABI_PushCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));

    mov(ABI_JIT_PTR, ABI_PARAM1);
    mov(rbx, ABI_PARAM2); // save temporarily in non-volatile register

    if (cb.enable_cycle_counting) {
        cb.GetTicksRemaining.EmitCall(*this);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], ABI_RETURN);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], ABI_RETURN);
    }

    // r14 = page table
    // r13 = fastmem pointer
    rcp(*this);

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited, T_NEAR);

    SwitchMxcsrOnEntry();
    jmp(rbx);

    align();
    step_code = getCurr<RunCodeFuncType>();

    ABI_PushCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));

    mov(ABI_JIT_PTR, ABI_PARAM1);

    if (cb.enable_cycle_counting) {
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], 1);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 1);
    }

    rcp(*this);

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited, T_NEAR);
    lock(); or_(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], u32(HaltReason::Step));

    SwitchMxcsrOnEntry();
    jmp(ABI_PARAM2);

    // Dispatcher loop

    align();
    return_from_run_code[0] = getCurr<const void*>();

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller);
    if (cb.enable_cycle_counting) {
        cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);
        jng(return_to_caller);
    }
    cb.LookupBlock.EmitCall(*this);
    jmp(ABI_RETURN);

    align();
    return_from_run_code[MXCSR_ALREADY_EXITED] = getCurr<const void*>();

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited);
    if (cb.enable_cycle_counting) {
        cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);
        jng(return_to_caller_mxcsr_already_exited);
    }
    SwitchMxcsrOnEntry();
    cb.LookupBlock.EmitCall(*this);
    jmp(ABI_RETURN);

    align();
    return_from_run_code[FORCE_RETURN] = getCurr<const void*>();
    L(return_to_caller);

    SwitchMxcsrOnExit();
    // fallthrough

    return_from_run_code[MXCSR_ALREADY_EXITED | FORCE_RETURN] = getCurr<const void*>();
    L(return_to_caller_mxcsr_already_exited);

    if (cb.enable_cycle_counting) {
        cb.AddTicks.EmitCall(*this, [this](RegList param) {
            mov(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)]);
            sub(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
        });
    }

    xor_(eax, eax);
    xchg(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], eax);

    ABI_PopCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));
    ret();

    PerfMapRegister(run_code, getCurr(), "dynarmic_dispatcher");
}

void BlockOfCode::SwitchMxcsrOnEntry() {
    stmxcsr(dword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, save_host_MXCSR)]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
}

void BlockOfCode::SwitchMxcsrOnExit() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
    ldmxcsr(dword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, save_host_MXCSR)]);
}

void BlockOfCode::EnterStandardASIMD() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_asimd_MXCSR]);
}

void BlockOfCode::LeaveStandardASIMD() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_asimd_MXCSR]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
}

void BlockOfCode::UpdateTicks() {
    if (!cb.enable_cycle_counting) {
        return;
    }

    cb.AddTicks.EmitCall(*this, [this](RegList param) {
        mov(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)]);
        sub(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
    });

    cb.GetTicksRemaining.EmitCall(*this);
    mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], ABI_RETURN);
    mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], ABI_RETURN);
}

void BlockOfCode::LookupBlock() {
    cb.LookupBlock.EmitCall(*this);
}

void BlockOfCode::LoadRequiredFlagsForCondFromRax(IR::Cond cond) {
#ifdef __APPLE__
    static const bool is_rosetta = IsUnderRosetta();
#endif

    // sahf restores SF, ZF, CF
    // add al, 0x7F restores OF

    switch (cond) {
    case IR::Cond::EQ:  // z
    case IR::Cond::NE:  // !z
    case IR::Cond::CS:  // c
    case IR::Cond::CC:  // !c
    case IR::Cond::MI:  // n
    case IR::Cond::PL:  // !n
        sahf();
        break;
    case IR::Cond::VS:  // v
    case IR::Cond::VC:  // !v
        cmp(al, 0x81);
        break;
    case IR::Cond::HI:  // c & !z
    case IR::Cond::LS:  // !c | z
        sahf();
        cmc();
        break;
    case IR::Cond::GE:  // n == v
    case IR::Cond::LT:  // n != v
    case IR::Cond::GT:  // !z & (n == v)
    case IR::Cond::LE:  // z | (n != v)
#ifdef __APPLE__
        if (is_rosetta) {
            shl(al, 3);
            xchg(al, ah);
            push(rax);
            popf();
            break;
        }
#endif
        cmp(al, 0x81);
        sahf();
        break;
    case IR::Cond::AL:
    case IR::Cond::NV:
        break;
    default:
        UNREACHABLE();
    }
}

Xbyak::Address BlockOfCode::Const(const Xbyak::AddressFrame& frame, u64 lower, u64 upper) {
    ++constant_references;
    return constant_pool.GetConstant(*this, frame, lower, upper);
}

CodePtr BlockOfCode::GetCodeBegin() const {
    return code_begin;
}

size_t BlockOfCode::GetTotalCodeSize() const {
    return maxSize_;
}

void* BlockOfCode::AllocateFromCodeSpace(size_t alloc_size) {
    if (size_ + alloc_size >= maxSize_) {
        using Xbyak::Error;
        XBYAK_THROW(Xbyak::ERR_CODE_IS_TOO_BIG);
    }

    EnsureMemoryCommitted(alloc_size);

    // Returns an executable address like everything else; the zero fill is a store, so it goes through
    // the writable view when the buffer is dual-mapped. Callers that write to the block must use
    // Writable().
    void* ret = getCurr<void*>();
    size_ += alloc_size;
    memset(Writable(ret), 0, alloc_size);
    return ret;
}

void BlockOfCode::SetCodePtr(CodePtr code_ptr) {
    // The "size" defines where top_, the insertion point, is.
    size_t required_size = reinterpret_cast<const u8*>(code_ptr) - getCode();
    setSize(required_size);
}

void BlockOfCode::EnsurePatchLocationSize(CodePtr begin, size_t size) {
    size_t current_size = getCurr<const u8*>() - reinterpret_cast<const u8*>(begin);
    ASSERT(current_size <= size);
    nop(size - current_size);
}

}  // namespace Dynarmic::Backend::X64
