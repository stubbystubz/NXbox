# SPDX-FileCopyrightText: Copyright 2026 NXbox contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the production UWP allocator against strict Windows API mocks on macOS."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class UwpFastmemTests(unittest.TestCase):
    def allocator_source(self):
        source = (ROOT / "src/common/host_memory.cpp").read_text()
        allocator = source.split("// UWP backing uses independent", 1)[1].split(
            "\n#endif\n\n#elif defined(__OPENORBIS__)", 1
        )[0]
        handler = source.split("namespace {\nvoid* g_section_owner", 1)[1].split(
            "} // namespace", 1
        )[0]
        handler = re.sub(
            r"bool ProbeMemoryAccess\(.*?^\}",
            "bool ProbeMemoryAccess(volatile u8* address, bool write, u8& value) {\n"
            "    return Mock::ProbeMemoryAccess(address, write, value);\n}",
            handler,
            flags=re.DOTALL | re.MULTILINE,
        )
        native_test = (ROOT / "tests/port/uwp_fastmem.cpp").read_text()
        native_test = native_test.replace(
            "// PRODUCTION_HANDLER", "namespace {\nvoid* g_section_owner" + handler + "}\n"
        ).replace("// PRODUCTION_ALLOCATOR", "// UWP backing uses independent" + allocator)
        seh_source = (
            ROOT / "src/dynarmic/src/dynarmic/backend/x64/exception_handler_windows.cpp"
        ).read_text()
        seh = re.search(
            r"code.CallLambda\((\[\].*?^        \})\);", seh_source, re.MULTILINE | re.DOTALL
        )[1]
        native_test = native_test.replace(
            "// PRODUCTION_SEH", "[[maybe_unused]] auto DispatchFastmem = " + seh + ";"
        )
        return native_test

    def test_allocator_rejects_incompatible_view_protection_with_error_87(self):
        self.compile_and_run(self.allocator_source())

    def test_probe_error_87_logs_exact_call_and_releases_arena(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    Mock::Reset();
    Mock::force_noaccess_alias = true;
    {
        Common::HostMemory::Impl memory(4 * 65536, 32 * 65536);
        assert(memory.Init() && !memory.virtual_base);
        assert(Mock::committed == 0 && Common::g_backing_committed == 0);
        assert(api_errors.size() == 1);
        const auto& error = api_errors.front();
        assert(error.api == "VirtualProtectFromApp" && error.address);
        assert(error.size == 4096 && error.protection == PAGE_READONLY && error.error == 87);
        assert(Fault(memory.backing_base));
    }
    Mock::Reset();
}
"""
        self.compile_and_run(native_test)

    def test_lazy_host_fault_failure_rejects_fastmem_after_api_probe_passes(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    Mock::Reset();
    for (bool suppress : {false, true}) {
        Mock::reject_fault_maps = !suppress;
        Mock::suppress_veh = suppress;
        {
            Common::HostMemory::Impl memory(4 * 65536, 32 * 65536);
            assert(memory.Init() && !memory.virtual_base);
            assert(Mock::probe_accesses == 1); // Explicit section API probe passed.
            assert(Mock::committed == 0 && Common::g_backing_committed == 0);
            assert(Common::g_section_owner == nullptr && Common::g_section_fault == nullptr);
            assert(Fault(memory.backing_base)); // Private demand-commit fallback works.
        }
        Mock::Reset();
    }
}
"""
        self.compile_and_run(native_test)

    def test_protected_jit_fault_prepares_native_fallback_before_handoff(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    Mock::Reset();
    {
        Common::HostMemory::Impl memory(4 * 65536, 32 * 65536);
        assert(memory.Init() && memory.virtual_base);
        assert(Mock::probe_accesses == 5);
        memory.Map(0, 65536, 65536, Common::MemoryPermission::Read);
        assert(Mock::committed == 0);
        // JIT write AV reaches VEH; permission fault proceeds to Dynarmic SEH.
        assert(!Fault(memory.virtual_base));
        u64 stack[4]{};
        CONTEXT context{0x1234, reinterpret_cast<u64>(stack + 4)};
        EXCEPTION_RECORD record{EXCEPTION_ACCESS_VIOLATION, 2,
                                {1, reinterpret_cast<size_t>(memory.virtual_base)}};
        std::function<FakeCall(u64)> callback = [](u64 rip) {
            assert(rip == 0x1234);
            return FakeCall{0x5678, 0x1238};
        };
        assert(DispatchFastmem(callback, &context, &record) == ExceptionContinueExecution);
        assert(context.Rip == 0x5678 && context.Rsp == reinterpret_cast<u64>(stack + 3));
        assert(stack[3] == 0x1238);
        // Native callback must be able to write immediately, without another AV.
        auto* backing = memory.backing_base + 65536;
        assert(Mock::Accessible(backing, true));
        const int maps = Mock::calls["map"];
        u8 value = 73;
        assert(Mock::ProbeMemoryAccess(backing, true, value));
        assert(Mock::calls["map"] == maps && backing[0] == 73);
        assert(Fault(memory.virtual_base, 0) && memory.virtual_base[0] == 73);
        // Reclamation removes both views, and the same sequence works again.
        memory.Clear(65536, 65536, 0);
        assert(!Fault(memory.virtual_base));
        assert(Mock::Accessible(backing, true) && backing[0] == 0);
        // An alias API failure also hands off with backing ready. Disabled
        // aliases must keep preparing backing after later reclamation.
        memory.Map(65536, 2 * 65536, 65536, Common::MemoryPermission::ReadWrite);
        Mock::fail_name = "map";
        Mock::fail_at = Mock::calls["map"] + 2;
        assert(!Fault(memory.virtual_base + 65536));
        assert(Mock::Accessible(memory.backing_base + 2 * 65536, true));
        memory.Clear(2 * 65536, 65536, 0);
        assert(!Fault(memory.virtual_base + 65536));
        assert(Mock::Accessible(memory.backing_base + 2 * 65536, true));
    }
    Mock::Reset();
}
"""
        self.compile_and_run(native_test)

    def test_untracked_alias_callback_prepares_untouched_and_reclaimed_backing(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        memory_source = (ROOT / "src/core/memory.cpp").read_text()
        helper = re.search(
            r"    u8\* PrepareNativePointer\(.*?^    }", memory_source, re.DOTALL | re.MULTILINE
        )[0]
        native_test += r"""
