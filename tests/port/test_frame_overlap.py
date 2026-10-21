# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay production syncpoint publication while presentation is blocked."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_depth_recording import function_body

ROOT = Path(__file__).resolve().parents[2]


class FrameOverlapTests(unittest.TestCase):
    def test_fence_precedes_present_but_host_waits_for_valid_data(self):
        method = function_body(
            (ROOT / "src/video_core/renderer_d3d12/d3d12_fence_manager.cpp").read_text(),
            "void FenceManager::SignalSyncPoint",
        )
        program = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <vector>
#include "common/gpu_thread_profile.h"
#include "common/nxbox_wait_diag.h"
#include "video_core/host1x/syncpoint_manager.h"
using namespace std::chrono_literals;
using u64 = std::uint64_t;
namespace D3D12 {
bool BisectEnabled(const char* name) {
    const auto* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}
class FenceManager {
public:
    Tegra::Host1x::SyncpointManager syncpoint_manager;
    struct { u64 PendingFenceTick() { return 1; } } context;
    std::atomic<u64> pending_syncpoints{}, signaled_syncpoints{};
    std::promise<void> published;
    std::function<void()> completion;
    void LogSyncpoints() const {}
    void SignalFence(std::function<void()>&& callback) {
        assert(syncpoint_manager.GetGuestSyncpointValue(0) == 1);
        assert(syncpoint_manager.GetHostSyncpointValue(0) == 0);
        // Publication must not own the syncpoint lock.
        syncpoint_manager.IncrementGuest(1);
        completion = std::move(callback);
        published.set_value();
    }
    void SignalSyncPoint(u32 value);
};
// METHOD
}
int main() {
    for (const char* option : {"", "0", "1", "invalid"}) {
        for (bool profile : {false, true}) {
            if (*option) setenv("NXBOX_D3D12_OVERLAP_SYNCPOINT", option, 1);
            else unsetenv("NXBOX_D3D12_OVERLAP_SYNCPOINT");
            const bool overlap = std::strcmp(option, "1") == 0;
            Common::GpuProfile::enabled = profile;
            D3D12::FenceManager manager;
            auto published = manager.published.get_future();
            std::promise<void> entered, release;
            auto presented = entered.get_future();
            auto may_return = release.get_future();
            int guest_data = 0;
            unsigned host_events = 0;
            manager.syncpoint_manager.RegisterHostAction(0, 1, [&] {
                assert(guest_data == 42); // Readback/cache retirement precedes host wake.
                ++host_events;
            });
            unsigned actions = 0;
            manager.syncpoint_manager.RegisterGuestAction(0, 2, [&] { ++actions; });
            manager.syncpoint_manager.RegisterGuestAction(0, 1, [&] {
                ++actions;
                entered.set_value();
                assert(may_return.wait_for(3s) == std::future_status::ready);
            });
            auto producer = std::async(std::launch::async, [&] { manager.SignalSyncPoint(0); });
            assert(presented.wait_for(3s) == std::future_status::ready);
            assert(manager.syncpoint_manager.GetGuestSyncpointValue(0) == 1);
            assert(host_events == 0);
            assert(manager.syncpoint_manager.GetHostSyncpointValue(0) == 0);
            if (overlap) {
                assert(published.wait_for(3s) == std::future_status::ready);
                assert(manager.pending_syncpoints == 1);
                // A real fence completion can now wake nvdrv while Present is blocked.
                guest_data = 42;
                manager.completion();
                assert(host_events == 1 && manager.pending_syncpoints == 0);
                assert(producer.wait_for(0s) != std::future_status::ready);
            } else {
                // Legacy ordering has not even captured/submitted the fence yet.
                assert(published.wait_for(0s) != std::future_status::ready);
            }
            release.set_value();
            assert(producer.wait_for(3s) == std::future_status::ready);
            producer.get();
            if (!overlap) {
                assert(published.wait_for(0s) == std::future_status::ready);
                guest_data = 42;
                manager.completion();
            }
            assert(host_events == 1 && manager.signaled_syncpoints == 1);
            assert(actions == 1);
            manager.syncpoint_manager.IncrementGuest(0);
            assert(actions == 2); // Future actions were not lost or run early.
        }
    }
    Tegra::Host1x::SyncpointManager empty;
    unsigned publications = 0;
    empty.IncrementGuest(3, [&] {
        assert(empty.GetGuestSyncpointValue(3) == 1);
        ++publications;
    });
    assert(publications == 1); // Publication also runs without a ready action.
}
""".replace("// METHOD", method)
        with tempfile.TemporaryDirectory(prefix="nxbox-overlap-") as directory:
            root = Path(directory)
            common = root / "common"
            common.mkdir()
            (common / "common_types.h").write_text(
                "#pragma once\n#include <cstdint>\nusing u32 = std::uint32_t;\n"
            )
            (common / "gpu_thread_profile.h").write_text(
                "#pragma once\n#include <atomic>\nnamespace Common::GpuProfile {\n"
                "using Tick = unsigned long long;\n"
                "enum class Wait { SyncpointHost, SyncpointGuest };\n"
                "struct WaitScope { explicit WaitScope(Wait) {} };\n"
                "inline std::atomic<bool> enabled{};\n"
                "inline int ProcessFence(unsigned, unsigned) { return 0; }\n"
                "inline void CompleteFence(int) {}\n}\n"
            )
            source = root / "test.cpp"
            source.write_text(program)
            binary = root / "test"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-pthread",
                    "-fsanitize=address,undefined",
                    f"-I{root}",
                    f"-I{ROOT / 'src'}",
                    str(source),
                    str(ROOT / "src/video_core/host1x/syncpoint_manager.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
