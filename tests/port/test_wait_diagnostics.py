# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stopped producer must not hide blocked calls or completed decode work."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class WaitDiagnosticsTests(unittest.TestCase):
    def test_snapshot_without_guest_or_gpu_progress(self):
        driver = r"""
#include <cassert>
#include <cstring>
#include <thread>
#include "common/nxbox_wait_diag.h"
using namespace NxboxWait;
int main() {
    const std::array<std::uint64_t, 2> args{0x42, 500000000};
    const auto blocked = Begin(Kind::Svc, 0x54, "11", args, 0x80001234, 0x80005678);
    Add(Counter::DecodeQueued);
    Add(Counter::DecodeStarted);
    // Decoder completion does not require another frame; upload retirement does.
    std::thread decoder([] { Add(Counter::DecodeCompleted); });
    decoder.join();
    const auto pending_query = BeginPending(PendingKind::Query, 0x1000, 37410);
    const auto pending_sync = BeginPending(PendingKind::Syncpoint, 1, 37408);
    if (!Enabled()) {
        assert(pending_query == 0 && pending_sync == 0 && Pending().Read().entries.empty());
        assert(blocked == 0 && recorder.Read().entries.empty());
        for (const auto& value : counters) assert(value == 0);
        return 0;
    }
    // Independent heartbeat copies retain the original age while the producer is idle.
    const auto before = Pending().Read();
    assert(before.entries.size() == 2 && before.dropped == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    auto pending = Pending().Read();
    for (const auto& entry : pending.entries) {
        assert(NowMs() - entry.began_ms >= 10);
        assert(entry.tick == (entry.kind == PendingKind::Query ? 37410 : 37408));
    }
    std::thread retirement([&] { EndPending(pending_query); });
    retirement.join();
    EndPending(pending_query); // Late completion cannot close another operation.
    pending = Pending().Read();
    assert(pending.entries.size() == 1 && pending.entries[0].token == pending_sync);
    assert(before.entries.size() == 2);
    EndPending(pending_sync);
    assert(Pending().Read().entries.empty());
    for (unsigned i = 0; i < 8200; ++i) BeginPending(PendingKind::Query, i, 42);
    assert(Pending().Read().entries.size() == 8192 && Pending().Read().dropped == 8);
    auto snapshot = recorder.Read();
    assert(snapshot.entries.size() == 1 && snapshot.dropped == 0);
    const auto entry = snapshot.entries.front();
    assert(entry.key == 0x54 && !entry.ended_ms && entry.args[1] == 500000000);
    assert(entry.pc == 0x80001234 && entry.lr == 0x80005678);
    assert(counters[static_cast<size_t>(Counter::DecodeCompleted)] == 1);
    assert(counters[static_cast<size_t>(Counter::DecodeUploaded)] == 0);
    // Blocking SVC returns on a different physical core. The token follows the guest.
    std::thread other_core([&] { End(blocked, 0xea01); });
    other_core.join();
    auto finished = recorder.Read().entries.front();
    assert(finished.ended_ms >= finished.began_ms && finished.result == 0xea01);
    assert(!entry.ended_ms); // The old snapshot owns a copy, not a guest pointer.
    const auto next = Begin(Kind::Svc, 0x54, "33", args);
    End(blocked); // A late completion must not close the newer call.
    snapshot = recorder.Read();
    assert(snapshot.entries.size() == 1 && snapshot.entries[0].token == next);
    assert(!snapshot.entries[0].ended_ms);
    Max(Counter::SubmittedTick, 120);
    Max(Counter::SubmittedTick, 119); // Concurrent/late observations cannot regress ticks.
    assert(counters[static_cast<size_t>(Counter::SubmittedTick)] == 120);
    const auto event = Begin(Kind::NvEvent, 7, "nvhost_event", args);
    recorder.EndKey(Kind::NvEvent, 7, 1);
    End(event); // A late signal must not overwrite a recorded cancellation.
    for (const auto& value : recorder.Read().entries)
        if (value.token == event) assert(value.result == 1);
    // Bound storage, report loss, and retain active waits under pressure.
    for (unsigned i = 0; i < 300; ++i) Begin(Kind::Service, i, "fs");
    snapshot = recorder.Read();
    assert(snapshot.entries.size() == 256 && snapshot.dropped > 0);
    bool retained = false;
    for (const auto& value : snapshot.entries)
        if (value.token == next) retained = !value.ended_ms;
    assert(retained);
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-wait-diag-") as directory:
            source = Path(directory) / "wait.cpp"
            binary = Path(directory) / "wait"
            source.write_text(driver)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-pthread",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            for enabled in ("0", "1"):
                subprocess.run(
                    [str(binary)],
                    env=dict(os.environ, NXBOX_WAIT_DIAG=enabled),
                    check=True,
                    timeout=10,
                )

    def test_real_syncpoint_wait_remains_visible_without_new_frames(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-syncpoint-diag-") as directory:
            temp = Path(directory)
            (temp / "common").mkdir()
            (temp / "common/common_types.h").write_text(
                "#pragma once\n#include <cstdint>\nusing u32 = std::uint32_t;\n"
            )
            (temp / "common/gpu_thread_profile.h").write_text(
                "#pragma once\nnamespace Common::GpuProfile {\n"
                "using Tick = unsigned long long;\n"
                "enum class Wait { SyncpointHost, SyncpointGuest };\n"
                "struct WaitScope { explicit WaitScope(Wait) {} }; }\n"
            )
            source = temp / "syncpoint.cpp"
            source.write_text(r"""
