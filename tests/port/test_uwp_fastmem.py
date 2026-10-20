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
