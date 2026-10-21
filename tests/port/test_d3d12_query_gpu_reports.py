# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard asynchronous reporting and exercise the production GPU reduction arithmetic."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


def function_body(source, signature):
    start = source.index("{", source.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1 : end - 1]


class GpuQueryReports(unittest.TestCase):
    def test_reports_and_resets_collect_only_in_synchronous_mode(self):
        source = (D3D12 / "d3d12_query_cache.cpp").read_text()
        for signature in ("QueryCacheRuntime::ReadCounter(", "QueryCacheRuntime::ResetCounter("):
            body = function_body(source, signature)
            synchronous = function_body(body, "if (impl->synchronous)")
            self.assertEqual(body.count("impl->Collect();"), 1)
            self.assertIn("impl->Collect();", synchronous)
            self.assertIn("impl->AccumulateGpu();", body)
            self.assertIn("impl->DispatchResolve(", body)
        policy = function_body(source, "bool SynchronousQueries()")
        self.assertNotIn("Settings::IsGPULevelHigh()", policy)
        self.assertIn('BisectEnabled("NXBOX_D3D12_SYNC_QUERIES")', policy)
        docs = (ROOT / "docs/d3d12-backend.md").read_text()
        self.assertRegex(docs, r"\| `NXBOX_D3D12_SYNC_QUERIES` \|.*`1`.*report/reset")
        self.assertIn("GpuResolvedQueryReports", (D3D12 / "d3d12_context.h").read_text())
        self.assertIn('"gpu_resolved_query_reports"', (D3D12 / "d3d12_context.cpp").read_text())

    def test_gpu_rotation_and_snapshot_retirement_do_not_wait(self):
        source = (D3D12 / "d3d12_query_cache.cpp").read_text()
        rotate = function_body(source, "void Rotate()")
        gpu = function_body(rotate, "if (!synchronous)")
        self.assertIn("AccumulateGpu();", gpu)
        self.assertIn("accumulated = 0;", gpu)
        self.assertNotRegex(gpu, r"\b(?:Wait|Finish|Consume)\(")
        pop = function_body(source, "void PopUnsyncedQueries()")
        self.assertIn("snapshot->second.mapped_span.data()", pop)
        self.assertIn("std::scoped_lock slots{guard};", pop)
        self.assertIn("IsFinalValueSynced", pop)
        self.assertNotRegex(pop, r"\b(?:Wait|Finish|ReadCounter)\(")
        read = function_body(source, "QueryCacheRuntime::ReadCounter(")
        self.assertIn('BisectEnabled("NXBOX_D3D12_NO_QUERIES")', read)
        self.assertIn("CopyFilledSize(stream, impl->gpu_report)", read)
        copy = function_body(source, "void QueryCacheRuntime::CopyValue(")
        self.assertIn("ObtainBufferOperation::MarkAsWritten", copy)

    def test_reports_keep_snapshots_and_batch_guest_writes(self):
        source = (D3D12 / "d3d12_query_cache.cpp").read_text()
        write = function_body(source, "size_t WriteCounter(")
        read = function_body(source, "QueryCacheRuntime::ReadCounter(")
        self.assertNotIn("WriteCounterReport", source)
        self.assertNotIn("SyncValues", write)
        self.assertNotIn("CopyValue", read)
        batch = function_body(read, "if (impl->batch_queries")
        self.assertIn("ObtainCPUBuffer", batch)
        self.assertIn("ObtainBufferOperation::MarkAsWritten", batch)
        self.assertIn("batch_destinations.push_back", batch)
        self.assertIn("result.mapped_span.subspan(offset, 8)", batch)
        self.assertIn("CopyBufferRegion(result.buffer->handle.Get()", read)
        self.assertIn("FrameCount::GpuResolvedQueryReports", read)
        compare = function_body(source, "bool QueryCacheRuntime::CompareConditional(")
        self.assertIn("host_managed(first) || host_managed(second)", compare)
        fallback = function_body(compare, "if (host_managed(first)")
        self.assertIn("EndHostConditionalRendering();", fallback)
        self.assertIn("return !Settings::IsGPULevelHigh();", fallback)

    def test_guest_delivery_and_one_wait_for_an_unfinished_batch(self):
        source = (ROOT / "src/video_core/query_cache/query_cache.h").read_text()
        header = (ROOT / "src/video_core/query_cache/query_cache_base.h").read_text()
        methods = "\n".join(
            "template<class Traits> "
            + result
            + " QueryCacheBase<Traits>::"
            + signature
            + " {"
            + function_body(source, "QueryCacheBase<Traits>::" + signature)
            + "}"
            for result, signature in (
                ("bool", "IsQueryDirty(QueryCacheBase<Traits>::QueryLocation location)"),
                ("bool", "SemiFlushQueryDirty(QueryCacheBase<Traits>::QueryLocation location)"),
                ("void", "RequestGuestHostSync()"),
            )
        )
        flush = function_body(header, "void FlushRegion(")
        driver = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
