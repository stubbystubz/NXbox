# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the production facade/transport against a fake D3D12 list, without Windows."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
METHODS = json.loads((Path(__file__).with_name("d3d12_record_methods.json")).read_text())


def source():
    preamble = r"""
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_record_args.h"
#include "video_core/renderer_d3d12/d3d12_recording_progress.h"
thread_local bool track_allocations = false;
thread_local bool fail_allocation = false;
static std::atomic<unsigned> producer_allocations{};
void* operator new(size_t bytes) {
    if (std::exchange(fail_allocation, false)) throw std::bad_alloc();
    if (track_allocations) ++producer_allocations;
    if (auto* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
using UINT = unsigned; using UINT8 = unsigned char; using UINT64 = uint64_t;
using INT = int; using FLOAT = float; using BOOL = int;
using D3D12_GPU_VIRTUAL_ADDRESS = uint64_t;
using D3D12_QUERY_TYPE = int; using D3D12_CLEAR_FLAGS = int;
using D3D_PRIMITIVE_TOPOLOGY = int; using DXGI_FORMAT = int;
using D3D12_PREDICATION_OP = int;
struct Object {
    std::atomic<int> refs{1}; bool* destroyed = nullptr;
    virtual ~Object() { if (destroyed) *destroyed = true; }
    void AddRef() { ++refs; }
    void Release() { if (--refs == 0) delete this; }
};
struct D3D12_RESOURCE_DESC {
    unsigned Dimension{}; uint64_t Width{};
    unsigned Height{}, DepthOrArraySize{}, MipLevels{}; DXGI_FORMAT Format{};
};
struct ID3D12Resource : Object { D3D12_RESOURCE_DESC GetDesc() { return {}; } };
struct ID3D12QueryHeap : Object {
    struct Description { unsigned Count; };
    Description GetDesc() { return {}; }
};
struct ID3D12RootSignature : Object {};
struct ID3D12PipelineState : Object {};
struct ID3D12CommandSignature : Object {};
struct D3D12_CPU_DESCRIPTOR_HANDLE { size_t ptr; };
struct D3D12_GPU_DESCRIPTOR_HANDLE { uint64_t ptr; };
struct D3D12_RECT { int left, top, right, bottom; };
struct D3D12_BOX { UINT left, top, front, right, bottom, back; };
struct D3D12_VIEWPORT { float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth; };
struct D3D12_VERTEX_BUFFER_VIEW { uint64_t BufferLocation; UINT SizeInBytes, StrideInBytes; };
struct D3D12_INDEX_BUFFER_VIEW { uint64_t BufferLocation; UINT SizeInBytes; DXGI_FORMAT Format; };
struct D3D12_STREAM_OUTPUT_BUFFER_VIEW { uint64_t BufferLocation, SizeInBytes, BufferFilledSizeLocation; };
constexpr unsigned D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER = 1;
constexpr unsigned D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE = 1;
constexpr unsigned D3D12_RESOURCE_DIMENSION_BUFFER = 1;
constexpr unsigned D3D12_RESOURCE_DIMENSION_TEXTURE3D = 4;
constexpr unsigned D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT = 1;
constexpr unsigned D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX = 0;
constexpr int D3D12_QUERY_TYPE_PIPELINE_STATISTICS = 2;
constexpr int D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 = 4;
constexpr int D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3 = 7;
struct D3D12_QUERY_DATA_PIPELINE_STATISTICS { uint64_t counters[11]; };
struct D3D12_QUERY_DATA_SO_STATISTICS { uint64_t counters[2]; };
struct D3D12_TEXTURE_COPY_LOCATION {
    ID3D12Resource* pResource; int Type;
    union {
        uint64_t detail[5];
        unsigned SubresourceIndex;
        struct {
            uint64_t Offset;
            struct { DXGI_FORMAT Format; unsigned Width, Height, Depth, RowPitch; } Footprint;
        } PlacedFootprint;
    };
};
constexpr int D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0, D3D12_RESOURCE_BARRIER_TYPE_ALIASING = 1,
              D3D12_RESOURCE_BARRIER_TYPE_UAV = 2;
struct D3D12_RESOURCE_BARRIER {
    int Type, Flags;
    union {
        struct { ID3D12Resource* pResource; UINT Subresource; int StateBefore, StateAfter; } Transition;
        struct { ID3D12Resource* pResourceBefore; ID3D12Resource* pResourceAfter; } Aliasing;
        struct { ID3D12Resource* pResource; } UAV;
    };
};
static std::map<size_t, size_t> descriptors;
constexpr int D3D12_DESCRIPTOR_HEAP_TYPE_RTV = 1, D3D12_DESCRIPTOR_HEAP_TYPE_DSV = 2;
struct D3D12_DESCRIPTOR_HEAP_DESC { int Type; UINT NumDescriptors; UINT Flags{}; };
struct ID3D12DescriptorHeap : Object {
    size_t base = 0;
    D3D12_DESCRIPTOR_HEAP_DESC GetDesc() { return {}; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStart() { return {base}; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart() { return {base}; }
};
template <class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
};
#define IID_PPV_ARGS(p) p
inline void Check(int result, const char*, void* = nullptr) { if (result) throw std::runtime_error("fake interface failure"); }
constexpr int D3D12_FEATURE_FORMAT_INFO = 1;
constexpr DXGI_FORMAT DXGI_FORMAT_BC1_TYPELESS = 70, DXGI_FORMAT_BC5_SNORM = 84;
constexpr DXGI_FORMAT DXGI_FORMAT_BC6H_TYPELESS = 94, DXGI_FORMAT_BC7_UNORM_SRGB = 99;
struct D3D12_FEATURE_DATA_FORMAT_INFO { DXGI_FORMAT Format; unsigned PlaneCount; };
struct ID3D12Device {
    int CheckFeatureSupport(int, void* data, size_t) {
        static_cast<D3D12_FEATURE_DATA_FORMAT_INFO*>(data)->PlaneCount = 1; return 0;
    }
    size_t next = 100000;
    int CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_DESC* d, ID3D12DescriptorHeap** result) {
        *result = new ID3D12DescriptorHeap; (*result)->base = next;
        for (size_t i=0; i<d->NumDescriptors; ++i) descriptors[next+i] = 0;
        next += d->NumDescriptors + 1; return 0;
    }
    UINT GetDescriptorHandleIncrementSize(int) { return 1; }
    void CopyDescriptorsSimple(UINT, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, int) {
        descriptors[dst.ptr] = descriptors.at(src.ptr);
    }
};
using Log = std::vector<std::string>;
template <class T> void append(std::string& out, const T& value) { out.append(reinterpret_cast<const char*>(&value), sizeof(value)); }
// Compare descriptor content, not the facade's private snapshot slot addresses.
inline void append(std::string& out, const D3D12_CPU_DESCRIPTOR_HANDLE& value) { append(out, descriptors.at(value.ptr)); }
template <class T> void array(std::string& out, const T* values, size_t count) {
    append(out, values != nullptr); append(out, count);
    if (values) for (size_t i = 0; i < count; ++i) append(out, values[i]);
}
struct ID3D12GraphicsCommandList : Object {
    Log calls;
    template <class T> int QueryInterface(T** value) { *value = static_cast<T*>(this); AddRef(); return 0; }
"""
    for name, args in METHODS:
        preamble += "void " + name + "(" + ", ".join(t + " " + n for t, n, c in args) + ") {\n"
        preamble += f'std::string out("{name}");\n'
        for t, n, c in args:
            if c is None:
                preamble += f"append(out, {n});\n"
            else:
                if name == "OMSetRenderTargets" and n == "handles":
                    preamble += "append(out, handles != nullptr); append(out, count);\n"
                    preamble += "if (handles) for (UINT i=0; i<count; ++i) append(out, D3D12_CPU_DESCRIPTOR_HANDLE{handles[contiguous ? 0 : i].ptr + (contiguous ? i : 0)});\n"
                    continue
                expr = n
                if c.startswith("DWORDS:"):
                    expr = f"static_cast<const UINT*>({n})"
                    c = c[7:]
                elif c.startswith("BYTES:"):
                    expr = f"static_cast<const UINT8*>({n})"
                    c = c[6:]
                preamble += f"array(out, {expr}, {c});\n"
        preamble += "calls.push_back(out); }\n"
    preamble += r"""
};
using ID3D12GraphicsCommandList1 = ID3D12GraphicsCommandList;
using ID3D12GraphicsCommandList8 = ID3D12GraphicsCommandList;
// Exercise root-CBV tracking while comparing every facade call to the raw list.
namespace D3D12 {
inline bool BisectEnabled(const char* name) {
    return std::strcmp(name, "NXBOX_D3D12_VALIDATE_COMMANDS") != 0;
}
inline void Diagnostic(const std::string&) {}
}
#include "video_core/renderer_d3d12/d3d12_commands.h"
// Transport tests run with profiling off. Any sink invocation is an off-path regression.
namespace D3D12 {
void GpuPassProfile::Work(GpuPassKind) { std::abort(); }
void GpuPassProfile::Boundary() { std::abort(); }
void GpuPassProfile::Target(GpuPassTarget) { std::abort(); }
}
using namespace D3D12;
int main() {
    ID3D12Resource resource;
    ID3D12QueryHeap query;
    ID3D12RootSignature root;
    ID3D12PipelineState pipeline;
    ID3D12CommandSignature signature;
    ID3D12DescriptorHeap heap;
    ID3D12Device device;
    ID3D12GraphicsCommandList expected, actual, immediate;
    CommandFacade direct; direct.immediate = &immediate;
    RecordStream<> stream;
    RecordDescriptors snapshots; snapshots.Initialize(&device);
    CommandFacade facade;
    facade.stream = &stream; facade.descriptors = &snapshots;
    RecordingTarget target; target.list = &actual; target.list1 = &actual; target.list8 = &actual;
    UINT words[4]{0x12345678, 0xabcdef01, 17, 0xffffffff};
    UINT8 bytes[4]{0, 255, 47, 2};
    FLOAT floats[4]{0.25f, -2.0f, 1.0f, 0.9f};
    D3D12_RECT rects[2]{{-1, 2, 93, 47}, {1, 2, 3, 4}};
    D3D12_VIEWPORT views[2]{{-2, 3, 47, 29, 0.2f, 0.9f}, {1, 2, 3, 4, 0, 1}};
    D3D12_VERTEX_BUFFER_VIEW vertices[2]{{0x123456789abcdef0, 12, 4}, {0xabcdef9876543210, 77, 19}};
    D3D12_INDEX_BUFFER_VIEW index{0xf123456789abcdef, 993, 5};
    D3D12_STREAM_OUTPUT_BUFFER_VIEW outputs[2]{{0x123456789ab, 777, 0xaabbccddeeff}, {3, 4, 5}};
    D3D12_TEXTURE_COPY_LOCATION location{}; location.pResource = &resource;
    location.Type = 1; location.detail[0] = 0xf123456789abcdef; location.detail[4] = 37;
    D3D12_BOX box{1, 2, 3, 49, 67, 89};
    D3D12_CPU_DESCRIPTOR_HANDLE handles[2]{{10}, {11}}, depth{12};
    descriptors[10] = 900; descriptors[11] = 901; descriptors[12] = 902;
    D3D12_RESOURCE_BARRIER barriers[3]{};
    barriers[0].Type = 0; barriers[0].Transition = {&resource, 0xffffffff, 4, 8};
    barriers[1].Type = 1; barriers[1].Aliasing = {nullptr, &resource};
    barriers[2].Type = 2; barriers[2].UAV = {nullptr};
    ID3D12DescriptorHeap* heaps[2]{&heap, &heap};
"""
    scalar_objects = {
        "ID3D12Resource*": "&resource",
        "ID3D12QueryHeap*": "&query",
        "ID3D12RootSignature*": "&root",
        "ID3D12PipelineState*": "&pipeline",
        "ID3D12CommandSignature*": "&signature",
    }
    arrays = {
        "const D3D12_RECT*": "rects",
        "const D3D12_VIEWPORT*": "views",
        "const D3D12_VERTEX_BUFFER_VIEW*": "vertices",
        "const D3D12_INDEX_BUFFER_VIEW*": "&index",
        "const FLOAT*": "floats",
        "const D3D12_STREAM_OUTPUT_BUFFER_VIEW*": "outputs",
        "const D3D12_TEXTURE_COPY_LOCATION*": "&location",
        "const D3D12_BOX*": "&box",
        "const D3D12_RESOURCE_BARRIER*": "barriers",
        "ID3D12DescriptorHeap* const*": "heaps",
    }
    for name, args in METHODS:
        expressions = []
        for i, (t, n, c) in enumerate(args):
            if t in scalar_objects:
                expr = scalar_objects[t]
            elif t in arrays:
                expr = arrays[t]
            elif t == "const void*":
                expr = "words" if c.startswith("DWORDS") else "bytes"
            elif t == "const D3D12_CPU_DESCRIPTOR_HANDLE*":
                expr = "&depth" if n == "depth" else "handles"
            elif t == "D3D12_CPU_DESCRIPTOR_HANDLE":
                expr = "depth" if name == "ClearDepthStencilView" else "handles[0]"
            elif t == "D3D12_GPU_DESCRIPTOR_HANDLE":
                expr = "D3D12_GPU_DESCRIPTOR_HANDLE{0xfedcba9876543210}"
            elif t == "UINT64" or t == "D3D12_GPU_VIRTUAL_ADDRESS":
                expr = "UINT64{0xfedcba9876543210}"
            elif t == "FLOAT":
                expr = f"{i + 1}.25f"
            elif t == "INT":
                expr = "-29"
            elif n == "count":
                expr = "3" if name == "ResourceBarrier" else "2"
            elif n == "size":
                expr = "4"
            elif n == "contiguous":
                expr = "0"
            else:
                expr = str(i + 3)
            expressions.append(expr)

        def emit(values):
            nonlocal preamble
            params = ", ".join(values)
            preamble += f"expected.{name}({params}); facade.{name}({params});\n"
            preamble += f"direct.{name}({params});\n"

        emit(expressions)
        # Null optional pointers, empty arrays, contiguous RTV handles, both copy unions.
        if any(c is not None for _, _, c in args):
            nulls = expressions.copy()
            for i, (_, n, c) in enumerate(args):
                if c is not None:
                    nulls[i] = "nullptr"
                elif n in ("count", "size"):
                    nulls[i] = "0"
            emit(nulls)
        if name == "OMSetRenderTargets":
            contiguous = expressions.copy()
            contiguous[2] = "1"
            emit(contiguous)
        if name == "CopyTextureRegion":
            preamble += "location.Type = 0; location.detail[0] = 179;\n"
            emit(expressions)
        if name == "SetPredication":
            nulls = expressions.copy()
            nulls[0] = "nullptr"
            emit(nulls)
    preamble += r"""
    // Destroy/overwrite caller-owned arrays and original descriptor contents before replay.
    memset(words, 0, sizeof(words)); memset(bytes, 0, sizeof(bytes)); memset(floats, 0, sizeof(floats));
    memset(rects, 0, sizeof(rects)); memset(views, 0, sizeof(views)); memset(vertices, 0, sizeof(vertices));
    memset(&index, 0, sizeof(index)); memset(outputs, 0, sizeof(outputs)); memset(&location, 0, sizeof(location));
    memset(&box, 0, sizeof(box)); memset(barriers, 0, sizeof(barriers)); memset(heaps, 0, sizeof(heaps));
    descriptors[10] = 999; descriptors[11] = 998; descriptors[12] = 997;
    assert(resource.refs > 1 && heap.refs > 1 && root.refs > 1 && pipeline.refs > 1);
    stream.Close();
    while (stream.Consume(&target)) {}
    assert(actual.calls == expected.calls);
    assert(immediate.calls == expected.calls);
    assert(resource.refs == 1 && heap.refs == 1 && root.refs == 1 && pipeline.refs == 1 && query.refs == 1 && signature.refs == 1);
    std::cout << "all facade kinds: exact sequence, scalars, arrays, nulls, unions, descriptor snapshots, COM holds OK\n";
    RecordStream<> descriptor_stream;
    ID3D12GraphicsCommandList descriptor_expected, descriptor_actual;
    CommandFacade descriptor_facade;
    descriptor_facade.stream = &descriptor_stream; descriptor_facade.descriptors = &snapshots;
    RecordingTarget descriptor_target; descriptor_target.list = &descriptor_actual;
    std::thread descriptor_worker([&] { while (descriptor_stream.Consume(&descriptor_target)) {} });
    for (unsigned i = 0; i < RecordDescriptors::Capacity * 3; ++i) {
        FLOAT color[4]{float(i), float(i+1), 0.5f, 1.0f};
        descriptor_expected.ClearRenderTargetView(handles[0], color, 0, nullptr);
        descriptor_facade.ClearRenderTargetView(handles[0], color, 0, nullptr);
    }
    descriptor_stream.Close(); descriptor_worker.join();
    assert(descriptor_actual.calls == descriptor_expected.calls);
    std::cout << "12288 CPU descriptor snapshots: concurrent replay and slot recycling OK\n";
    // Tiny chunks force records and payloads across boundaries and recycle the pool many times.
    RecordStream<31, 3> tiny;
    tiny.metrics = true;
    struct State { uint64_t next = 0; std::atomic<uint64_t> acknowledged{0}; } state;
    constexpr uint64_t count = 30000;
    auto run = [](void* target, uint64_t value, const UINT* data) {
        auto& s = *static_cast<State*>(target);
        assert(value == s.next++ && data[0] == UINT(value) && data[299] == UINT(value + 299));
    };
    std::thread worker([&] { while (tiny.Consume(&state)) {} });
    for (uint64_t i = 0; i < count; ++i) {
        UINT data[300]; for (UINT j=0; j<300; ++j) data[j] = UINT(i+j);
        EncodeCall<run>(tiny, i, Snapshot(data, 300));
        // A submit must publish the partial tail, even if the producer does nothing afterwards.
        if (i % 101 == 0) {
            EncodeCall<[](void* target, uint64_t value) {
                auto& s = *static_cast<State*>(target);
                s.acknowledged.store(value, std::memory_order_release); s.acknowledged.notify_all();
            }>(tiny, i+1);
            tiny.Flush();
            auto done = state.acknowledged.load(std::memory_order_acquire);
            while (done < i+1) { state.acknowledged.wait(done); done = state.acknowledged.load(); }
        }
    }
    tiny.Close(); worker.join();
    assert(state.next == count && tiny.waits > 0 && tiny.wait_ns > 0);
    std::cout << "SPSC: 30000 records, partial flush acknowledgement, oversized payloads, recycling and backpressure OK\n";
    RecordStream<> allocation_stream;
    track_allocations = true;
    for (UINT i=0; i<1000; ++i) {
        UINT values[4]{i, i+1, i+2, i+3};
        EncodeCall<[](void*, UINT, const UINT*) {}>(allocation_stream, i, Snapshot(values, 4));
    }
    track_allocations = false;
    assert(producer_allocations == 0);
    allocation_stream.Close(); while (allocation_stream.Consume(nullptr)) {}
    std::cout << "producer encoding: zero allocations for 1000 calls OK\n";
    RecordStream<31, 3> failed_pool;
    std::thread failed_worker([&] {
        bool execute = true;
        for (;;) {
            try { if (!failed_pool.Consume(nullptr, execute)) break; }
            catch (const std::runtime_error&) { execute = false; }
        }
    });
    EncodeCall<[](void*) { throw std::runtime_error("injected recorder failure"); }>(failed_pool);
    for (unsigned i=0; i<10000; ++i)
        EncodeCall<[](void*, ID3D12Resource*) { assert(false); }>(failed_pool, &resource);
    failed_pool.Close(); failed_worker.join(); assert(resource.refs == 1);
    std::cout << "failed recorder drains exhausted pool without stranding producer or COM holds OK\n";
    // A scratch allocation failure occurs after the header has been consumed. The
    // failure drain must resume that record, not interpret its payload as a header.
    // Force scratch: the direct path intentionally does not allocate.
    RecordStream<> scratch_failure{RecordStream<>::chunk_count, false};
    unsigned scratch_replays = 0;
    EncodeCall<[](void* target, uint64_t, uint64_t, ID3D12Resource*) {
        ++*static_cast<unsigned*>(target);
    }>(scratch_failure, uint64_t{0}, uint64_t{0}, &resource);
    EncodeCall<[](void* target, uint64_t, uint64_t, ID3D12Resource*) {
        ++*static_cast<unsigned*>(target);
    }>(scratch_failure, uint64_t{0}, uint64_t{0}, &resource);
    scratch_failure.Close();
    fail_allocation = true;
    try { scratch_failure.Consume(&scratch_replays); assert(false); }
    catch (const std::bad_alloc&) {}
    assert(resource.refs == 3 && scratch_replays == 0);
    assert(scratch_failure.Consume(&scratch_replays, false));
    assert(resource.refs == 2);
    assert(scratch_failure.Consume(&scratch_replays, false));
    assert(resource.refs == 1 && scratch_replays == 0);
    assert(!scratch_failure.Consume(&scratch_replays, false));
    std::cout << "scratch allocation failure preserves record framing and releases holds OK\n";
    RecordStream<31, 3> scratch_pool;
    std::thread scratch_worker([&] {
        fail_allocation = true;
        bool execute = true;
        for (;;) {
            try { if (!scratch_pool.Consume(nullptr, execute)) break; }
            catch (const std::bad_alloc&) { execute = false; }
        }
        assert(!execute);
    });
    for (unsigned i=0; i<10000; ++i)
        EncodeCall<[](void*, ID3D12Resource*, const UINT*) { assert(false); }>(
            scratch_pool, &resource, Snapshot(words, 4));
    scratch_pool.Close(); scratch_worker.join(); assert(resource.refs == 1);
    std::cout << "scratch allocation failure drains chunk backpressure and shutdown OK\n";
    // Failed/abandoned replay still drops references and wakes descriptor-pool waiters.
    RecordStream<> failed;
    EncodeCall<[](void*, ID3D12Resource*) { throw std::runtime_error("injected"); }>(failed, &resource);
    EncodeCall<[](void*, ID3D12Resource*) { assert(false); }>(failed, &resource);
    failed.Close();
    try { failed.Consume(nullptr); assert(false); } catch (const std::runtime_error&) {}
    assert(resource.refs == 2);
    assert(failed.Consume(nullptr, false)); assert(resource.refs == 1);
    std::cout << "failure cleanup and abandoned replay OK\n";
    bool destroyed = false;
    RecordStream<> lifetime;
    auto* temporary = new ID3D12Resource;
    temporary->destroyed = &destroyed;
    EncodeCall<[](void*, ID3D12Resource* object, const UINT* values) {
        assert(object->refs == 1 && values[0] == 42 && values[1] == 77);
    }>(lifetime, temporary, Snapshot(std::array<UINT,2>{42,77}.data(), 2));
    temporary->Release(); assert(!destroyed);
    lifetime.Close(); lifetime.Consume(nullptr); assert(destroyed);
    // Use the production acknowledgement primitive: a flush makes the final submit
    // runnable without any later producer call. Acknowledgement is not GPU completion.
    std::atomic<uint64_t> ack{0}, gpu_completed{0}; std::atomic<bool> error{false};
    RecordingProgress progress{ack, error};
    RecordStream<37, 3> submissions;
    struct SubmissionTarget {
        RecordingProgress* progress; std::atomic<uint64_t>* completed;
        uint64_t slots[16]{}; uint64_t next = 1;
    } submission{&progress, &gpu_completed};
    std::thread submitter([&] {
        progress.Ready();
        while (submissions.Consume(&submission)) {}
    });
    progress.WaitReady();
    for (uint64_t tick = 1; tick <= 96; ++tick) {
        EncodeCall<[](void* target, uint64_t tick) {
            auto& s = *static_cast<SubmissionTarget*>(target);
            assert(tick == s.next++);
            assert(s.slots[(tick-1)%16] <= s.completed->load());
            s.slots[(tick-1)%16] = tick;
            s.progress->Acknowledge(tick);
        }>(submissions, tick);
        submissions.Flush();
        progress.Await(tick);
        assert(ack == tick && gpu_completed == tick-1);
        gpu_completed = tick;
    }
    submissions.Close(); submitter.join();
    std::atomic<uint64_t> never{0}; std::atomic<bool> broken{false};
    RecordingProgress rejected{never, broken};
    std::atomic<bool> waiting{false}, caught{false};
    std::thread waiter([&] {
        waiting = true; waiting.notify_one();
        try { rejected.Await(77); assert(false); }
        catch (const std::runtime_error& e) { assert(std::string(e.what()) == "device removed"); caught = true; }
    });
    while (!waiting.load()) waiting.wait(false);
    rejected.Fail(std::make_exception_ptr(std::runtime_error("device removed")));
    waiter.join(); assert(caught && never == 0);
    try { rejected.Check(); assert(false); } catch (const std::runtime_error&) {}
    std::cout << "temporary object/input lifetime, 96 submits, ack vs completion, failure wake/check OK\n";
}
"""
    return preamble


