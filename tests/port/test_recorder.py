# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise the production recorder policies without Windows or a GPU."""

import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run


class RecorderTests(unittest.TestCase):
    def test_names_options_shortcut_ring_and_timeline(self):
        driver = r"""
#include <cassert>
#include <thread>
#include <vector>
#include "eden_uwp/recorder_policy.h"
#include "eden_uwp/stick_motion.h"
int main(int, char** argv) {
    using namespace EdenXbox::Recording;
    std::tm time{};
    time.tm_year = 126; time.tm_mon = 9; time.tm_mday = 21;
    time.tm_hour = 3; time.tm_min = 4; time.tm_sec = 5;
    assert(FileName("0100ABCDEF123456", time) == "0100ABCDEF123456-20261021-030405.mp4");
    assert(FileName("../escape", time) == "0000000000000000-20261021-030405.mp4");
    assert(FileName("../../foo1234567", time).find("..") == std::string::npos);
    assert(Seconds(nullptr, 120) == 120 && Seconds("0") == 0);
    assert(Seconds("12") == 12 && Seconds("99999999999999999") == 120);
    for (auto text : {"-1", "1.5", "nan", "5x", " 5"}) assert(Seconds(text, 17) == 17);
    const std::filesystem::path local(argv[1]);
    assert(ReadOptions(local).mode == 0);
    for (unsigned mode = 0; mode <= 4; ++mode) {
        assert(SaveOptions(local, {mode}));
        const auto options = ReadOptions(local);
        assert(options.mode == mode);
        assert(options.Width() == (mode >= 3 ? 1920u : 1280u));
        assert(options.Height() == (mode >= 3 ? 1080u : 720u));
        assert(options.Fps() == (mode == 2 || mode == 4 ? 60u : 30u));
    }
    { std::ofstream out(local / "recording.txt"); out << "99999"; }
    assert(ReadOptions(local).mode == 0);
    assert(!SaveOptions(local / "missing", {1}));
    Shortcut shortcut;
    assert(!shortcut.Update(true, false, false));
    assert(shortcut.Update(true, true, false));
    assert(!shortcut.Update(true, true, false));
    assert(!shortcut.Update(true, true, true)); // HOME wins.
    assert(!shortcut.Update(false, false, false));
    assert(shortcut.Update(true, true, false));
    assert(shortcut.consume_stick);
    assert(!shortcut.Update(false, true, false));
    assert(shortcut.consume_stick); // Releasing View first must not leak the click to the game.
    assert(!shortcut.Update(false, false, false) && !shortcut.consume_stick);
    assert(!shortcut.Update(false, true, false));
    assert(!shortcut.Update(true, true, false)); // View requires a fresh right-stick click.
    EdenXbox::ViewGesture motion;
    const auto now = EdenXbox::ViewGesture::Clock::now();
    motion.Update(true, 1, 0, now);
    motion.CancelTap();
    assert(!motion.Update(false, 0, 0, now + std::chrono::milliseconds(100)));
    Ring ring;
    const int a = ring.Acquire(), b = ring.Acquire(), c = ring.Acquire();
    assert(a >= 0 && b >= 0 && c >= 0 && a != b && b != c && a != c);
    for (int i = 0; i < 20; ++i) assert(ring.Acquire() == -1);
    assert(ring.drops == 20);
    ring.Release(b);
    assert(ring.Acquire() == b); // GPU/worker-owned a and c are never overwritten.
    assert(ring.Acquire() == -1);
    ring.Release(a); ring.Release(b); ring.Release(c);
    // Exercise actual acquire/release memory ordering between the GPU and encoder threads.
    Ring concurrent;
    std::array<unsigned, Slots> values{};
    std::array<std::atomic<bool>, Slots> published{};
    std::atomic<bool> done{};
    std::atomic<unsigned> consumed{};
    std::thread consumer([&] {
        while (!done || consumed < 10000) {
            for (unsigned i = 0; i < Slots; ++i) {
                if (!published[i].load(std::memory_order_acquire)) continue;
                assert(values[i] > 0 && values[i] <= 10000);
                published[i].store(false, std::memory_order_release);
                concurrent.Release(i);
                ++consumed;
            }
        }
    });
    for (unsigned value = 1; value <= 10000;) {
        const int i = concurrent.Acquire();
        if (i < 0) { std::this_thread::yield(); continue; }
        values[i] = value++;
        published[i].store(true, std::memory_order_release);
    }
    done = true; consumer.join(); assert(consumed == 10000);
    Timeline timeline;
    timeline.automatic = true; timeline.duration = 2; timeline.delay = 3;
    assert(!timeline.AutoStart(100)); // First game frame starts the delay clock.
    assert(!timeline.AutoStart(102.99));
    assert(timeline.AutoStart(103));
    assert(!timeline.AutoStart(200)); // Exactly once per game, including after manual stop.
    timeline.Start(103);
    assert(timeline.Due(103) && !timeline.Due(103.001));
    assert(timeline.Due(103 + 1./30));
    assert(timeline.Due(104.5)); // Stall: one frame, never a backlog burst.
    assert(!timeline.Due(104.5));
    assert(timeline.Timestamp(104.5) == 15000000);
    assert(!timeline.Expired(104.99) && timeline.Expired(105));
    Timeline disabled;
    assert(!disabled.AutoStart(0));
    for (unsigned fps : {30, 60}) {
        Timeline cadence; cadence.fps = fps; cadence.Start(1000);
        unsigned frames = 0;
        for (unsigned i = 0; i < 600; ++i)
            frames += cadence.Due(1000 + i / 60.);
        assert(frames == fps * 10);
    }
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-recorder-") as directory:
            compile_run(driver, Path(directory), extra_flags=["-Wall", "-Wextra", "-Werror"])

    def test_optional_allocation_never_waits_or_retries(self):
        # Compile the real Context method with a device that can inject memory/map
        # failures. There is deliberately no Finish/Wait/pool API in this fixture.
        root = Path(__file__).resolve().parents[2]
        context = (root / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        method = (
            "ResourcePtr Context::CreateResourceNoWait("
            + context.split("ResourcePtr Context::CreateResourceNoWait(", 1)[1].split(
                "ResourcePtr Context::CreateResource(", 1
            )[0]
        )
        driver = r"""
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
using u64 = std::uint64_t;
using u8 = unsigned char;
using SIZE_T = std::size_t;
constexpr int D3D12_HEAP_TYPE_READBACK = 1;
constexpr int D3D12_HEAP_FLAG_NONE = 0;
using D3D12_HEAP_TYPE = int;
using D3D12_RESOURCE_STATES = int;
struct D3D12_RESOURCE_DESC { u64 Width; };
struct D3D12_HEAP_PROPERTIES { int Type, CreationNodeMask, VisibleNodeMask; };
struct D3D12_RANGE { SIZE_T Begin, End; };
struct MemoryBudget { std::atomic<u64> used{}; };
struct FakeResource {
    std::array<u8, 64> bytes{};
    int map_result{};
    int Map(int, const D3D12_RANGE*, void** out) {
        *out = bytes.data(); return map_result;
    }
};
struct Resource {
    std::shared_ptr<FakeResource> handle;
    int heap{}, state{};
    u64 allocation_size{};
    std::shared_ptr<MemoryBudget> budget;
    std::span<u8> mapped;
    ~Resource() { if (budget) budget->used -= allocation_size; }
};
using ResourcePtr = std::shared_ptr<Resource>;
struct Device {
    struct Allocation { u64 SizeInBytes; };
    u64 size{64}; int creates{}, result{}, map_result{};
    Allocation GetResourceAllocationInfo(int, int, const D3D12_RESOURCE_DESC*) { return {size}; }
    int CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, int, const D3D12_RESOURCE_DESC*,
                                int, void*, std::shared_ptr<FakeResource>* out) {
        ++creates;
        if (!result) { *out = std::make_shared<FakeResource>(); (*out)->map_result = map_result; }
        return result;
    }
};
struct DevicePtr {
    Device value;
    Device* operator->() { return &value; }
    Device* Get() { return &value; }
};
u64 NativeBufferMemoryTarget(u64) { return 256; }
void Check(int result, const char*, Device*) { if (result) throw std::runtime_error("injected"); }
#define IID_PPV_ARGS(x) (x)
struct Context {
    DevicePtr device;
    std::shared_ptr<MemoryBudget> budget = std::make_shared<MemoryBudget>();
    ResourcePtr CreateResourceNoWait(const D3D12_RESOURCE_DESC&, int, int);
};
"""
        driver += method
        driver += r"""
