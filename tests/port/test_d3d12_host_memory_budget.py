# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise host-budget thresholds and the production per-frame texture GC hook."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


class D3D12HostMemoryBudgetTests(unittest.TestCase):
    def run_driver(self, source):
        with tempfile.TemporaryDirectory(prefix="nxbox-host-budget-") as folder:
            cpp = Path(folder) / "budget.cpp"
            binary = Path(folder) / "budget"
            cpp.write_text(source)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_shared_host_budget_reserves_cpu_headroom(self):
        self.run_driver(r"""
#include <cassert>
#include <cstdint>
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
int main() {
    using D3D12::TextureMemoryThresholds;
    constexpr std::uint64_t mib = 1ULL << 20;
    auto budget = TextureMemoryThresholds(5120*mib, 3000*mib, 512*mib);
    assert(budget.expected == 320*mib && budget.critical == 448*mib);
    // CPU/JIT growth consumes the same physical budget even with a small GPU cache.
    budget = TextureMemoryThresholds(5120*mib, 4900*mib, 156*mib);
    assert(budget.expected == 56*mib && budget.critical == 120*mib);
    budget = TextureMemoryThresholds(5120*mib, 5100*mib, 512*mib);
    assert(budget.expected == 212*mib && budget.critical == 276*mib);
    // Saturation cannot wrap a near-limit process into an enormous cache target.
    budget = TextureMemoryThresholds(5120*mib, 6000*mib, 512*mib);
    assert(budget.expected == 0 && budget.critical == 0);
    budget = TextureMemoryThresholds(0, UINT64_MAX, 1);
    assert(budget.expected == 0 && budget.critical == 0);
    budget = TextureMemoryThresholds(UINT64_MAX, 0, UINT64_MAX);
    assert(budget.expected == 320*mib && budget.critical == 448*mib);
    // Returning memory to the OS restores the ordinary watermarks.
    budget = TextureMemoryThresholds(5120*mib, 4000*mib, 156*mib);
    assert(budget.expected == 320*mib && budget.critical == 448*mib);
}
""")

    def test_native_adapter_and_telemetry_failure(self):
        source = (BACKEND / "d3d12_context.cpp").read_text()
        adapter = source.split("TextureMemoryBudget NativeTextureMemoryBudget(", 1)[1].split(
            "u64 NativeBufferMemoryTarget", 1
        )[0]
        driver = r"""
#include <cassert>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
using u64 = std::uint64_t;
namespace winrt::Windows::System {
struct MemoryManager {
    static inline bool fail = false;
    static unsigned AppMemoryUsageLevel() { return 0; }
    static u64 AppMemoryUsageLimit() {
        if (fail) throw std::runtime_error("telemetry unavailable");
        return 5120ULL << 20;
    }
    static u64 AppMemoryUsage() { return 4900ULL << 20; }
};
}
namespace D3D12 {
TextureMemoryBudget NativeTextureMemoryBudget(ADAPTER
}
int main() {
    auto budget = D3D12::NativeTextureMemoryBudget(156ULL << 20);
#ifdef NXBOX_UWP
    assert(budget.expected == (56ULL << 20) && budget.critical == (120ULL << 20));
#else
    assert(budget.expected == (320ULL << 20) && budget.critical == (448ULL << 20));
#endif
    winrt::Windows::System::MemoryManager::fail = true;
    budget = D3D12::NativeTextureMemoryBudget(156ULL << 20);
    assert(budget.expected == (320ULL << 20) && budget.critical == (448ULL << 20));
}
""".replace("ADAPTER", adapter)
        self.run_driver(driver)
        self.run_driver("#define NXBOX_UWP 1\n" + driver)

    def test_texture_gc_refreshes_thresholds_before_collecting(self):
        source = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        tick = source.split("void TextureCache<P>::TickFrame() {", 1)[1].split(
            "\ntemplate <class P>", 1
        )[0]
        self.run_driver(
            r"""
#define NXBOX_UWP 1
#include <cassert>
#include <cstdint>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_memory_budget.h"
using u64 = std::uint64_t;
constexpr u64 mib = 1ULL << 20;
namespace PassDump {
struct Config { bool enabled = false; u64 frame = 0; };
Config GetConfig() { return {}; }
}
namespace NxboxStall {
enum class Kind { GarbageCollect };
struct Scope { Scope(Kind) {} };
}
struct RuntimeBase {
    u64 host_usage = 3000*mib, gpu_usage = 156*mib;
    bool CanReportMemoryUsage() const { return true; }
    u64 GetDeviceMemoryUsage() const { return gpu_usage; }
    void TickFrame() {}
    void FreeDeferredStagingBuffer(int&) {}
};
struct Runtime : RuntimeBase {
    void ApplyMemoryBudgetOverride(u64& expected, u64& critical) {
        auto budget = D3D12::TextureMemoryThresholds(5120*mib, host_usage, gpu_usage);
        expected = budget.expected;
        critical = budget.critical;
    }
    void TickFrame() {}
    void FreeDeferredStagingBuffer(int&) {}
};
struct Ring { void Tick() {} };
template<class P> struct TextureCache {
    P runtime;
    u64 frame_tick = 100, total_used_memory = 0, minimum_memory = 0;
    u64 expected_memory = 320*mib, critical_memory = 448*mib;
    bool aggressive = false;
    Ring sentenced_images, sentenced_framebuffers, sentenced_image_view;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = false;
    std::vector<int> async_buffers_death_ring;
    void DumpPassRenderTargets() {}
    void ReportPassDumpHistory() {}
    void TickAsyncDecode() {}
    void TickAsyncUnswizzle() {}
    void RunGarbageCollector() { aggressive = total_used_memory >= critical_memory; }
    void TickFrame();
};
template<class P> void TextureCache<P>::TickFrame() {
"""
            + tick
            + r"""
int main() {
    TextureCache<Runtime> cache;
    cache.TickFrame();
    assert(!cache.aggressive);
    cache.runtime.host_usage = 4900*mib;
    cache.TickFrame();
    assert(cache.aggressive && cache.critical_memory == 120*mib);
    cache.runtime.host_usage = 3000*mib;
    cache.TickFrame();
    assert(!cache.aggressive && cache.critical_memory == 448*mib);
    // Backends without the optional hook retain their thresholds.
    TextureCache<RuntimeBase> plain;
    plain.runtime.host_usage = 4900*mib;
    plain.TickFrame();
    assert(!plain.aggressive && plain.critical_memory == 448*mib);
}
"""
        )


if __name__ == "__main__":
    unittest.main()
