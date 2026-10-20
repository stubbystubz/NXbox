# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production CounterReport with a delayed fence and a fake guest clock."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function_body(source, signature):
    start = source.index("{", source.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1 : end - 1]


class QueryReportTimestamp(unittest.TestCase):
    def test_processing_time_survives_delayed_publication(self):
        source = (ROOT / "src/video_core/query_cache/query_cache.h").read_text()
        body = function_body(source, "void QueryCacheBase<Traits>::CounterReport(")
        driver = r"""
#include "core/core_timing_trace.h"
#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <utility>
using u8=unsigned char; using u32=std::uint32_t; using u64=std::uint64_t;
using GPUVAddr=u64; using DAddr=u64; using VAddr=u64;
#define LOG_ERROR(...) assert(false)
namespace D3D12 { struct QueryCacheParams { static constexpr bool REQUIRE_GPU_COMPLETION=true; }; }
struct OtherTraits {};
namespace Core { constexpr u64 DEVICE_PAGEBITS=12, DEVICE_PAGEMASK=4095; }
namespace Settings {
bool high=true;
bool IsGPUFenceBehaviorDefault() { return true; }
bool IsGPULevelHigh() { return high; }
bool IsGPUFenceBehaviorBalanced() { return false; }
bool IsGPUFenceBehaviorAccurate() { return false; }
}
enum class QueryFlagBits : u32 {
    HasTimestamp=1, IsFence=2, IsRewritten=4, IsInvalidated=8,
    IsFinalValueSynced=16, IsGuestSynced=32
};
enum class QueryPropertiesFlags : u32 { HasTimeout=1, IsAFence=2 };
template<class T> T operator&(T a,T b) { return T(u32(a)&u32(b)); }
template<class T> T operator|(T a,T b) { return T(u32(a)|u32(b)); }
template<class T> T& operator|=(T& a,T b) { return a=a|b; }
template<class T> bool True(T v) { return u32(v)!=0; }
template<class T> bool False(T v) { return !True(v); }
enum class QueryType { Payload };
u64 now=123456;
int tick_reads=0;
struct Query { u64 value=99, guest_address=0; QueryFlagBits flags{}; };
struct Streamer {
    Query query; int freed=0;
    size_t WriteCounter(u64 address,bool timestamp,u32,u32) {
        query={99,address,QueryFlagBits::IsFinalValueSynced};
        if(timestamp) query.flags|=QueryFlagBits::HasTimestamp;
        now+=100; // Native processing may already consume time before returning.
        return 0;
    }
    Query* GetQuery(size_t) { return &query; }
    u64 GetAmendValue() { return 0; }
    void SetAccumulationValue(u64 value) { assert(value==99); }
    void Free(size_t) { ++freed; }
};
struct Memory {
    std::array<u8,32> bytes;
    template<class T> T* GetPointer(u64 address) { return reinterpret_cast<T*>(bytes.data()+address); }
    std::optional<u64> GpuToCpuAddress(u64 address) { return address; }
};
struct Part { u32 value; void Assign(u32 v) { value=v; } };
struct Location { Part stream_id,query_id; };
struct Rasterizer {
    std::function<void()> pending;
    void SignalFence(std::function<void()> f) { pending=std::move(f); }
    void SyncOperation(std::function<void()> f) { pending=std::move(f); }
};
struct Runtime {
    int pending=0,completed=0;
    u64 ReportPending(u64) { ++pending; return 7; }
    void ReportCompleted(bool invalidated,u64 token) { assert(token==7); completed+=invalidated?10:1; }
};
struct Gpu { u64 GetTicks() { ++tick_reads; return now; } };
struct Impl {
    Streamer streamer; std::array<Streamer*,1> streamers{&streamer};
    Memory device_memory; Rasterizer rasterizer; Runtime runtime; Gpu gpu;
    std::vector<Location> pending_unregister;
    Query* ObtainQuery(Location) { return &streamer.query; }
};
template<class Traits> struct QueryCacheBase {
    using QueryLocation=Location;
    Impl storage; Impl* impl=&storage;
    Memory* gpu_memory=&storage.device_memory;
    std::mutex cache_mutex;
    std::map<u64,std::map<u32,Location>> cached_queries;
    void CounterReport(GPUVAddr,QueryType,QueryPropertiesFlags,u32,u32);
};
template<class Traits>
void QueryCacheBase<Traits>::CounterReport(GPUVAddr addr,QueryType counter_type,
    QueryPropertiesFlags flags,u32 payload,u32 subreport) { __BODY__ }
template<class Traits> void Run(bool fence,bool high,bool timestamp,bool invalidated=false) {
    QueryCacheBase<Traits> cache;
    auto& state=cache.storage;
    Settings::high=high;
    state.device_memory.bytes.fill(0x55);
    now=123456;
    tick_reads=0;
    using namespace Core::Timing::Trace;
    FlightRecorder().Take(Source::QueryIssue);
    FlightRecorder().Take(Source::QueryPublish);
    auto flags=QueryPropertiesFlags{};
    if(fence) flags|=QueryPropertiesFlags::IsAFence;
    if(timestamp) flags|=QueryPropertiesFlags::HasTimeout;
    cache.CounterReport(0,QueryType::Payload,flags,99,0);
    const bool immediate=!fence&&!high;
    if(!immediate) {
        // Capturing the timestamp must not publish any part of the report early.
        for(auto byte:state.device_memory.bytes) assert(byte==0x55);
        assert(state.rasterizer.pending);
        assert(state.runtime.pending==1);
        now+=987654;
        if(invalidated) state.streamer.query.flags|=QueryFlagBits::IsInvalidated;
        state.rasterizer.pending();
    }
    if(invalidated) {
        for(auto byte:state.device_memory.bytes) assert(byte==0x55);
        assert(state.runtime.completed==10);
        return;
    }
    u64 tick{},value{};
    std::memcpy(&tick,state.device_memory.bytes.data()+8,8);
    std::memcpy(&value,state.device_memory.bytes.data(),timestamp?8:4);
    assert(value==99);
    const char* env=std::getenv("NXBOX_QUERY_TIMESTAMP_AT_PUBLISH");
    bool legacy=env && std::string_view{env}=="1";
    if constexpr(!std::is_same_v<Traits,D3D12::QueryCacheParams>) legacy=true;
    assert(tick==(timestamp?(legacy?now:123456):0x5555555555555555ULL));
    // Disabled tracing must not sample clocks or do work for diagnostics.
    if(!Core::Timing::Trace::Enabled()) assert(tick_reads==(timestamp?1:0));
    auto issued=FlightRecorder().Take(Source::QueryIssue);
    auto published=FlightRecorder().Take(Source::QueryPublish);
    assert(issued.samples.size()==(Enabled()&&timestamp?1:0));
    assert(published.samples.size()==(Enabled()&&timestamp?1:0));
    if(Enabled()&&timestamp) {
        assert(issued.samples[0].value==123456);
        assert(published.samples[0].value==tick);
        assert(published.samples[0].detail==now);
    }
    assert(state.runtime.completed==(immediate?0:1));
    if(!immediate) assert(True(state.streamer.query.flags&QueryFlagBits::IsGuestSynced));
}
int main() {
    for(bool fence:{false,true}) for(bool high:{false,true}) for(bool timestamp:{false,true}) {
        Run<D3D12::QueryCacheParams>(fence,high,timestamp);
        Run<OtherTraits>(fence,high,timestamp);
    }
    Run<D3D12::QueryCacheParams>(true,true,true,true);
}
""".replace("__BODY__", body)
        with tempfile.TemporaryDirectory(prefix="nxbox-query-timestamp-") as folder:
            cpp, binary = Path(folder) / "test.cpp", Path(folder) / "test"
            cpp.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
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
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for opt_out in (None, "0", "true", "1"):
                for trace in ("0", "1"):
                    environment = os.environ.copy()
                    environment.pop("NXBOX_QUERY_TIMESTAMP_AT_PUBLISH", None)
                    if opt_out is not None:
                        environment["NXBOX_QUERY_TIMESTAMP_AT_PUBLISH"] = opt_out
                    environment["NXBOX_TIMING_TRACE"] = trace
                    subprocess.run([str(binary)], env=environment, check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