int main() {
    Context c;
    const D3D12_RESOURCE_DESC desc{64};
    auto r = c.CreateResourceNoWait(desc, D3D12_HEAP_TYPE_READBACK, 3);
    assert(c.device.value.creates == 1 && c.budget->used == 64);
    assert(r->heap == D3D12_HEAP_TYPE_READBACK && r->state == 3 && r->mapped.size() == 64);
    r.reset(); assert(c.budget->used == 0);
    auto fails = [&] {
        bool threw = false;
        try { c.CreateResourceNoWait(desc, D3D12_HEAP_TYPE_READBACK, 3); }
        catch (const std::runtime_error&) { threw = true; }
        assert(threw);
    };
    c.budget->used = 230;
    fails(); assert(c.device.value.creates == 1 && c.budget->used == 230);
    c.budget->used = 0; c.device.value.size = UINT64_MAX;
    fails(); assert(c.device.value.creates == 1);
    c.device.value.size = 64; c.device.value.result = -1;
    fails(); assert(c.device.value.creates == 2 && c.budget->used == 0); // No OOM retry.
    c.device.value.result = 0; c.device.value.map_result = -1;
    fails(); assert(c.device.value.creates == 3 && c.budget->used == 0);
}
"""
        with tempfile.TemporaryDirectory(prefix="nxbox-recorder-alloc-") as directory:
            compile_run(driver, Path(directory), extra_flags=["-Wall", "-Wextra", "-Werror"])
