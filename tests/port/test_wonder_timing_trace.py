# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production flight recorder and clock methods without an Xbox build."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1 : end - 1]


class WonderTimingTrace(unittest.TestCase):
    def compile_and_run(self, driver, environments):
        with tempfile.TemporaryDirectory(prefix="nxbox-wonder-timing-") as directory:
            source = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            source.write_text(driver)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
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
            for value in environments:
                environment = os.environ.copy()
                environment.pop("NXBOX_TIMING_TRACE", None)
                if value is not None:
                    environment["NXBOX_TIMING_TRACE"] = value
                subprocess.run([str(binary)], env=environment, check=True, timeout=15)

    def test_opt_in_bounded_retention_and_concurrent_producers(self):
        self.compile_and_run(
            r"""
#include "core/core_timing_trace.h"
#include <cassert>
using namespace Core::Timing::Trace;
int main() {
    const char* value = std::getenv("NXBOX_TIMING_TRACE");
    const bool expected = value && std::string_view{value} == "1";
    assert(Enabled() == expected);
    Record(Source::ClockTicks, 19200000, 19200000, 7);
    auto batch = FlightRecorder().Take(Source::ClockTicks);
    assert(batch.samples.size() == (expected ? 1 : 0));
    if (expected) {
        assert(batch.samples[0].value == 19200000);
        assert(batch.samples[0].detail == 19200000 && batch.samples[0].id == 7);
    }
    Recorder recorder;
    for (std::size_t i=0; i<Recorder::Capacity+9; ++i)
        recorder.Record(Source::ClockTicks, i);
    recorder.Record(Source::VsyncSignal, 0, 0, 42);
    batch = recorder.Take(Source::ClockTicks);
    assert(batch.overwritten == 9 && batch.contended == 0);
    assert(batch.samples.size() == Recorder::Capacity);
    for (std::size_t i=0; i<batch.samples.size(); ++i) {
        assert(batch.samples[i].value == i+9);
        if (i) assert(batch.samples[i].host_ns >= batch.samples[i-1].host_ns);
    }
    assert(recorder.Take(Source::ClockTicks).samples.empty());
    assert(recorder.Take(Source::VsyncSignal).samples[0].id == 42);
    constexpr std::size_t writers=4, per_writer=10000;
    std::vector<std::jthread> threads;
    for (std::size_t i=0; i<writers; ++i) {
        threads.emplace_back([&] {
            for (std::size_t j=0; j<per_writer; ++j)
                recorder.Record(Source::GpuTicks, j);
        });
    }
    threads.clear();
    batch=recorder.Take(Source::GpuTicks);
    assert(batch.samples.size()+batch.overwritten+batch.contended == writers*per_writer);
    assert(recorder.Take(Source::GpuTicks).contended == 0);
}
""",
            [None, "0", "true", "1"],
        )

    def test_native_timestamp_conversion_is_nanoseconds_and_monotonic(self):
        cases = []
        for frequency in (19_200_000, 614_400_000, 1_000_000_000, 3_579_545, 2**63 + 9):
            ticks = sorted({0, 1, frequency // 60, frequency // 30, frequency - 1, frequency})
            for tick in ticks:
                expected = tick * 1_000_000_000 // frequency
                cases.append(
                    f"assert(D3D12::TimestampNanoseconds({tick}ULL, {frequency}ULL) "
                    f"== {expected}ULL);"
                )
        self.compile_and_run(
            '#include "video_core/renderer_d3d12/d3d12_query_helpers.h"\n'
            "#include <cassert>\nint main() {\n" + "\n".join(cases) + "\n}\n",
            [None],
        )

    def test_production_clock_values_are_preserved(self):
        source = (ROOT / "src/core/core_timing.cpp").read_text()
        driver = r"""
#include "core/core_timing_trace.h"
#include <cassert>
using u64=std::uint64_t; using u32=std::uint32_t; using f64=double;
namespace Settings {
struct Number { double value; double GetValue() const { return value; } };
struct Values { Number cpu_clock{1020}, sync_core_speed{0}; } values;
double speed=100;
double SpeedLimit() { return speed; }
}
namespace Common {
struct WallClock {
    u64 GetCNTPCT() { return 19200000; }
    u64 GetGPUTick() { return 614400000; }
    static u64 CPUTickToCNTPCT(u64 value) { return value/32; }
    static u64 CPUTickToGPUTick(u64 value) { return value; }
} g_wall_clock;
}
namespace Core::Timing {
constexpr u32 CPU_CLOCK_BASE_MHZ=1020;
u32 CpuClockTargetMhz(double value) { return u32(value); }
struct CoreTiming {
    bool is_multicore=true;
    u64 cpu_ticks=614400000;
    u64 GetClockTicks() const { __CLOCK__ }
    u64 GetGPUTicks() const { __GPU__ }
};
}
int main() {
    using namespace Core::Timing;
    CoreTiming clock;
    for (bool multicore : {true, false}) {
        clock.is_multicore=multicore;
        Settings::values.cpu_clock.value=1020;
        Settings::values.sync_core_speed.value=0;
        assert(clock.GetClockTicks()==19200000);
        assert(clock.GetGPUTicks()==614400000);
        Settings::values.cpu_clock.value=2040;
        assert(clock.GetClockTicks()==38400000);
        Settings::values.sync_core_speed.value=1;
        Settings::speed=200;
        assert(clock.GetClockTicks()==19200000);
        // GPU clock is independent of CPU scaling and speed-limit settings.
        assert(clock.GetGPUTicks()==614400000);
    }
    auto batch=Trace::FlightRecorder().Take(Trace::Source::ClockTicks);
    assert(batch.samples.size()==(Trace::Enabled() ? 6 : 0));
    for (auto& sample : batch.samples) assert(sample.detail==19200000);
}
"""
        driver = driver.replace("__CLOCK__", body(source, "u64 CoreTiming::GetClockTicks()"))
        driver = driver.replace("__GPU__", body(source, "u64 CoreTiming::GetGPUTicks()"))
        self.compile_and_run(driver, [None, "1"])


if __name__ == "__main__":
    unittest.main()
