# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Separate optional cache retirement from resource ordering and OOM recovery."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_view_casts import RESOURCE_DRIVER, method

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp"


class AllocationHeadroomTests(unittest.TestCase):
    def compile_and_run(self, source, environments=(None,), strict=True):
        with tempfile.TemporaryDirectory(prefix="nxbox-headroom-") as folder:
            cpp = Path(folder) / "headroom.cpp"
            binary = Path(folder) / "headroom"
            cpp.write_text(source)
            flags = ["-Wconversion"] if strict else []
            compiled = subprocess.run(
                ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Wshadow", "-Werror"]
                + flags
                + [
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for setting in environments:
                environment = dict(os.environ)
                environment.pop("NXBOX_D3D12_ALLOCATION_HEADROOM", None)
                if setting is not None:
                    environment["NXBOX_D3D12_ALLOCATION_HEADROOM"] = setting
                subprocess.run([str(binary)], env=environment, check=True, timeout=30)

    def test_policy_and_live_telemetry_with_latched_switch(self):
        adapter = method(SOURCE.read_text(), "bool NativeAllocationPressureWait(")
        self.compile_and_run(
            r"""
#define NXBOX_UWP 1
#include <cassert>
#include <cstring>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
using u64 = std::uint64_t;
constexpr u64 mib = 1ULL << 20;
namespace winrt::Windows::System {
struct MemoryManager {
    static inline u64 limit = 5120*mib, usage = 3000*mib;
    static inline bool fail = false;
    static u64 AppMemoryUsageLimit() {
        if (fail) throw std::runtime_error("unavailable");
        return limit;
    }
    static u64 AppMemoryUsage() { return usage; }
};
}
namespace D3D12 {
__ADAPTER__
}
int main() {
    using namespace D3D12;
    using M = winrt::Windows::System::MemoryManager;
    static_assert(!AllocationNeedsRetirement(5120*mib, 3000*mib, 16*mib));
    static_assert(AllocationNeedsRetirement(5120*mib, 4864*mib, 0));
    static_assert(!AllocationNeedsRetirement(5120*mib, 4864*mib-1, 0));
    static_assert(AllocationNeedsRetirement(5120*mib, 4800*mib, 64*mib));
    static_assert(!AllocationNeedsRetirement(5120*mib, 4800*mib, 64*mib-1));
    static_assert(AllocationNeedsRetirement(0, 0, 1));
    static_assert(AllocationNeedsRetirement(1, UINT64_MAX, 1));
    static_assert(AllocationNeedsRetirement(UINT64_MAX, 0, UINT64_MAX));
    static_assert(!AllocationNeedsRetirement(UINT64_MAX, 0, 1));
    const char* setting = std::getenv("NXBOX_D3D12_ALLOCATION_HEADROOM");
    const bool legacy = setting && std::strcmp(setting, "0") == 0;
    assert(NativeAllocationPressureWait(16*mib) == legacy);
    // The option latches; telemetry must not latch across allocations.
    setenv("NXBOX_D3D12_ALLOCATION_HEADROOM", legacy ? "1" : "0", 1);
    assert(NativeAllocationPressureWait(16*mib) == legacy);
    M::usage = 4900*mib;
    assert(NativeAllocationPressureWait(1));
    M::usage = 3000*mib;
    assert(NativeAllocationPressureWait(16*mib) == legacy);
    M::limit = 0;
    assert(NativeAllocationPressureWait(1));
    M::limit = 5120*mib; M::fail = true;
    assert(NativeAllocationPressureWait(1));
}
""".replace("__ADAPTER__", adapter),
            (None, "0", "1"),
        )
        self.compile_and_run(
            '#include <cassert>\n#include <cstring>\n#include "video_core/renderer_d3d12/d3d12_memory_budget.h"\n'
            + "using u64 = std::uint64_t;\nnamespace D3D12 {\n"
            + adapter
            + "\n}\nint main() { assert(D3D12::NativeAllocationPressureWait(1)); }",
        )

    def test_committed_allocation_preserves_creation_and_oom_retry(self):
        driver = RESOURCE_DRIVER.split("int main()", 1)[0]
        driver = (
            driver.replace(
                "bool NativeAllocationPressureWait(u64) noexcept { return true; }",
                "bool wait_needed = false; u64 requested_bytes = 0;\n"
                "bool NativeAllocationPressureWait(u64 bytes) noexcept { requested_bytes = bytes; return wait_needed; }",
            )
            .replace(
                "u64 NativeBufferMemoryTarget(u64) { return UINT64_MAX; }",
                "u64 NativeBufferMemoryTarget(u64) { return 0; }",
            )
            .replace(
                "struct Pool { void Trim() {} }",
                "struct Pool { int trims = 0; void Trim() { ++trims; } }",
            )
        )
        driver = driver.replace(
            "void Collect() {} void Finish(unsigned) {}",
            "int pressure_waits = 0, retry_waits = 0; void Collect() {}\n"
            "void Finish(unsigned reason) { if (reason == FrameCount::ForcedSubmitMemoryPressure) ++pressure_waits; else ++retry_waits; }",
        )
        source = SOURCE.read_text()
        production = method(source, "ResourcePtr Context::CreateResource(")
        heap_kind = method(source, "NxboxMemory::Kind HeapMemoryKind(")
        driver = '#include "common/maxwell_method_profile.h"\n' + driver
        driver = driver.replace("// CREATE_RESOURCE", heap_kind + "\n// CREATE_RESOURCE")
        driver = driver.replace("// CREATE_RESOURCE", production)
        driver += r"""
int main() {
    Context c;
    D3D12_RESOURCE_DESC desc;
    auto resource = c.CreateResource(desc, 0, 0);
    assert(c.pressure_waits == 0 && c.retry_waits == 0 && pool.trims == 1);
    assert(requested_bytes == 65536 && c.device->legacy == 1);
    assert(resource->allocation_size == 65536 && resource->budget == c.budget);
    wait_needed = true;
    resource = c.CreateResource(desc, 0, 0);
    assert(c.pressure_waits == 1 && c.device->legacy == 2 && pool.trims == 2);
    wait_needed = false;
    c.casting_device = c.device;
    c.device->fail = E_OUTOFMEMORY;
    resource = c.CreateResource(desc, 0, 0);
    assert(c.pressure_waits == 1 && c.retry_waits == 1);
    assert(c.device->modern == 2 && pool.trims == 4);
    assert(resource && resource->budget == c.budget);
}
"""
        # The inherited D3D format fixture has existing conversion warnings.
        self.compile_and_run(
            "#define __ID3D12Device10_INTERFACE_DEFINED__ 1\n" + driver, strict=False
        )


if __name__ == "__main__":
    unittest.main()
