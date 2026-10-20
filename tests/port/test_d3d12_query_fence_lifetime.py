# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production query fence publication protocol with std-only mocks."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
using u8=std::uint8_t; using u32=std::uint32_t; using u64=std::uint64_t;
using GPUVAddr=u64; using DAddr=u64; using VAddr=u64;
#define UNIMPLEMENTED() assert(false)
#define LOG_ERROR(...) ((void)0)
namespace Core { constexpr int DEVICE_PAGEBITS=12; constexpr u64 DEVICE_PAGEMASK=4095; }
namespace Settings {
inline bool high=false;
inline bool IsGPUFenceBehaviorDefault() { return true; }
inline bool IsGPULevelHigh() { return high; }
inline bool IsGPUFenceBehaviorBalanced() { return false; }
inline bool IsGPUFenceBehaviorAccurate() { return false; }
}
namespace VideoCommon {
enum class QueryType { Payload, Counter };
enum class QueryPropertiesFlags : u32 { None=0, HasTimeout=1, IsAFence=2 };
enum class QueryFlagBits : u32 { None=0, HasTimestamp=1, IsFinalValueSynced=2,
    IsInvalidated=4, IsFence=8, IsRewritten=16, IsGuestSynced=32 };
template<class T> T operator|(T a,T b) { return T(u32(a)|u32(b)); }
template<class T> T operator&(T a,T b) { return T(u32(a)&u32(b)); }
template<class T> T& operator|=(T& a,T b) { return a=a|b; }
template<class T> bool True(T a) { return u32(a)!=0; }
template<class T> bool False(T a) { return !True(a); }
struct Query { u64 value; QueryFlagBits flags; bool alive=true; u64 guest_address=16; };
struct Field { u32 value{}; void Assign(u32 v) { value=v; } };
struct QueryLocation { Field stream_id,query_id; };
struct StreamerInterface {
    std::deque<Query> queries;
    int freed{};
    size_t WriteCounter(u64,bool timestamp,u32 payload,u32) {
        queries.push_back({payload,QueryFlagBits::IsFinalValueSynced |
            (timestamp ? QueryFlagBits::HasTimestamp : QueryFlagBits::None)});
        return queries.size()-1;
    }
    Query* GetQuery(size_t id) { return &queries[id]; }
    void Free(size_t id) { assert(queries[id].alive); queries[id].alive=false; ++freed; }
    u64 GetAmendValue() { return 0; }
    void SetAccumulationValue(u64) {}
};
struct Rasterizer {
    bool inline_completion=false;
    std::function<void()> on_publish,on_retire;
    std::vector<std::function<void()>> pending;
    void SignalFence(std::function<void()> op) {
        if(on_publish) on_publish();
        if(inline_completion) { op(); if(on_retire) on_retire(); }
        else pending.push_back(std::move(op));
    }
    void SyncOperation(std::function<void()> op) { pending.push_back(std::move(op)); }
};
struct Memory {
    std::array<u8,128> bytes{};
    template<class T> T* GetPointer(u64 address) { return reinterpret_cast<T*>(bytes.data()+address); }
    std::optional<u64> GpuToCpuAddress(u64 address) { return address; }
};
struct GPU { u64 GetTicks() { return 1234; } };
struct Runtime {
    u64 queued{}, written{}, invalidated{};
    u64 ReportPending(VAddr address) { assert(address == 16); return ++queued; }
    void ReportCompleted(bool cancelled, u64 token) {
        assert(token == queued && token != 0);
        if (cancelled) ++invalidated;
        else ++written;
    }
};
template<class Traits> struct Impl {
    Runtime runtime;
    StreamerInterface payload,counter;
    std::array<StreamerInterface*,2> streamers{&payload,&counter};
    Rasterizer rasterizer;
    Memory device_memory;
    GPU gpu;
    std::vector<QueryLocation> pending_unregister;
    Query* ObtainQuery(QueryLocation loc) { return streamers[loc.stream_id.value]->GetQuery(loc.query_id.value); }
};
template<class Traits> struct QueryCacheBase {
    std::unique_ptr<Impl<Traits>> impl=std::make_unique<Impl<Traits>>();
    Memory* gpu_memory=&impl->device_memory;
    std::mutex cache_mutex;
    std::unordered_map<u64,std::unordered_map<u32,QueryLocation>> cached_queries;
    void CounterReport(GPUVAddr,QueryType,QueryPropertiesFlags,u32,u32);
    void Retire() {
        for(auto loc:impl->pending_unregister) {
            assert(cached_queries[0].contains(16));
            impl->streamers[loc.stream_id.value]->Free(loc.query_id.value);
            cached_queries[0].erase(16);
        }
        impl->pending_unregister.clear();
    }
    u32 Value() { u32 v{}; std::memcpy(&v,impl->device_memory.bytes.data()+16,4); return v; }
};
__COUNTER_REPORT__
}
struct Native { static constexpr bool REQUIRE_GPU_COMPLETION=true; };
struct Legacy {};
int main() {
    using namespace VideoCommon;
    QueryCacheBase<Native> native;
    native.CounterReport(16,QueryType::Payload,QueryPropertiesFlags::IsAFence,42,0);
    assert(native.impl->payload.freed==0 && native.cached_queries[0].size()==1);
    assert(native.Value()==0);
    assert(native.impl->runtime.queued == 1 && native.impl->runtime.written == 0);
    native.impl->rasterizer.pending[0]();
    assert(native.Value()==42);
    assert(native.impl->runtime.written == 1);
    assert(True(native.impl->payload.queries[0].flags & QueryFlagBits::IsGuestSynced));
    native.Retire();
    assert(native.impl->payload.freed==1 && native.cached_queries[0].empty());
    // A fence already reached by the GPU can retire inside SignalFence itself.
    QueryCacheBase<Native> completed;
    completed.impl->rasterizer.inline_completion=true;
    completed.impl->rasterizer.on_publish=[&] { assert(completed.cached_queries[0].size()==1); };
    completed.impl->rasterizer.on_retire=[&] { completed.Retire(); };
    completed.CounterReport(16,QueryType::Payload,QueryPropertiesFlags::IsAFence,77,0);
    assert(completed.Value()==77 && completed.impl->payload.freed==1);
    assert(completed.cached_queries[0].empty());
    // Preserve immediate low-accuracy behavior for backends without this requirement.
    QueryCacheBase<Legacy> legacy;
    legacy.impl->rasterizer.inline_completion=true;
    legacy.CounterReport(16,QueryType::Payload,QueryPropertiesFlags::IsAFence,88,0);
    assert(legacy.Value()==88 && legacy.impl->payload.freed==1 && legacy.cached_queries.empty());
    QueryCacheBase<Native> immediate;
    immediate.CounterReport(16,QueryType::Payload,QueryPropertiesFlags::None,99,0);
    assert(immediate.Value()==99 && immediate.impl->payload.freed==1 && immediate.cached_queries.empty());
    assert(immediate.impl->runtime.queued == 0 && immediate.impl->runtime.written == 0);
    QueryCacheBase<Native> invalidated;
    invalidated.CounterReport(16,QueryType::Payload,QueryPropertiesFlags::IsAFence,55,0);
    invalidated.impl->payload.queries[0].flags |= QueryFlagBits::IsInvalidated;
    invalidated.impl->rasterizer.pending[0]();
    assert(invalidated.Value() == 0 && invalidated.impl->runtime.invalidated == 1);
    assert(invalidated.impl->runtime.written == 0);
}
"""


class D3D12QueryFenceLifetimeTests(unittest.TestCase):
    def test_deferred_slots_and_registration_before_publication(self):
        source = (ROOT / "src/video_core/query_cache/query_cache.h").read_text()
        marker = "template <typename Traits>\nvoid QueryCacheBase<Traits>::CounterReport"
        report = marker + source.split(marker, 1)[1].split("template <typename Traits>", 1)[0]
        with tempfile.TemporaryDirectory(prefix="nxbox-query-fence-lifetime-") as folder:
            cpp = Path(folder) / "reports.cpp"
            binary = Path(folder) / "reports"
            cpp.write_text(
                f'#include "{ROOT / "src/core/core_timing_trace.h"}"\n'
                "namespace D3D12 { struct QueryCacheParams; }\n"
                + DRIVER.replace("__COUNTER_REPORT__", report)
            )
            result = subprocess.run(
                ["clang++", "-std=c++20", str(cpp), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)