using u8=unsigned char; using u32=std::uint32_t; using u64=std::uint64_t;
using VAddr=u64;
enum class QueryFlagBits : u32 {
    IsFinalValueSynced=1, IsGuestSynced=2, IsHostManaged=4, HasTimestamp=8
};
QueryFlagBits operator&(QueryFlagBits a,QueryFlagBits b) { return QueryFlagBits(u32(a)&u32(b)); }
QueryFlagBits operator|(QueryFlagBits a,QueryFlagBits b) { return QueryFlagBits(u32(a)|u32(b)); }
QueryFlagBits& operator|=(QueryFlagBits& a,QueryFlagBits b) { return a=a|b; }
bool True(QueryFlagBits v) { return u32(v)!=0; }
bool False(QueryFlagBits v) { return !True(v); }
struct Query { u64 guest_address,value; QueryFlagBits flags; };
struct Memory {
    std::array<u8,64> bytes{};
    template<class T> T* GetPointer(u64 addr) { return reinterpret_cast<T*>(bytes.data()+addr); }
};
struct Runtime { int waits{}; void ReportQueryFenceWait() { ++waits; } };
struct Rasterizer { std::function<void()> complete; void ReleaseFences() { complete(); } };
struct Impl {
    std::array<Query,2> queries{{{0,0x123456789ULL,QueryFlagBits::IsHostManaged},
                              {16,0xabcdef1234567890ULL,QueryFlagBits::IsHostManaged|QueryFlagBits::HasTimestamp}}};
    Memory device_memory;
    Runtime runtime;
    Rasterizer rasterizer;
    Query* ObtainQuery(int index) { return index<2 ? &queries[index] : nullptr; }
};
template<class Traits> struct QueryCacheBase {
    using QueryLocation=int;
    Impl storage; Impl* impl=&storage;
    bool IsQueryDirty(QueryLocation);
    bool SemiFlushQueryDirty(QueryLocation);
    void RequestGuestHostSync();
    template<bool, class F> void IterateCache(VAddr address,std::size_t,F f) { f(int(address/16)); }
    void FlushRegion(VAddr addr,std::size_t size) { __FLUSH__ }
};
__METHODS__
int main() {
    QueryCacheBase<int> cache;
    auto& state=cache.storage;
    state.device_memory.bytes.fill(0x55);
    state.rasterizer.complete=[&] {
        // The fence manager publishes every snapshot and executes guest callbacks
        // for the whole batch before returning from ReleaseFences.
        for(int i=0;i<2;++i) {
            state.queries[i].flags|=QueryFlagBits::IsFinalValueSynced;
            cache.SemiFlushQueryDirty(i);
        }
    };
    assert(cache.IsQueryDirty(0) && cache.IsQueryDirty(1));
    cache.FlushRegion(0,4);
    cache.FlushRegion(16,8);
    assert(state.runtime.waits==1);
    assert(!cache.IsQueryDirty(0) && !cache.IsQueryDirty(1));
    u32 low{}; u64 wide{};
    std::memcpy(&low,state.device_memory.bytes.data(),4);
    std::memcpy(&wide,state.device_memory.bytes.data()+16,8);
    assert(low==0x23456789U && wide==0xabcdef1234567890ULL);
    // Four-byte reports preserve the following bytes; timestamp delivery remains
    // the completion callback's responsibility, not SemiFlushQueryDirty's.
    assert(state.device_memory.bytes[4]==0x55 && state.device_memory.bytes[24]==0x55);
    state.queries[0].flags=QueryFlagBits::IsHostManaged|QueryFlagBits::IsFinalValueSynced;
    cache.FlushRegion(0,4);
    assert(state.runtime.waits==1 && !cache.IsQueryDirty(0));
    assert(!cache.SemiFlushQueryDirty(9));
}
""".replace("__FLUSH__", flush).replace("__METHODS__", methods)
        with tempfile.TemporaryDirectory(prefix="nxbox-query-delivery-") as folder:
            cpp, binary = Path(folder) / "delivery.cpp", Path(folder) / "delivery"
            cpp.write_text(driver)
            result = subprocess.run(
                ["clang++", "-std=c++20", str(cpp), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)
        runtime = (D3D12 / "d3d12_query_cache.cpp").read_text()
        wait = function_body(runtime, "void QueryCacheRuntime::ReportQueryFenceWait()")
        self.assertIn("FrameCount::QueryReportFenceWaits", wait)
        self.assertIn('"query_report_fence_waits"', (D3D12 / "d3d12_context.cpp").read_text())

    def test_production_shader_reduction_and_reset_semantics(self):
        # Execute the HLSL bodies with byte-buffer mocks. This tests the actual
        # shader's mappings/branches and uint2 arithmetic without a Windows SDK.
        shader = (
            (D3D12 / "d3d12_query_resolve.h")
            .read_text()
            .split('R"hlsl(', 1)[1]
            .split(')hlsl"', 1)[0]
        )
        shader = re.sub(
            r"cbuffer Parameters[^}]+};",
            "uint operation, first_slot, slot_count, report_type, report_stream;",
            shader,
        )
        shader = re.sub(r"(?:RW)?ByteAddressBuffer (\w+) : register\([^;]+;", r"Buffer \1;", shader)
        shader = shader.replace("[numthreads(32, 1, 1)]", "").replace(
            "void main(uint3 id : SV_DispatchThreadID)", "void Resolve(uint3 id)"
        )
        enums = (ROOT / "src/video_core/query_cache/types.h").read_text()
        defines = "\n".join(
            f"#define {name} {value}"
            for name, value in re.findall(r"^    (\w+) = (\d+),", enums, re.M)
        )
        driver = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
using uint = std::uint32_t;
struct uint2 { uint x,y; uint2(uint x,uint y):x(x),y(y){} };
struct uint3 { uint x,y,z; };
struct uint4 { uint x,y,z,w; };
struct Buffer {
    std::vector<uint> data = std::vector<uint>(4096);
    uint2 Load2(uint offset) { return {data.at(offset/4),data.at(offset/4+1)}; }
    uint4 Load4(uint offset) { return {data.at(offset/4),data.at(offset/4+1),data.at(offset/4+2),data.at(offset/4+3)}; }
    void Store2(uint offset,uint2 value) { data.at(offset/4)=value.x; data.at(offset/4+1)=value.y; }
};
#define TYPE_COUNT 32
#define RESULT_STRIDE 160
#define SO_OFFSET 96
__DEFINES__
__SHADER__
std::uint64_t Value(Buffer& b,uint offset) {
    auto v=b.Load2(offset); return (std::uint64_t(v.y)<<32)|v.x;
}
void Run(uint op,uint type=0,uint stream=0) {
    operation=op; report_type=type; report_stream=stream;
    for(uint i=0;i<128;++i) Resolve({i,0,0});
}
int main() {
    first_slot=0; slot_count=2;
    // A captured stream and a geometry-only draw. Poison inactive query data.
    metadata.data[0]=1; metadata.data[1]=0; metadata.data[2]=1; metadata.data[3]=3;
    metadata.data[4]=0; metadata.data[5]=1; metadata.data[6]=1; metadata.data[7]=0;
    results.Store2(0,{0xffffffff,1});
    results.Store2(160,{12345,6});
    for(uint i=0;i<10;++i) {
        results.Store2(8+i*8,{10+i,0});
        results.Store2(160+8+i*8,{20+i,0});
    }
    results.Store2(96,{3,0}); results.Store2(104,{9,0});
    results.Store2(112,{8,0}); results.Store2(120,{2,0});
    Run(0);
    auto total=[](uint type,uint stream=0) { return Value(counters,(type*4+stream)*8); };
    assert(total(ZPassPixelCount)==0x1ffffffffULL);
    assert(total(ZPassPixelCount64)==total(ZPassPixelCount));
    assert(total(VerticesGenerated)==30);
    assert(total(GeometryShaderPrimitivesGenerated)==14+24);
    assert(total(ClipperInvocations)==15+25);
    assert(total(PrimitivesGenerated)==9+24);
    assert(total(PrimitivesGenerated,1)==0);
    assert(total(VtgPrimitivesOut,1)==2);
    assert(total(StreamingPrimitivesSucceeded)==3);
    assert(total(StreamingPrimitivesNeeded)==9);
    assert(total(StreamingPrimitivesNeededMinusSucceeded)==6);
    assert(total(StreamingPrimitivesNeededMinusSucceeded,1)==0);
    assert(total(TotalStreamingPrimitivesNeededMinusSucceeded)==6);
    assert(total(TotalStreamingPrimitivesNeededMinusSucceeded,1)==0);
    Run(1,ZPassPixelCount); assert(Value(report,0)==0x1ffffffffULL);
    Run(2,ZPassPixelCount); Run(1,ZPassPixelCount); assert(Value(report,0)==0);
    // Maxwell Conditional requires BOTH 32-bit words, not a nonzero uint64.
    // Execute the production predicate shader with poisoned output and all lanes.
    const auto saved_results = results.data;
    const auto saved_counters = counters.data;
    const auto saved_metadata = metadata.data;
    for (uint low : {0u, 1u, 0x80000000u, 0xffffffffu}) {
        for (uint high : {0u, 1u, 0x80000000u, 0xffffffffu}) {
            results.Store2(0, {low, high});
            report.Store2(0, {0xdeadbeef, 0xdeadbeef});
            Run(3);
            assert(Value(report, 0) == uint(low != 0 && high != 0));
            assert(counters.data == saved_counters);
            assert(metadata.data == saved_metadata);
        }
    }
    results.data = saved_results;
    // Reset affects only its counter; uint2 addition/subtraction carries/borrows.
    first_slot=0; slot_count=1; results.Store2(0,{2,0}); Run(0);
    Run(1,ZPassPixelCount); assert(Value(report,0)==2);
    Run(1,ZPassPixelCount64); assert(Value(report,0)==0x200000001ULL);
    Run(1,StreamingPrimitivesNeededMinusSucceeded); assert(Value(report,0)==12);
    Run(2,StreamingPrimitivesNeededMinusSucceeded);
    Run(1,StreamingPrimitivesNeededMinusSucceeded); assert(Value(report,0)==0);
    // Inactive pipeline/SO bytes must never enter totals.
    metadata.data[2]=0; metadata.data[3]=0; Run(0);
    assert(total(VerticesGenerated)==40);
    assert(total(GeometryShaderPrimitivesGenerated)==52);
    assert(Value(report,0)==0);
}
""".replace("__DEFINES__", defines).replace("__SHADER__", shader)
        with tempfile.TemporaryDirectory(prefix="nxbox-query-gpu-") as folder:
            source, binary = Path(folder) / "resolve.cpp", Path(folder) / "resolve"
            source.write_text(driver)
            result = subprocess.run(
                ["clang++", "-std=c++20", str(source), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)
