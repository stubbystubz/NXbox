# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay production encoder, workers and native submission against deterministic D3D12 lists."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r"""
#include <cassert>
#include <iostream>
#include <string>
#include "video_core/renderer_d3d12/d3d12_parallel_record.h"
using namespace D3D12;
using Key = ParallelStateKey;
struct Object {
    std::atomic<unsigned> references{1};
    void AddRef() { ++references; }
    void Release() { assert(references.fetch_sub(1) > 1); }
};
struct List {
    std::vector<std::string> calls;
    std::map<Key, std::vector<unsigned>> state;
    std::vector<decltype(state)> draws;
    std::atomic<bool>* own_started{};
    std::atomic<bool>* other_started{};
    void Set(Key key, const unsigned* words, size_t size, Object*) {
        state[key] = std::vector<unsigned>(words, words + size);
        std::string call = std::to_string(unsigned(key.state)) + ":" +
                           std::to_string(key.slot) + ":" + std::to_string(key.word);
        for (size_t i = 0; i < size; ++i) call += ":" + std::to_string(words[i]);
        calls.push_back(call);
    }
    void Draw(unsigned id) {
        // Force a real overlap: neither recorder can complete its first draw until
        // the other recorder is inside its own draw. Serial replay deadlocks here.
        if (own_started) {
            own_started->store(true); own_started->notify_all();
            while (!other_started->load()) other_started->wait(false);
            own_started = nullptr;
        }
        calls.push_back("Draw:" + std::to_string(id));
        draws.push_back(state);
    }
};
static void Set(void* target, Key key, const unsigned* words, size_t size, Object* object) {
    static_cast<List*>(target)->Set(key, words, size, object);
}
static void Draw(void* target, unsigned id) { static_cast<List*>(target)->Draw(id); }
struct CompactDraw {
    unsigned id;
    void Replay(void* target) const { Draw(target, id); }
};
int main() {
    ParallelRecordCuts cuts{2};
    cuts.Reset(false); cuts.Draw(); cuts.Draw(); assert(!cuts.TryCut());
    cuts.Reset(true); cuts.Draw(); assert(!cuts.TryCut()); cuts.Draw();
    cuts.BeginQuery(10, 1, 4); cuts.BeginQuery(11, 1, 4);
    assert(!cuts.TryCut()); cuts.EndQuery(10, 1, 4); assert(!cuts.TryCut());
    cuts.BeginScope(); cuts.EndQuery(11, 1, 4); assert(!cuts.TryCut());
    cuts.EndScope(); assert(cuts.TryCut()); assert(!cuts.TryCut());
    cuts.Reset(true); cuts.Draw(); cuts.Draw(); cuts.EndQuery(10, 1, 4);
    assert(!cuts.TryCut());
    cuts.Reset(true); cuts.Draw(); cuts.Draw(); cuts.Disable(); assert(!cuts.TryCut());
    cuts.Reset(true); cuts.Draw(); cuts.Draw(); cuts.BeginQuery(10, 1, 4);
    cuts.BeginQuery(10, 1, 4); cuts.EndQuery(10, 1, 4); assert(!cuts.TryCut());
    cuts.Reset(true); cuts.Draw(); cuts.Draw(); cuts.EndScope(); assert(!cuts.TryCut());
    std::cout << "cut policy: opt-out, threshold, nested queries/scopes, one cut, fail closed OK\n";

    Object object;
    List first, second, expected_first, expected_second;
    std::atomic<bool> first_started{}, second_started{};
    first.own_started = &first_started; first.other_started = &second_started;
    second.own_started = &second_started; second.other_started = &first_started;
    ParallelRecordSnapshot snapshot;
    std::map<Key, std::vector<unsigned>> state;
    RecordStream<> stream;
    ParallelRecordWorker worker{[&]() -> void* { return &second; }};
    std::thread recorder{[&] { while (stream.Consume(&first)) {} }};
    // Cover every state category, root slots and partial constants/IA slot updates.
    for (unsigned category = 0; category <= unsigned(ParallelState::Predication); ++category) {
        Key key{ParallelState(category), 0, 0};
        unsigned data[4]{category, 0x80000000U, 0xffffffffU, category + 91};
        state[key] = {data, data + 4};
        snapshot.Remember<Set>(key, key, Snapshot(data, 4), size_t{4}, &object);
        EncodeCall<Set>(stream, key, Snapshot(data, 4), size_t{4}, &object);
        expected_first.Set(key, data, 4, &object);
        data[0] = 0; // Caller storage must not back a snapshot.
    }
    for (unsigned i = 0; i < 64; ++i) {
        Key key{ParallelState::GraphicsRootArgument, 7, i};
        unsigned data[1]{i + 400};
        state[key] = {data, data + 1};
        snapshot.Remember<Set>(key, key, Snapshot(data, 1), size_t{1}, &object);
        EncodeCall<Set>(stream, key, Snapshot(data, 1), size_t{1}, &object);
        expected_first.Set(key, data, 1, &object);
    }
    stream.Repeat(CompactDraw{0}); expected_first.Draw(0);
    // Publishing the first queue before switching is part of RouteWritesTo.
    stream.RouteWritesTo(&worker.Stream());
    snapshot.Encode(stream);
    for (const auto& [key, data] : state)
        expected_second.Set(key, data.data(), data.size(), &object);
    stream.Repeat(CompactDraw{1}); expected_second.Draw(1);
    // Mutating/resetting encoder state must not mutate the already encoded packet.
    unsigned replacement[2]{55, 66};
    const Key blend{ParallelState::BlendFactor, 0, 0};
    snapshot.Remember<Set>(blend, blend, Snapshot(replacement, 2), size_t{2}, &object);
    snapshot.Reset();
    // Query boundaries stay on the second list, with a deterministic call trace.
    EncodeCall<[](void* target) { static_cast<List*>(target)->calls.push_back("BeginQuery"); }>(stream);
    EncodeCall<Draw>(stream, 2U); expected_second.calls.push_back("BeginQuery"); expected_second.Draw(2);
    EncodeCall<[](void* target) { static_cast<List*>(target)->calls.push_back("EndQuery"); }>(stream);
    expected_second.calls.push_back("EndQuery");
    stream.RouteWritesTo(nullptr);
    worker.Publish(1);
    std::vector<List*> submitted;
    EncodeCall<[](void* target, std::uintptr_t address, std::uintptr_t output, std::uintptr_t next) {
        auto& secondary = *reinterpret_cast<ParallelRecordWorker*>(address);
        secondary.Wait(1);
        auto& lists = *reinterpret_cast<std::vector<List*>*>(output);
        // Models exactly one ExecuteCommandLists call containing both ordered lists.
        assert(lists.empty());
        lists = {static_cast<List*>(target), reinterpret_cast<List*>(next)};
    }>(stream, reinterpret_cast<std::uintptr_t>(&worker),
       reinterpret_cast<std::uintptr_t>(&submitted), reinterpret_cast<std::uintptr_t>(&second));
    stream.Close(); recorder.join();
    assert(first.calls == expected_first.calls && second.calls == expected_second.calls);
    assert(first.draws == expected_first.draws && second.draws == expected_second.draws);
    assert(submitted == std::vector<List*>({&first, &second}));
    assert(object.references == 1);
    std::cout << "two overlapping recorders: exact per-list native sequence and draw state, ordered single execute OK\n";

    // Forget invalidated root arguments without retaining stale COM state.
    ParallelRecordSnapshot invalidated;
    unsigned value = 99;
    invalidated.Remember<Set>(Key{ParallelState::GraphicsRootArgument, 3}, Key{ParallelState::GraphicsRootArgument, 3}, Snapshot(&value, 1), size_t{1}, &object);
    invalidated.Forget(ParallelState::GraphicsRootArgument);
    RecordStream<> empty; invalidated.Encode(empty); empty.Close();
    List reset; while (empty.Consume(&reset)) {} assert(reset.calls.empty());
    assert(object.references == 1);
    // Discarded snapshots and throwing snapshot replay retire every retained hold.
    ParallelRecordSnapshot abandoned;
    abandoned.Remember<Set>(blend, blend, Snapshot(&value, 1), size_t{1}, &object);
    RecordStream<> discard; abandoned.Encode(discard); abandoned.Reset(); discard.Close();
    while (discard.Consume(nullptr, false)) {} assert(object.references == 1);
    abandoned.Remember<[](void*, Object*) { throw std::runtime_error("snapshot failure"); }>(blend, &object);
    RecordStream<> throwing; abandoned.Encode(throwing); abandoned.Reset(); throwing.Close();
    try { throwing.Consume(nullptr); assert(false); } catch (const std::runtime_error&) {}
    assert(object.references == 1);
    std::cout << "snapshot mutation/reset/invalidation, arrays, retained COM, discard and throwing replay OK\n";

    // Unrouted encoding remains the ordinary serial opt-out path.
    RecordStream<> serial; List serial_list;
    for (unsigned i = 0; i < 100; ++i) serial.Repeat(CompactDraw{i});
    serial.Close(); while (serial.Consume(&serial_list)) {}
    assert(serial_list.calls.size() == 100);
    RecordStream<> a, b, c;
    a.RouteWritesTo(&b);
    bool rejected = false;
    try { c.RouteWritesTo(&a); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected); a.RouteWritesTo(nullptr);
    rejected = false;
    try { a.RouteWritesTo(&a); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    std::cout << "serial opt-out, compact draw routing and invalid route rejection OK\n";

    try {
        ParallelRecordWorker bad{[]() -> void* { throw std::runtime_error("startup failure"); }};
        assert(false);
    } catch (const std::runtime_error& error) { assert(std::string(error.what()) == "startup failure"); }
    {
        ParallelRecordWorker bad{[]() -> void* { return nullptr; }};
        EncodeCall<[](void*) { throw std::runtime_error("worker failure"); }>(bad.Stream());
        // Exceed queue capacity: a failing worker must keep draining COM holds.
        for (unsigned i = 0; i < 150000; ++i)
            EncodeCall<[](void*, Object*) { assert(false); }>(bad.Stream(), &object);
        try { bad.Publish(42); bad.Wait(42); assert(false); }
        catch (const std::runtime_error& error) { assert(std::string(error.what()) == "worker failure"); }
    }
    assert(object.references == 1);
    std::cout << "worker startup/replay failure, bounded-queue drain, error wake and join OK\n";
}
"""


FACADE_MAIN = r"""
int main() {
    ID3D12Device device;
    ID3D12GraphicsCommandList first, second, expected_first, expected_second;
    RecordingTarget a{}, b{}; a.list = &first; b.list = &second;
    a.list1 = a.list8 = &first; b.list1 = b.list8 = &second;
    RecordStream<> stream;
    RecordDescriptors ordinary; ordinary.Initialize(&device);
    CommandFacade encoder;
    encoder.stream = &stream; encoder.descriptors = &ordinary;
    encoder.parallel_descriptors = std::make_shared<RecordDescriptors>();
    encoder.parallel_descriptors->Initialize(&device);
    encoder.parallel_enabled = true;
    encoder.parallel_cuts = ParallelRecordCuts{2};
    encoder.BeginParallelRecording();
    ParallelRecordWorker worker{[&]() -> void* { return &b; }};
    encoder.parallel_begin = [&] { stream.RouteWritesTo(&worker.Stream()); };
    std::thread recorder{[&] { while (stream.Consume(&a)) {} }};
    ID3D12RootSignature graphics, compute;
    ID3D12PipelineState pipeline;
    ID3D12DescriptorHeap heap;
    ID3D12DescriptorHeap* heaps[]{&heap};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[]{{100}, {101}}, depth{200};
    descriptors[100] = 701; descriptors[101] = 702; descriptors[200] = 703;
    D3D12_VIEWPORT viewport{1, 2, 300, 400, 0, 1};
    D3D12_RECT scissor{2, 3, 290, 390};
    D3D12_VERTEX_BUFFER_VIEW vertices[]{{1000, 64, 8}, {2000, 128, 16}};
    D3D12_VERTEX_BUFFER_VIEW replacement{3000, 256, 32};
    D3D12_INDEX_BUFFER_VIEW index{4000, 64, 7};
    D3D12_STREAM_OUTPUT_BUFFER_VIEW so{5000, 64, 6000};
    ID3D12Resource predicate;
    FLOAT blend[]{.1f, .2f, .3f, .4f};
    UINT constants[]{11, 22, 33}, changed = 99;
    // The independent reference snapshot uses explicit native calls in replay order.
    auto setup = [&](auto& target) {
        target.SetDescriptorHeaps(1, heaps);
        target.SetGraphicsRootSignature(&graphics);
        target.SetComputeRootSignature(&compute);
        target.SetPipelineState(&pipeline);
        target.SetGraphicsRootConstantBufferView(0, 10000);
        target.SetGraphicsRootShaderResourceView(1, 20000);
        target.SetGraphicsRootUnorderedAccessView(2, 30000);
        target.SetComputeRootConstantBufferView(0, 40000);
        target.SetComputeRootShaderResourceView(1, 50000);
        target.SetComputeRootUnorderedAccessView(2, 60000);
        target.SetGraphicsRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{70000});
        target.SetComputeRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{80000});
        target.OMSetRenderTargets(2, rtvs, false, &depth);
        target.RSSetViewports(1, &viewport);
        target.RSSetScissorRects(1, &scissor);
        target.IASetIndexBuffer(&index);
        target.OMSetFrontAndBackStencilRef(12, 34);
        target.OMSetBlendFactor(blend);
        target.IASetPrimitiveTopology(5);
        target.OMSetDepthBounds(.25f, .75f);
        target.SOSetTargets(0, 1, &so);
        target.SetPredication(&predicate, 8, 1);
    };
    setup(encoder); setup(expected_first);
    encoder.SetGraphicsRoot32BitConstants(4, 3, constants, 0);
    expected_first.SetGraphicsRoot32BitConstants(4, 3, constants, 0);
    encoder.SetGraphicsRoot32BitConstants(4, 1, &changed, 1);
    expected_first.SetGraphicsRoot32BitConstants(4, 1, &changed, 1);
    encoder.SetComputeRoot32BitConstants(4, 3, constants, 2);
    expected_first.SetComputeRoot32BitConstants(4, 3, constants, 2);
    encoder.IASetVertexBuffers(2, 2, vertices);
    expected_first.IASetVertexBuffers(2, 2, vertices);
    encoder.IASetVertexBuffers(3, 1, &replacement);
    expected_first.IASetVertexBuffers(3, 1, &replacement);
    ID3D12QueryHeap query;
    encoder.DrawInstanced(3, 1, 0, 0); expected_first.DrawInstanced(3, 1, 0, 0);
    encoder.BeginQuery(&query, 0, 7); expected_first.BeginQuery(&query, 0, 7);
    // Threshold is reached inside the query. Three further draws must stay on list 1.
    for (unsigned i = 0; i < 4; ++i) {
        encoder.DrawIndexedInstanced(6, 1, i, -2, 0);
        expected_first.DrawIndexedInstanced(6, 1, i, -2, 0);
        assert(!encoder.parallel_split);
    }
    encoder.EndQuery(&query, 0, 7); expected_first.EndQuery(&query, 0, 7);
    encoder.DrawInstanced(9, 1, 0, 0); assert(encoder.parallel_split);
    // Build the full canonical native snapshot independently of Remember/EncodeState.
    expected_second.SetDescriptorHeaps(1, heaps);
    expected_second.SetGraphicsRootSignature(&graphics);
    expected_second.SetComputeRootSignature(&compute);
    expected_second.SetPipelineState(&pipeline);
    expected_second.SetGraphicsRootConstantBufferView(0, 10000);
    expected_second.SetGraphicsRootShaderResourceView(1, 20000);
    expected_second.SetGraphicsRootUnorderedAccessView(2, 30000);
    constants[1] = changed;
    for (UINT i = 0; i < 3; ++i)
        expected_second.SetGraphicsRoot32BitConstants(4, 1, constants + i, i);
    constants[1] = 22;
    expected_second.SetComputeRootConstantBufferView(0, 40000);
    expected_second.SetComputeRootShaderResourceView(1, 50000);
    expected_second.SetComputeRootUnorderedAccessView(2, 60000);
    for (UINT i = 0; i < 3; ++i)
        expected_second.SetComputeRoot32BitConstants(4, 1, constants + i, i + 2);
    expected_second.SetGraphicsRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{70000});
    expected_second.SetComputeRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{80000});
    expected_second.OMSetRenderTargets(2, rtvs, false, &depth);
    expected_second.RSSetViewports(1, &viewport);
    expected_second.RSSetScissorRects(1, &scissor);
    expected_second.IASetVertexBuffers(2, 1, vertices);
    expected_second.IASetVertexBuffers(3, 1, &replacement);
    expected_second.IASetIndexBuffer(&index);
    expected_second.OMSetFrontAndBackStencilRef(12, 34);
    expected_second.OMSetBlendFactor(blend);
    expected_second.IASetPrimitiveTopology(5);
    expected_second.OMSetDepthBounds(.25f, .75f);
    expected_second.SOSetTargets(0, 1, &so);
    expected_second.SetPredication(&predicate, 8, 1);
    expected_second.DrawInstanced(9, 1, 0, 0);
    // Caller memory, source descriptors and live state change after encoding the cut.
    descriptors[100] = 999; constants[0] = 999; vertices[0].SizeInBytes = 999;
    encoder.SetPipelineState(nullptr); expected_second.SetPipelineState(nullptr);
    encoder.DrawIndexedInstanced(12, 2, 1, 3, 4);
    expected_second.DrawIndexedInstanced(12, 2, 1, 3, 4);
    stream.RouteWritesTo(nullptr);
    encoder.parallel_snapshot.Reset();
    worker.Publish(1); worker.Wait(1);
    stream.Close(); recorder.join();
    assert(first.calls == expected_first.calls);
    assert(second.calls == expected_second.calls);
    std::cout << "production facade: full native snapshot, partial roots/IA, query-safe cut, immutable descriptors and exact per-list calls OK\n";

    // Native signature changes invalidate arguments; repeated identical signatures preserve them.
    RecordStream<> invalidation;
    CommandFacade c; c.stream = &invalidation; c.parallel_enabled = true;
    c.BeginParallelRecording();
    c.SetGraphicsRootSignature(&graphics);
    c.SetGraphicsRootConstantBufferView(0, 42);
    c.SetGraphicsRootSignature(&graphics);
    c.SetComputeRootSignature(&compute);
    c.SetComputeRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{77});
    c.SetDescriptorHeaps(1, heaps); // Invalidates the earlier table.
    c.SetGraphicsRootSignature(&compute); // Invalidates the earlier CBV.
    RecordStream<> snapshot;
    c.parallel_snapshot.Encode(snapshot); c.parallel_snapshot.Reset();
    snapshot.Close(); ID3D12GraphicsCommandList rebuilt; RecordingTarget rt{}; rt.list = &rebuilt;
    while (snapshot.Consume(&rt)) {}
    ID3D12GraphicsCommandList expected_reset;
    expected_reset.SetDescriptorHeaps(1, heaps);
    expected_reset.SetGraphicsRootSignature(&compute);
    expected_reset.SetComputeRootSignature(&compute);
    assert(rebuilt.calls == expected_reset.calls);
    invalidation.Close(); while (invalidation.Consume(nullptr, false)) {}
    RecordStream<> timestamp; c.stream = &timestamp;
    c.parallel_cuts = ParallelRecordCuts{0}; c.BeginParallelRecording();
    c.EndQuery(&query, static_cast<D3D12_QUERY_TYPE>(2), 9);
    assert(c.parallel_cuts.TryCut()); // A timestamp has no matching BeginQuery.
    timestamp.Close(); while (timestamp.Consume(nullptr, false)) {}
    // Unsupported indirect mutation and split barriers suppress the pending cut.
    RecordStream<> blocked;
    c.stream = &blocked; c.parallel_cuts = ParallelRecordCuts{0}; c.BeginParallelRecording();
    ID3D12CommandSignature signature;
    c.ExecuteIndirect(&signature, 1, nullptr, 0, nullptr, 0);
    c.DrawInstanced(1, 1, 0, 0); assert(!c.parallel_split);
    c.BeginParallelRecording();
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Flags = 1;
    c.ResourceBarrier(1, &barrier);
    c.DrawInstanced(1, 1, 0, 0); assert(!c.parallel_split);
    c.BeginParallelRecording();
    ID3D12DescriptorHeap other_heap; ID3D12DescriptorHeap* other_heaps[]{&other_heap};
    c.SetDescriptorHeaps(1, heaps);
    c.SetDescriptorHeaps(1, other_heaps);
    c.DrawInstanced(1, 1, 0, 0); assert(!c.parallel_split);
    blocked.Close(); while (blocked.Consume(nullptr, false)) {}
    c.parallel_snapshot.Reset();
    std::cout << "production invalidation: signature/heaps and unsupported mutations fail closed OK\n";
}
"""


NATIVE_PREAMBLE = r"""
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using u64 = uint64_t;
using HRESULT = int;
using HANDLE = void*;
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT = 1;
constexpr int EVENT_ALL_ACCESS = 1, WAIT_OBJECT_0 = 0, WAIT_FAILED = -1, ERROR_TIMEOUT = 2;
constexpr bool FALSE = false;
#define IID_PPV_ARGS(value) value
#define FAILED(value) ((value) != 0)
#define HRESULT_FROM_WIN32(value) (value)
static unsigned live_events{}, waits{};
static int wait_result{}, signal_error{}, reset_error{};
inline int GetLastError() { return 3; }
inline HANDLE CreateEventExW(void*, void*, unsigned, int) { ++live_events; return reinterpret_cast<void*>(1); }
inline void CloseHandle(HANDLE) { assert(live_events); --live_events; }
namespace Common {
enum class ThreadPriority { Critical };
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadPriority(ThreadPriority) {}
}
namespace D3D12 {
inline void Check(int result, const char* name, void* = nullptr) {
    if (result) throw std::runtime_error(name);
}
template<class T> struct ComPtr {
    T* value{};
    T* Get() const { return value; }
    T* operator->() const { return value; }
    T** operator&() { return &value; }
    template<class U> int As(U** target) { *target = value; return 0; }
};
struct ID3D12CommandAllocator {
    unsigned resets{};
    int Reset() { ++resets; return reset_error; }
};
struct ID3D12CommandList { virtual ~ID3D12CommandList() = default; };
struct ID3D12GraphicsCommandList : ID3D12CommandList {
    bool closed{};
    unsigned resets{};
    ID3D12CommandAllocator* allocator{};
    int Close() { assert(!closed); closed = true; return 0; }
    int Reset(ID3D12CommandAllocator* next, void*) {
        assert(closed); closed = false; ++resets; allocator = next; return reset_error;
    }
};
using ID3D12GraphicsCommandList1 = ID3D12GraphicsCommandList;
using ID3D12GraphicsCommandList8 = ID3D12GraphicsCommandList;
struct RecordingTarget {
    ID3D12GraphicsCommandList *list{}, *list1{}, *list8{};
};
struct ID3D12Fence {
    u64 completed{}, pending{}, signaled{};
    u64 GetCompletedValue() { return completed; }
    int SetEventOnCompletion(u64 tick, HANDLE) { pending = tick; return signal_error; }
};
static ID3D12Fence* waiting_fence;
inline int WaitForSingleObjectEx(HANDLE, int, bool) {
    ++waits; waiting_fence->completed = waiting_fence->pending; return wait_result;
}
struct ID3D12Device {
    std::vector<std::unique_ptr<ID3D12CommandAllocator>> allocators;
    std::vector<std::unique_ptr<ID3D12GraphicsCommandList>> lists;
    int CreateCommandAllocator(int, ID3D12CommandAllocator** result) {
        allocators.push_back(std::make_unique<ID3D12CommandAllocator>());
        *result = allocators.back().get(); return 0;
    }
    int CreateCommandList(int, int, ID3D12CommandAllocator*, void*, ID3D12GraphicsCommandList** result) {
        lists.push_back(std::make_unique<ID3D12GraphicsCommandList>());
        *result = lists.back().get(); return 0;
    }
    int GetDeviceRemovedReason() { return 0; }
};
"""

NATIVE_MIDDLE = r"""
struct Queue {
    std::vector<std::vector<ID3D12CommandList*>> executions;
    void ExecuteCommandLists(unsigned count, ID3D12CommandList* const* lists) {
        for (unsigned i = 0; i < count; ++i)
            assert(static_cast<ID3D12GraphicsCommandList*>(lists[i])->closed);
        executions.emplace_back(lists, lists + count);
    }
    int Signal(ID3D12Fence* fence, u64 tick) { fence->signaled = tick; return 0; }
};
namespace NxboxWait { enum class Counter { SubmittedTick }; void Max(Counter, u64) {} }
inline void ClearInfoQueue(int) {}
inline void LogDeviceInfoQueue(ID3D12Device*) {}
struct Context {
    struct { std::shared_ptr<ParallelNativeState> parallel_native; } facade;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<Queue> queue;
    struct { bool registered{}; struct { int Get() { return 0; } } queue; } info_callback;
    struct Slot { u64 tick{}; };
    std::vector<Slot> allocators{3};
    size_t allocator_index{};
    struct { u64 tick{}; void Acknowledge(u64 next) { tick = next; } } recorder_progress;
    void RecorderSubmit(u64 tick);
};
"""

NATIVE_MAIN = r"""
}
int main() {
    using namespace D3D12;
    ID3D12Device device;
    ID3D12Fence fence; waiting_fence = &fence;
    auto parallel = std::make_shared<ParallelNativeState>();
    parallel->Initialize(&device, &fence, 3);
    assert(device.allocators.size() == 3 && parallel->list->closed);
    assert(parallel->target.list1 && parallel->target.list8);
    Context context; Queue queue; ID3D12GraphicsCommandList first;
    context.device.value = &device; context.fence.value = &fence;
    context.commands.value = &first; context.queue.value = &queue;
    context.facade.parallel_native = parallel;
    parallel->Begin(1); assert(parallel->slots[0].tick == 1);
    parallel->list->Close(); parallel->submit_second = true;
    context.RecorderSubmit(1);
    assert(queue.executions.size() == 1);
    assert(queue.executions[0] == std::vector<ID3D12CommandList*>({&first, parallel->list.Get()}));
    assert(fence.signaled == 1 && context.recorder_progress.tick == 1);
    assert(context.allocators[0].tick == 1 && !parallel->submit_second);
    // A serial submission remains one list even after a parallel submission.
    first.closed = false; context.RecorderSubmit(2);
    assert(queue.executions[1] == std::vector<ID3D12CommandList*>({&first}));
    parallel->Begin(4);
    assert(waits == 1 && fence.completed == 1 && !live_events);
    assert(parallel->slots[0].allocator->resets == 2);
    parallel->list->Close();
    signal_error = 1;
    try { parallel->Begin(7); assert(false); }
    catch (const std::runtime_error& error) { assert(std::string(error.what()) == "parallel allocator fence"); }
    assert(!live_events);
    signal_error = 0; wait_result = WAIT_FAILED;
    try { parallel->Begin(7); assert(false); }
    catch (const std::runtime_error& error) { assert(std::string(error.what()) == "parallel allocator wait"); }
    assert(!live_events);
    std::cout << "production native lifecycle: allocator fence reuse, event cleanup, ordered single execute and serial fallback OK\n";
}
"""


class ParallelRecording(unittest.TestCase):
    def run_binary(self, sanitizer, source_text=SOURCE):
        with tempfile.TemporaryDirectory(prefix="d3d12-parallel-") as directory:
            source = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            source.write_text(source_text)
            from test_d3d12_command_validation import FMT

            (Path(directory) / "fmt").mkdir()
            (Path(directory) / "fmt/format.h").write_text(FMT)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O1",
                    "-g",
                    "-pthread",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=" + sanitizer,
                    "-fno-omit-frame-pointer",
                    "-I" + directory,
                    "-I" + str(ROOT / "src"),
                    str(source),
                    str(ROOT / "src/video_core/renderer_d3d12/d3d12_parallel_record.cpp"),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(result.stderr, result.stderr)
            print(sanitizer + ":\n" + result.stdout, end="")

    def facade_source(self):
        from test_d3d12_deferred_recording import source

        return source().split("int main() {", 1)[0] + FACADE_MAIN

    def test_facade_address_undefined(self):
        self.run_binary("address,undefined", self.facade_source())

    def test_facade_thread(self):
        self.run_binary("thread", self.facade_source())

    def native_source(self):
        context = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        native = context.split("struct ParallelNativeState {", 1)[1].split(
            "bool BisectEnabled(", 1
        )[0]
        submit = context.split("void Context::RecorderSubmit(", 1)[1].split(
            "u64 Context::SealRecording", 1
        )[0]
        return (
            NATIVE_PREAMBLE
            + "struct ParallelNativeState {"
            + native
            + NATIVE_MIDDLE
            + "void Context::RecorderSubmit("
            + submit
            + NATIVE_MAIN
        )

    def test_native_lifecycle_address_undefined(self):
        self.run_binary("address,undefined", self.native_source())

    def test_native_wiring(self):
        context = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        self.assertIn('BisectEnabled("NXBOX_D3D12_PARALLEL_RECORD")', context)
        self.assertIn("->Acknowledge(tick - 1)", context)
        seal = context.split("u64 Context::SealRecording(", 1)[1].split("Context::~Context", 1)[0]
        self.assertLess(seal.index('"parallel Close"'), seal.index("RouteWritesTo(nullptr)"))
        self.assertLess(seal.index("->Publish(tick)"), seal.index("->Wait(tick)"))
        self.assertLess(seal.index("->Wait(tick)"), seal.index("->RecorderSubmit(tick)"))
        submit = context.split("void Context::RecorderSubmit(", 1)[1].split(
            "u64 Context::SealRecording", 1
        )[0]
        self.assertIn("lists[]{commands.Get(), second ? parallel->list.Get() : nullptr}", submit)
        self.assertIn("ExecuteCommandLists(second ? 2 : 1, lists)", submit)
        self.assertIn("facade.BeginParallelRecording()", context)
        cmake = (ROOT / "src/video_core/CMakeLists.txt").read_text()
        self.assertIn("renderer_d3d12/d3d12_parallel_record.cpp", cmake)

    def test_address_undefined(self):
        self.run_binary("address,undefined")

    def test_thread(self):
        self.run_binary("thread")


if __name__ == "__main__":
    unittest.main()