#define NXBOX_UWP 1
struct Buffer {
    Common::HostMemory::Impl& memory;
    u8* VirtualBasePointer() const { return memory.virtual_base; }
    bool PrepareBackingPage(const u8* pointer) { return memory.PrepareBackingPage(pointer); }
};
struct Callback {
    Buffer* host_buffer;
HELPER
};
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    Mock::Reset();
    {
        Common::HostMemory::Impl memory(4 * 65536, 32 * 65536);
        assert(memory.Init() && memory.fastmem_enabled.load());
        Buffer buffer{memory};
        Callback callback{&buffer};
        auto* backing = memory.backing_base + 2 * 65536 + 8;
        // No regional alias record: a valid canonical page-table callback follows.
        assert(!Fault(memory.virtual_base + 17 * 65536));
        assert(!Mock::Accessible(backing, true));
        for (int pass = 0; pass < 2; ++pass) {
            assert(callback.PrepareNativePointer(backing) == backing);
            assert(Mock::Accessible(backing, true));
            const int maps = Mock::calls["map"];
            u8 value = 42;
            assert(Mock::ProbeMemoryAccess(backing, true, value));
            assert(Mock::calls["map"] == maps && backing[0] == 42);
            memory.Clear(2 * 65536, 65536, 0);
            assert(!Mock::Accessible(backing, true));
        }
        assert(memory.fastmem_enabled.load()); // Normal protection/hole faults are expected.
    }
    Mock::Reset();
}
""".replace("HELPER", helper)
        self.compile_and_run(native_test)

    def test_default_native_pointer_path_performs_no_preparation_call(self):
        memory_source = (ROOT / "src/core/memory.cpp").read_text()
        helper = re.search(
            r"    u8\* PrepareNativePointer\(.*?^    }", memory_source, re.DOTALL | re.MULTILINE
        )[0]
        native_test = r"""
