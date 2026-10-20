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
        native_test = (ROOT / "tests/port/uwp_fastmem.cpp").read_text()
        native_test = native_test.replace(
            "// PRODUCTION_HANDLER", "namespace {\nvoid* g_section_owner" + handler + "}\n"
        ).replace("// PRODUCTION_ALLOCATOR", "// UWP backing uses independent" + allocator)
        return native_test

    def test_allocator_rejects_incompatible_view_protection_with_error_87(self):
        self.compile_and_run(self.allocator_source())

    def test_probe_error_87_logs_exact_call_and_releases_arena(self):
        native_test = self.allocator_source().split("int main() {", 1)[0]
        native_test += r"""
int main() {
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
        assert(IsFastmemEnabled());
        setenv("NXBOX_FASTMEM", "0", 1);
        assert(!IsFastmemEnabled());
        setenv("NXBOX_FASTMEM", "1", 1);
        assert(IsFastmemEnabled());
    }
    unsetenv("NXBOX_FASTMEM");
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