def startup_source():
    context = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
    start = context.index("void Context::StartRecorder() {")
    end = context.index("void Context::RecorderBegin()", start)
    return (
        r"""
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include "video_core/renderer_d3d12/d3d12_record_stream.h"
#include "video_core/renderer_d3d12/d3d12_recording_progress.h"
static int failure_stage;
static void InjectFailure(int stage) {
    if (failure_stage == stage) throw std::runtime_error("startup failure");
}
namespace Common {
enum class ThreadPriority { Critical };
void SetCurrentThreadName(const char*) { InjectFailure(1); }
void SetCurrentThreadPriority(ThreadPriority) { InjectFailure(2); }
}
namespace D3D12 {
inline bool BisectEnabled(const char*) { return false; }
struct RecordingTarget { void* owner{}; int *list{}, *list1{}, *list8{}; };
struct FakeList {
    int value{};
    int* Get() { return &value; }
    int As(int** output) { InjectFailure(4); *output = &value; return 0; }
};
struct FakeExtended {
    int* value{};
    int** operator&() { return &value; }
    int* Get() { return value; }
};
struct Context {
    std::unique_ptr<RecordStream<>> stream;
    std::atomic<uint64_t> submitted{};
    std::atomic<bool> submit_failed{};
    RecordingProgress recorder_progress{submitted, submit_failed};
    std::thread recorder;
    struct {
        bool Enabled() { return false; }
        void BindRecorder(std::thread::native_handle_type) {}
    } profile;
    struct { int depth_bounds_status{}, independent_stencil_status{}; } facade;
    FakeList commands;
    FakeExtended extended1, extended8;
    void InitializeLists() { InjectFailure(3); }
    void StartRecorder();
};
"""
        + context[start:end]
        + r"""
}
int main() {
    for (failure_stage = 1; failure_stage <= 4; ++failure_stage) {
        D3D12::Context context;
        try { context.StartRecorder(); assert(false); }
        catch (const std::runtime_error& error) {
            assert(std::string(error.what()) == "startup failure");
        }
        assert(!context.recorder.joinable() && context.submit_failed);
    }
    std::cout << "recorder name/priority/list/interface startup failures propagate and join OK\n";
}
"""
    )


class DeferredRecording(unittest.TestCase):
    def run_binary(self, sanitizer, make_source=source):
        with tempfile.TemporaryDirectory(prefix="d3d12-recorder-") as tmp:
            cpp = Path(tmp) / "test.cpp"
            binary = Path(tmp) / "test"
            cpp.write_text(make_source())
            # The validator has its own behavior test. Transport tests leave it disabled.
            from test_d3d12_command_validation import FMT

            (Path(tmp) / "fmt").mkdir()
            (Path(tmp) / "fmt/format.h").write_text(FMT)
            compilation = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O1",
                    "-g",
                    "-pthread",
                    "-fsanitize=" + sanitizer,
                    "-fno-omit-frame-pointer",
                    "-I" + tmp,
                    "-I" + str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(result.stderr, result.stderr)
            print(sanitizer + ": " + result.stdout, end="")

    def test_address_undefined(self):
        self.run_binary("address,undefined")

    def test_startup_address_undefined(self):
        self.run_binary("address,undefined", startup_source)

    def test_startup_thread(self):
        self.run_binary("thread", startup_source)

    def test_thread(self):
        self.run_binary("thread")


if __name__ == "__main__":
    unittest.main()