#include <cassert>
#include <thread>
#include "common/nxbox_wait_diag.h"
#include "video_core/host1x/syncpoint_manager.h"
int main() {
    Tegra::Host1x::SyncpointManager manager;
    std::thread gpu([&] { manager.WaitHost(7, 1); });
    const auto deadline = NxboxWait::NowMs() + 2000;
    bool observed = false;
    while (!observed && NxboxWait::NowMs() < deadline) {
        for (const auto& entry : NxboxWait::recorder.Read().entries) {
            if (entry.kind == NxboxWait::Kind::Syncpoint && entry.key == 7) {
                assert(!entry.ended_ms && entry.args[0] == 1 && entry.args[1] == 0);
                observed = true;
            }
        }
        std::this_thread::yield();
    }
    // Unblock/join even when the assertion below would fail.
    manager.IncrementHost(7);
    gpu.join();
    assert(observed);
    const auto snapshot = NxboxWait::recorder.Read();
    assert(snapshot.entries.size() == 1 && snapshot.entries[0].ended_ms);
}
""")
            binary = temp / "syncpoint"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-pthread",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(temp),
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    str(ROOT / "src/video_core/host1x/syncpoint_manager.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                [str(binary)],
                env=dict(os.environ, NXBOX_WAIT_DIAG="1"),
                check=True,
                timeout=10,
            )

    def test_fatal_frame_walk_is_bounded_and_uses_silent_reads(self):
        source = (ROOT / "src/core/memory.cpp").read_text()
        walk = source.split("        u64 frame = ctx.fp;", 1)[1]
        walk = "u64 frame = ctx.fp;" + walk.split("        // Silent byte reads", 1)[0]
        driver = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <vector>
using u64 = std::uint64_t;
struct Frame { size_t depth; u64 fp; bool mapped; u64 next; u64 pc; };
struct Memory {
    struct { u64 fp = 0x1000, sp = 0x1000; } ctx;
    std::map<u64, unsigned char> bytes;
    std::vector<Frame> frames;
    const unsigned char* GetPointerSilent(u64 address) {
        const auto it = bytes.find(address);
        return it == bytes.end() ? nullptr : &it->second;
    }
    void Record(size_t depth, u64 fp, bool mapped, u64 next, u64 pc) {
        frames.push_back({depth, fp, mapped, next, pc});
    }
    void Put(u64 address, u64 next, u64 pc) {
        for (unsigned i = 0; i < 8; ++i) {
            bytes[address + i] = next >> (8 * i);
            bytes[address + 8 + i] = pc >> (8 * i);
        }
    }
    void Walk() {
#define LOG_ERROR(category, format, ...) Record(__VA_ARGS__)
__WALK__
#undef LOG_ERROR
    }
};
int main() {
    Memory memory;
    for (unsigned i = 0; i < 20; ++i)
        memory.Put(0x1000 + 16 * i, 0x1010 + 16 * i, 0x80000000 + 4 * i);
    memory.Walk();
    assert(memory.frames.size() == 16 && memory.frames[15].pc == 0x8000003c);
    memory.frames.clear();
    memory.Put(0x1000, 0x1000, 0x80001234); // Self-cycle.
    memory.Walk();
    assert(memory.frames.size() == 1 && memory.frames[0].pc == 0x80001234);
    memory.frames.clear();
    memory.bytes.erase(0x1008); // Missing return-address page/byte.
    memory.Walk();
    assert(memory.frames.size() == 1 && !memory.frames[0].mapped);
    memory.frames.clear();
    memory.ctx.fp = 0x1001; // Misaligned frame.
    memory.Walk();
    assert(memory.frames.empty());
}
""".replace("__WALK__", walk)
        with tempfile.TemporaryDirectory(prefix="nxbox-fatal-frames-") as directory:
            source = Path(directory) / "frames.cpp"
            binary = Path(directory) / "frames"
            source.write_text(driver)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_stall_capture_does_not_dispatch_to_the_stalled_gpu(self):
        source = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        block = source.split('std::string progress = "WAIT_PROGRESS";', 1)[1]
        block = block.split("const auto stats =", 1)[0]
        self.assertIn("if (frames == measured_frames)", block)
        self.assertIn("NxboxWait::recorder.Read()", block)
        self.assertNotIn("rasterizer", block)
        self.assertNotIn("RunGpuPipeline", block)


if __name__ == "__main__":
    unittest.main()