#include <cassert>
#include <new>
using u8 = unsigned char;
#define NXBOX_UWP 1
struct Buffer {
    int calls{};
    u8* arena{};
    u8* VirtualBasePointer() const { return arena; }
    bool PrepareBackingPage(const u8*) { ++calls; return true; }
};
struct Callback {
    Buffer* host_buffer;
HELPER
};
int main() {
    Buffer buffer;
    Callback callback{&buffer};
    u8 value{};
    for (int i = 0; i < 1000; ++i)
        assert(callback.PrepareNativePointer(&value) == &value);
    assert(callback.PrepareNativePointer(nullptr) == nullptr);
    assert(buffer.calls == 0);
    buffer.arena = &value;
    assert(callback.PrepareNativePointer(&value) == &value);
    assert(buffer.calls == 1); // The regional path still prepares backing.
}
""".replace("HELPER", helper)
        self.compile_and_run(native_test)

    def test_device_continuity_translation_does_not_materialize_backing(self):
        memory_source = (ROOT / "src/core/memory.cpp").read_text()
        helper = re.search(
            r"    u8\* PrepareNativePointer\(.*?^    }", memory_source, re.DOTALL | re.MULTILINE
        )[0]
        lookup = re.search(
            r"    template\s*<bool prepare_backing.*?^    }",
            memory_source,
            re.DOTALL | re.MULTILINE,
        )[0]
        translation = re.search(
            r"    \[\[nodiscard\]\] u8\* GetPointerForAddressTranslation\(.*?^    }",
            memory_source,
            re.DOTALL | re.MULTILINE,
        )[0]
        device_source = (ROOT / "src/core/device_memory_manager.inc").read_text()
        continuity = re.search(
            r"template <typename Traits>\nvoid DeviceMemoryManager<Traits>::TrackContinuityImpl.*?^}",
            device_source,
            re.DOTALL | re.MULTILINE,
        )[0]
        # Both metadata walks must use the same allocation-free translation API.
        map_body = device_source.split("::Map(DAddr", 1)[1].split("::Unmap(", 1)[0]
        self.assertIn("GetPointerForAddressTranslation", map_body)
        self.assertNotIn("GetPointerSilent", map_body)
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
#define NXBOX_UWP 1
#define ASSERT_MSG(condition, ...) assert(condition)
using VAddr = u64;
using DAddr = u64;
namespace Common {
using ProcessAddress = u64;
size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
enum class PageType { Unmapped, Memory, DebugMemory, RasterizerCachedMemory };
struct PageTable {
    struct PageEntryData {
        uintptr_t pointer{};
        PageType type{PageType::Memory};
        static uintptr_t ExtractPointer(PageEntryData raw) { return raw.pointer; }
        PageEntryData Raw() const { return *this; }
    };
    std::vector<PageEntryData> entries;
};
}
u64 GetInteger(u64 value) { return value; }
constexpr size_t YUZU_PAGEBITS = 12;
bool AddressSpaceContains(const Common::PageTable& table, u64 address, size_t) {
    return (address >> YUZU_PAGEBITS) < table.entries.size();
}
struct Buffer {
    Common::HostMemory::Impl& memory;
    u8* VirtualBasePointer() const { return memory.virtual_base; }
    bool PrepareBackingPage(const u8* pointer) { return memory.PrepareBackingPage(pointer); }
};
struct ProcessMemory {
    Buffer* host_buffer;
    Common::PageTable* current_page_table;
    u8* special_pointer{};
    u8* GetPointerFromDebugMemory(u64) const { return special_pointer; }
    u8* GetPointerFromRasterizerCachedMemory(u64) const { return special_pointer; }
HELPER
LOOKUP
TRANSLATION
    u8* GetPointerSilent(u64 address) const {
        return GetPointerImpl(address, [] {}, [] {});
    }
};
namespace Core::Memory {
using Memory = ProcessMemory;
constexpr size_t YUZU_PAGEBITS = 12, YUZU_PAGESIZE = 4096;
}
namespace Memory = Core::Memory;
struct Asid { size_t id; };
template <typename Traits> struct DeviceMemoryManager {
    std::array<ProcessMemory*, 1> registered_processes;
    static constexpr size_t page_size = 4096;
    struct Entry { u32 continuity_tracker{}; };
    struct Entries {
        std::vector<Entry> values;
        void CommitRegion(size_t, size_t end) { values.resize(end); }
        Entry& GetUnchecked(size_t index) { return values.at(index); }
    } tracked_entries;
    void TrackContinuityImpl(DAddr, VAddr, size_t, Asid);
};
CONTINUITY
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    Mock::Reset();
    {
        constexpr size_t size = 32 * 65536;
        Common::HostMemory::Impl memory(size, 2 * size);
        assert(memory.Init() && memory.fastmem_enabled.load());
        Buffer buffer{memory};
        Common::PageTable table;
        table.entries.resize(size / 4096);
        for (auto& entry : table.entries)
            entry.pointer = reinterpret_cast<uintptr_t>(memory.backing_base);
        ProcessMemory process{&buffer, &table};
        DeviceMemoryManager<int> device{{&process}, {}};
        const auto calls = Mock::calls;
        Mock::fail_name = "create";
        Mock::fail_at = Mock::calls["create"] + 1;
        Mock::reject_private_replacement = true;
        // The production reverse walk reaches the last region first. No bytes are
        // touched: even unavailable backing APIs must not fail this metadata query.
        device.TrackContinuityImpl(0, 0, size, {0});
        assert(Mock::calls == calls && Mock::committed == 0);
        assert(memory.fastmem_enabled.load());
        assert(device.tracked_entries.values.front().continuity_tracker == size / 4096 - 1);
        assert(device.tracked_entries.values.back().continuity_tracker == 0);
        for (auto type : {Common::PageType::DebugMemory,
                          Common::PageType::RasterizerCachedMemory}) {
            table.entries[0] = {0, type};
            process.special_pointer = memory.backing_base;
            assert(process.GetPointerForAddressTranslation(0) == memory.backing_base);
            assert(Mock::committed == 0 && Mock::calls == calls);
        }
        table.entries[0] = {0, Common::PageType::Unmapped};
        assert(process.GetPointerForAddressTranslation(0) == nullptr);
        assert(process.GetPointerForAddressTranslation(size) == nullptr);
        table.entries[0] = {reinterpret_cast<uintptr_t>(memory.backing_base)};
        // A genuine native-pointer request must still prepare, and report failure
        // before a dereference when both recovery paths are unavailable.
        bool failed{};
        try { process.GetPointerSilent(0); } catch (const std::bad_alloc&) { failed = true; }
        assert(failed && !memory.fastmem_enabled.load());
        Mock::reject_private_replacement = false;
        auto* pointer = process.GetPointerSilent(0);
        assert(pointer == memory.backing_base && Mock::Accessible(pointer, true));
    }
    Mock::Reset();
}
"""
        for marker, source in (
            ("HELPER", helper),
            ("LOOKUP", lookup),
            ("TRANSLATION", translation),
            ("CONTINUITY", continuity),
        ):
            native_test = native_test.replace(marker, source)
        self.compile_and_run(native_test)

    def test_backed_region_preparation_avoids_lock_and_rechecks_after_reclaim(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        # Instrument the production mutex without changing its locking semantics.
        native_test = "#include <atomic>\nstd::atomic<int> lock_calls{};\n" + native_test
        native_test = native_test.replace(
            "std::recursive_mutex mutex;",
            "struct CountingMutex { std::recursive_mutex inner; "
            "void lock() { ++lock_calls; inner.lock(); } "
            "void unlock() { inner.unlock(); } } mutex;",
        )
        native_test += r"""
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    Mock::Reset();
    for (bool private_backing : {false, true}) {
        {
            Common::HostMemory::Impl memory(4 * 65536, 32 * 65536);
            assert(memory.Init());
            auto* backing = memory.backing_base + 65536;
            if (private_backing) {
                Mock::fail_name = "create";
                Mock::fail_at = Mock::calls["create"] + 1;
            }
            int locks = lock_calls.load();
            assert(memory.PrepareBackingPage(backing));
            assert(lock_calls > locks); // First materialization must lock.
            locks = lock_calls.load();
            std::vector<std::thread> workers;
            for (int core = 0; core < 8; ++core)
                workers.emplace_back([&] {
                    for (int i = 0; i < 1000; ++i)
                        assert(memory.PrepareBackingPage(backing + 4096));
                });
            for (auto& worker : workers)
                worker.join();
            assert(lock_calls == locks);
            memory.Clear(65536, 65536, 0);
            assert(!Mock::Accessible(backing, true));
            locks = lock_calls.load();
            assert(memory.PrepareBackingPage(backing));
            assert(lock_calls > locks && Mock::Accessible(backing, true));
        }
        Mock::Reset();
    }
}
"""
        self.compile_and_run(native_test)

    def test_outside_native_pointers_pass_through_without_disabling_fastmem(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        memory_source = (ROOT / "src/core/memory.cpp").read_text()
        helper = re.search(
            r"    u8\* PrepareNativePointer\(.*?^    }", memory_source, re.DOTALL | re.MULTILINE
        )[0]
        native_test += r"""
#define NXBOX_UWP 1
struct Buffer {
    Common::HostMemory::Impl& memory;
    u8* VirtualBasePointer() const { return memory.virtual_base; }
    bool PrepareBackingPage(const u8* pointer) { return memory.PrepareBackingPage(pointer); }
};
struct Callback {
    Buffer* host_buffer;
HELPER
};
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    Mock::Reset();
    {
        Common::HostMemory::Impl memory(4 * 65536, 32 * 65536);
        assert(memory.Init() && memory.fastmem_enabled.load());
        Buffer buffer{memory};
        Callback callback{&buffer};
        u8 unrelated{};
        const auto calls = Mock::calls;
        for (auto* pointer : {&unrelated, memory.virtual_base,
                              reinterpret_cast<u8*>(reinterpret_cast<size_t>(memory.backing_base) - 1),
                              memory.backing_base + memory.backing_size}) {
            assert(callback.PrepareNativePointer(pointer) == pointer);
            assert(memory.fastmem_enabled.load());
            assert(fallback_reasons.empty() && Mock::calls == calls);
        }
        assert(callback.PrepareNativePointer(nullptr) == nullptr);
    }
    Mock::Reset();
}
""".replace("HELPER", helper)
        self.compile_and_run(native_test)

    def test_unexpected_backing_and_alias_state_latch_session_fallback(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    for (int mode : {0, 1, 2}) {
        const bool backing_fault = mode != 1;
        Mock::Reset();
        {
            Common::HostMemory::Impl memory(4 * 65536, 32 * 65536);
            assert(memory.Init());
            memory.Map(0, 65536, 65536, Common::MemoryPermission::ReadWrite);
            assert(Fault(memory.virtual_base));
            auto* backing = memory.backing_base + 65536;
            auto* target = backing_fault ? backing : memory.virtual_base;
            backing[0] = 42;
            DWORD old{};
            if (mode == 2)
                assert(Mock::Unmap(GetCurrentProcess(), backing, MEM_PRESERVE_PLACEHOLDER));
            else
                assert(Mock::Protect(target, 65536, PAGE_READONLY, &old));
            assert(!Mock::Accessible(target, true));
            assert(Fault(target) == backing_fault);
            assert(backing[0] == 42); // Remapping a retained section preserves guest bytes.
            assert(!memory.fastmem_enabled.load());
            assert(Mock::Accessible(backing, true));
            assert(fallback_reasons.size() == 1);
            // Existing view data is retained; new views never re-enable fastmem.
            backing[0] = 99;
            memory.Map(65536, 65536, 65536, Common::MemoryPermission::ReadWrite);
            assert(!Fault(memory.virtual_base + 65536));
            assert(memory.PrepareBackingPage(backing) && backing[0] == 99);
            assert(!memory.fastmem_enabled.load() && fallback_reasons.size() == 1);
        }
    }
    Mock::Reset();
}
"""
        self.compile_and_run(native_test)

    def test_failed_native_preparation_is_explicit_and_never_claims_retry(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    Mock::Reset();
    {
        Common::HostMemory::Impl memory(4 * 65536, 32 * 65536);
        assert(memory.Init());
        Mock::fail_name = "create";
        Mock::fail_at = Mock::calls["create"] + 1;
        Mock::reject_private_replacement = true;
        assert(!memory.PrepareBackingPage(memory.backing_base + 65536));
        assert(!Mock::Accessible(memory.backing_base + 65536, true));
        assert(!memory.fastmem_enabled.load() && fallback_reasons.size() == 1);
        // A later successful backing allocation cannot reset the session latch.
        assert(memory.PrepareBackingPage(memory.backing_base + 65536));
        assert(!memory.fastmem_enabled.load() && fallback_reasons.size() == 1);
    }
    Mock::Reset();
}
"""
        self.compile_and_run(native_test)

    def test_runtime_section_failure_recovers_private_backing_in_place(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    for (const char* api : {"create", "map"}) {
        Mock::Reset();
        {
            Common::HostMemory::Impl memory(4 * 65536, 32 * 65536);
            assert(memory.Init());
            memory.Map(0, 65536, 65536, Common::MemoryPermission::ReadWrite);
            auto* backing = memory.backing_base + 65536;
            Mock::fail_name = api;
            Mock::fail_at = Mock::calls[api] + 1; // Fail section creation or backing-view map.
            // Existing compiled alias access declines, with usable native backing.
            assert(!Fault(memory.virtual_base));
            assert(!memory.fastmem_enabled.load() && fallback_reasons.size() == 1);
            assert(Mock::sections == 0 && Mock::Accessible(backing, true));
            assert(Mock::calls["private_replace"] == 1);
            assert(memory.PrepareBackingPage(backing));
            backing[0] = 91;
            memory.Clear(65536 + 4096, 4096, 0);
            assert(backing[0] == 91 && backing[4096] == 0);
            // Reclaim private allocation to its original placeholder, with no relocation.
            memory.Clear(65536, 65536, 0);
            assert(!Mock::Accessible(backing, true) && Mock::committed == 0);
            assert(memory.PrepareBackingPage(backing) && backing[0] == 0);
            assert(!memory.fastmem_enabled.load() && fallback_reasons.size() == 1);
        }
    }
    Mock::Reset();
}
"""
        self.compile_and_run(native_test)

    def test_boundary_writes_and_private_fallback_keep_last_region_in_bounds(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
int main() {
    setenv("NXBOX_FASTMEM", "1", 1);
    for (bool private_recovery : {false, true}) {
        Mock::Reset();
        {
            constexpr size_t region = 65536;
            // Include a short final section, as well as ordinary 64 KiB boundaries.
            constexpr size_t size = 3 * region + 4096;
            Common::HostMemory::Impl memory(size, 32 * region);
            assert(memory.Init());
            memory.Map(0, 0, size, Common::MemoryPermission::ReadWrite);
            if (private_recovery) {
                // Retain an existing section/alias and its data through recovery.
                assert(Fault(memory.virtual_base));
                Mock::Store(memory.backing_base, 99);
                Mock::fail_name = "map";
                Mock::fail_at = Mock::calls["map"] + 1;
            }
            // Simulate native range writes using the production VEH for each
            // inaccessible destination, including the final byte of backing.
            for (size_t begin : {region - 8, 2 * region - 8, 3 * region - 8, size - 16}) {
                for (size_t offset = begin; offset < begin + 16; ++offset) {
                    u8 value = 73;
                    assert(Mock::ProbeMemoryAccess(memory.backing_base + offset, true, value));
                    assert(memory.backing_base[offset] == 73);
                }
            }
            // The one-past-end pointer is outside backing, never a region index.
            // An untracked arena address also declines, allowing guarded SEH fallback.
            assert(!Fault(memory.backing_base + size));
            assert(!Fault(memory.virtual_base + size));
            assert(!Fault(memory.virtual_base + 32 * region));
            if (private_recovery) {
                assert(!memory.fastmem_enabled.load());
                assert(Mock::calls["map"] == Mock::fail_at);
                assert(Mock::calls["private_replace"] == 3);
                assert(fallback_reasons.size() == 1 && api_errors.size() == 1);
                assert(memory.backing_base[0] == 99 && memory.virtual_base[0] == 99);
                // Reclaimed fresh backing stays private: no repeated section/view
                // failures or log flood on the next native access.
                const auto creates = Mock::calls["create"];
                memory.Clear(region, size - region, 0);
                assert(memory.PrepareBackingPage(memory.backing_base + size - 1));
                assert(Mock::calls["create"] == creates);
                assert(Mock::calls["map"] == Mock::fail_at);
                assert(memory.backing_base[size - 1] == 0);
            } else {
                assert(memory.fastmem_enabled.load());
                assert(Fault(memory.virtual_base + size - 1));
                assert(Mock::Accessible(memory.virtual_base + size - 1, true));
                assert(memory.backing_base[size - 1] == 73);
            }
        }
        Mock::Reset();
    }
}
"""
        self.compile_and_run(native_test)

    def test_session_latch_stops_a32_and_a64_fastmem_emission(self):
        emit_source = (
            ROOT / "src/dynarmic/src/dynarmic/backend/x64/emit_x64_memory.cpp.inc"
        ).read_text()
        should_fastmem = (
            re.search(
                r"std::optional<AxxEmitX64::DoNotFastmemMarker>.*?^}",
                emit_source,
                re.DOTALL | re.MULTILINE,
            )[0]
            .replace("AxxEmitX64", "Emitter")
            .replace("AxxEmitContext", "Context")
        )
        for arch in ("32", "64"):
            config = (ROOT / f"src/dynarmic/src/dynarmic/interface/A{arch}/config.h").read_text()
            query = re.search(
                r"    bool IsFastmemEnabled\(\) const {.*?^    }",
                config,
                re.DOTALL | re.MULTILINE,
            )[0]
            native_test = r"""
#include <atomic>
#include <cassert>
#include <optional>
#include <set>
#include <tuple>
struct Config {
    std::optional<uintptr_t> fastmem_pointer;
    const std::atomic<bool>* fastmem_enabled = nullptr;
QUERY
};
struct Context { int Location() { return 1; } };
namespace IR { struct Inst { int GetName() { return 2; } }; }
struct Emitter {
    using DoNotFastmemMarker = std::tuple<int, int>;
    Config conf;
    struct Handler { bool SupportsFastmem() const { return true; } } exception_handler;
    std::set<DoNotFastmemMarker> do_not_fastmem;
    std::optional<DoNotFastmemMarker> ShouldFastmem(Context&, IR::Inst*) const;
};
SHOULD_FASTMEM
int main() {
    std::atomic<bool> enabled{true};
    Emitter emitter;
    emitter.conf = {0x100000, &enabled};
    Context ctx;
    IR::Inst inst;
    assert(emitter.ShouldFastmem(ctx, &inst));
    enabled.store(false);
    assert(!emitter.ShouldFastmem(ctx, &inst));
    emitter.conf.fastmem_enabled = nullptr; // Other platforms retain their selection.
    assert(emitter.ShouldFastmem(ctx, &inst));
}
""".replace("QUERY", query).replace("SHOULD_FASTMEM", should_fastmem)
            self.compile_and_run(native_test)

    def test_seh_declines_non_data_faults_without_changing_context(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
int main() {
    unsigned calls = 0;
    std::function<FakeCall(u64)> callback = [&](u64) { ++calls; return FakeCall{}; };
    CONTEXT context{0x1234, 0}; // Invalid stack: declining must never touch it.
    EXCEPTION_RECORD record{EXCEPTION_ACCESS_VIOLATION, 2, {1, 0x10000}};
    for (unsigned test = 0; test < 5; ++test) {
        record = {EXCEPTION_ACCESS_VIOLATION, 2, {1, 0x10000}};
        if (test == 0) record.ExceptionCode = 0xe06d7363; // C++ exception.
        if (test == 1) record.NumberParameters = 1;
        if (test == 2) record.ExceptionInformation[0] = 8; // Execute AV.
        if (test == 3) record.ExceptionFlags = EXCEPTION_UNWINDING;
        if (test == 4) record.ExceptionFlags = EXCEPTION_EXIT_UNWIND;
        assert(DispatchFastmem(callback, &context, &record) == ExceptionContinueSearch);
        assert(context.Rip == 0x1234 && context.Rsp == 0 && calls == 0);
    }
    record = {EXCEPTION_ACCESS_VIOLATION, 2, {1, 0x10000}};
    assert(DispatchFastmem(callback, &context, &record) == ExceptionContinueSearch);
    assert(context.Rip == 0x1234 && context.Rsp == 0 && calls == 1);
}
"""
        self.compile_and_run(native_test)

    def test_uwp_settings_gate_and_opt_out(self):
        source = (ROOT / "src/common/settings.cpp").read_text()
        gate = re.search(
            r"bool IsFastmemEnabled\(\) \{.*?^\}", source, re.DOTALL | re.MULTILINE
        ).group()
        native_test = r"""
#include <cassert>
#include <cstdlib>
#include <string_view>
#define YUZU_UWP_APPCONTAINER 1
namespace Settings {
enum class CpuAccuracy { Auto, Accurate, Debugging, Unsafe };
struct Accuracy {
    CpuAccuracy value = CpuAccuracy::Auto;
    CpuAccuracy GetValue() { return value; }
};
struct Values {
    Accuracy cpu_accuracy;
    bool cpuopt_fastmem = false;
    bool cpuopt_unsafe_host_mmu = false;
} values;
GATE
}
int main() {
    using namespace Settings;
    for (auto accuracy : {CpuAccuracy::Auto, CpuAccuracy::Accurate, CpuAccuracy::Debugging,
                          CpuAccuracy::Unsafe}) {
        values.cpu_accuracy.value = accuracy;
        values.cpuopt_fastmem = values.cpuopt_unsafe_host_mmu = true;
        unsetenv("NXBOX_FASTMEM");
        assert(!IsFastmemEnabled()); // opt-in on UWP
        setenv("NXBOX_FASTMEM", "0", 1);
        assert(!IsFastmemEnabled());
        setenv("NXBOX_FASTMEM", "1", 1);
        assert(IsFastmemEnabled());
    }
    setenv("NXBOX_FASTMEM", "1", 1);
    values.cpu_accuracy.value = CpuAccuracy::Debugging;
    values.cpuopt_fastmem = false;
    assert(!IsFastmemEnabled());
    values.cpu_accuracy.value = CpuAccuracy::Unsafe;
    values.cpuopt_unsafe_host_mmu = false;
    assert(!IsFastmemEnabled());
}
"""
        self.compile_and_run(native_test.replace("GATE", gate))

    def test_unwind_registration_failure_disables_fastmem(self):
        source = (
            ROOT / "src/dynarmic/src/dynarmic/backend/x64/exception_handler_windows.cpp"
        ).read_text()
        registration = re.search(
            r"        registered = RtlAddFunctionTable.*?#endif", source, re.DOTALL
        ).group()
        query = re.search(
            r"    bool IsRegistered\(\) const noexcept \{.*?^    \}",
            source,
            re.DOTALL | re.MULTILINE,
        ).group()
        destructor = re.search(
            r"    ~Impl\(\) \{.*?^    \}", source, re.DOTALL | re.MULTILINE
        ).group()
        supports = re.search(
            r"bool ExceptionHandler::SupportsFastmem\(\) const noexcept \{.*?^\}",
            source,
            re.DOTALL | re.MULTILINE,
        ).group()
        native_test = r"""
#include <cassert>
#include <cstdint>
#include <memory>
#define YUZU_UWP_APPCONTAINER 1
template <typename... T> void Log(const char*, T&&...) {}
#define LOG_INFO(category, ...) Log(__VA_ARGS__)
using DWORD64 = std::uint64_t;
struct RUNTIME_FUNCTION {};
bool accept;
unsigned adds, deletes;
bool RtlAddFunctionTable(RUNTIME_FUNCTION*, unsigned count, DWORD64 base) {
    assert(count == 1 && base == 0x10000);
    ++adds;
    return accept;
}
void RtlDeleteFunctionTable(RUNTIME_FUNCTION*) { ++deletes; }
struct Code { void* getCode() { return reinterpret_cast<void*>(0x10000); } };
struct ExceptionHandler {
    struct Impl {
        bool registered{};
        RUNTIME_FUNCTION table;
        RUNTIME_FUNCTION* rfuncs = &table;
        Impl(Code& code) {
REGISTRATION
        }
QUERY
DESTRUCTOR
    };
    std::unique_ptr<Impl> impl;
    bool SupportsFastmem() const noexcept;
};
SUPPORTS
int main() {
    Code code;
    for (bool success : {false, true}) {
        accept = success;
        adds = deletes = 0;
        {
            ExceptionHandler handler;
            assert(!handler.SupportsFastmem());
            handler.impl = std::make_unique<ExceptionHandler::Impl>(code);
            assert(handler.SupportsFastmem() == success);
            assert(adds == 1);
        }
        assert(deletes == (success ? 1u : 0u));
    }
}
"""
        native_test = (
            native_test.replace("REGISTRATION", registration)
            .replace("QUERY", query)
            .replace("DESTRUCTOR", destructor)
            .replace("SUPPORTS", supports)
        )
        self.compile_and_run(native_test)

    def compile_and_run(self, native_test):
        with tempfile.TemporaryDirectory(prefix="nxbox-fastmem-") as directory:
            cpp = Path(directory) / "fastmem.cpp"
            binary = Path(directory) / "fastmem"
            cpp.write_text(native_test)
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O1",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
