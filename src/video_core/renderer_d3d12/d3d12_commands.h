// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/renderer_d3d12/d3d12_command_validation.h"
#include "video_core/renderer_d3d12/d3d12_parallel_record.h"
#include "video_core/renderer_d3d12/d3d12_record_args.h"
#include "video_core/renderer_d3d12/d3d12_recorded_state.h"

#include "video_core/renderer_d3d12/d3d12_record_descriptors.h"

namespace D3D12 {
template <>
struct RecordLease<D3D12_TEXTURE_COPY_LOCATION> {
    static void Hold(const D3D12_TEXTURE_COPY_LOCATION& v) {
        RecordLease<ID3D12Resource*>::Hold(v.pResource);
    }
    static void Drop(const D3D12_TEXTURE_COPY_LOCATION& v) {
        RecordLease<ID3D12Resource*>::Drop(v.pResource);
    }
};
template <>
struct RecordLease<D3D12_RESOURCE_BARRIER> {
    template <bool Holding>
    static void Visit(const D3D12_RESOURCE_BARRIER& v) {
        auto apply = [](ID3D12Resource* r) {
            if constexpr (Holding)
                RecordLease<ID3D12Resource*>::Hold(r);
            else
                RecordLease<ID3D12Resource*>::Drop(r);
        };
        switch (v.Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            apply(v.Transition.pResource);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
            apply(v.Aliasing.pResourceBefore);
            apply(v.Aliasing.pResourceAfter);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            apply(v.UAV.pResource);
            break;
        }
    }
    static void Hold(const D3D12_RESOURCE_BARRIER& v) {
        Visit<true>(v);
    }
    static void Drop(const D3D12_RESOURCE_BARRIER& v) {
        Visit<false>(v);
    }
};
// Extended interfaces are queried once on the recorder, and checked only when used.
struct RecordingTarget {
    void* owner{};
    ID3D12GraphicsCommandList* list{};
    ID3D12GraphicsCommandList1* list1{};
    ID3D12GraphicsCommandList8* list8{};
    void OMSetDepthBounds(FLOAT minimum, FLOAT maximum) {
        if (!list1)
            throw std::runtime_error("D3D12 depth-bounds command interface unavailable");
        list1->OMSetDepthBounds(minimum, maximum);
    }
    void OMSetFrontAndBackStencilRef(UINT front, UINT back) {
        if (!list8)
            throw std::runtime_error("D3D12 independent stencil command interface unavailable");
        list8->OMSetFrontAndBackStencilRef(front, back);
    }
};
#if defined(_MSC_VER)
#define D3D12_FORWARD_INLINE __forceinline
#else
#define D3D12_FORWARD_INLINE inline __attribute__((always_inline))
#endif
struct ParallelNativeState;
class CommandFacade {
public:
    const bool reuse_graphics_root_cbvs{BisectEnabled("NXBOX_D3D12_ROOT_CBV_REUSE")};
    GraphicsRootCbvCache graphics_root_cbvs;
    CommandValidation validation;
    ID3D12Device* device{};
    int32_t depth_bounds_status{}, independent_stencil_status{};
    RecordDescriptors* descriptors{};
    ID3D12GraphicsCommandList* immediate{};
    RecordStream<>* stream{}; // Latched once; null is the immediate path.
    // Native resources live in the context translation unit; the encoder owns routing.
    std::shared_ptr<RecordDescriptors> parallel_descriptors;
    std::shared_ptr<ParallelNativeState> parallel_native;
    std::shared_ptr<ParallelRecordWorker> parallel_worker;
    std::function<void()> parallel_begin;
    std::vector<ID3D12DescriptorHeap*> parallel_heaps;
    ParallelRecordSnapshot parallel_snapshot;
    ParallelRecordCuts parallel_cuts{1790};
    bool parallel_enabled{}, parallel_split{};
    ID3D12RootSignature* parallel_graphics_signature{};
    ID3D12RootSignature* parallel_compute_signature{};
    bool CaptureParallelState() const {
        return parallel_enabled && !parallel_split;
    }
    void BeginParallelRecording() {
        parallel_heaps.clear();
        parallel_snapshot.Reset();
        parallel_cuts.Reset(parallel_enabled);
        parallel_split = false;
        parallel_graphics_signature = parallel_compute_signature = nullptr;
    }
    void BeforeParallelDraw() {
        if (!parallel_enabled)
            return;
        if (parallel_cuts.TryCut()) {
            parallel_begin();
            parallel_snapshot.Encode(*stream);
            parallel_snapshot.Reset();
            parallel_split = true;
        }
        parallel_cuts.Draw();
    }
    template <auto Invoke, class... Args>
    void EncodeState(ParallelStateKey key, Args... args) {
        if (CaptureParallelState())
            parallel_snapshot.Remember<Invoke>(key, args...);
        EncodeCall<Invoke>(*stream, args...);
    }
    bool compact_draws{true}; // NXBOX_D3D12_COMPACT_DRAWS=0 restores EncodeCall.
    bool reuse_draw_state{};  // NXBOX_D3D12_RECORD_STATE=1; independent of guest invalidation.
    struct TransportState {
        RecordedValue<D3D_PRIMITIVE_TOPOLOGY> topology;
        RecordedArray<D3D12_VIEWPORT, 16> viewports;
        RecordedArray<D3D12_RECT, 16> scissors;
        RecordedValue<std::array<FLOAT, 4>> blend;
        RecordedValue<std::array<UINT, 2>> stencil;
        RecordedValue<D3D12_INDEX_BUFFER_VIEW> index;
        RecordedArray<D3D12_VERTEX_BUFFER_VIEW, 32> vertices;
        UINT vertex_start{};
    } transport_state;
    void ResetDrawState() {
        transport_state = {};
    }
    struct DrawPacket {
        UINT count, instances, first, first_instance;
        INT base_vertex;
        UINT indexed;
        void Replay(void* target) const {
            auto* list = static_cast<RecordingTarget*>(target)->list;
            if (indexed)
                list->DrawIndexedInstanced(count, instances, first, base_vertex, first_instance);
            else
                list->DrawInstanced(count, instances, first, first_instance);
        }
    };
    D3D12_FORWARD_INLINE void BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
        if (!stream) {
            immediate->BeginQuery(heap, type, index);
            return;
        }
        if (parallel_enabled)
            parallel_cuts.BeginQuery(reinterpret_cast<std::uintptr_t>(heap), type, index);
        EncodeCall<[](void* target, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
            static_cast<RecordingTarget*>(target)->list->BeginQuery(heap, type, index);
        }>(*stream, heap, type, index);
    }
    D3D12_FORWARD_INLINE void EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
        if (!stream) {
            immediate->EndQuery(heap, type, index);
            return;
        }
        if (parallel_enabled)
            if (type != static_cast<D3D12_QUERY_TYPE>(2)) // D3D12_QUERY_TYPE_TIMESTAMP
                parallel_cuts.EndQuery(reinterpret_cast<std::uintptr_t>(heap), type, index);
        EncodeCall<[](void* target, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
            static_cast<RecordingTarget*>(target)->list->EndQuery(heap, type, index);
        }>(*stream, heap, type, index);
    }
    D3D12_FORWARD_INLINE void ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
                                               UINT start, UINT count, ID3D12Resource* destination,
                                               UINT64 offset) {
        validation.Resolve(heap, type, start, count, destination, offset);
        if (!stream) {
            immediate->ResolveQueryData(heap, type, start, count, destination, offset);
            return;
        }
        EncodeCall<[](void* target, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start,
                      UINT count, ID3D12Resource* destination, UINT64 offset) {
            static_cast<RecordingTarget*>(target)->list->ResolveQueryData(heap, type, start, count,
                                                                          destination, offset);
        }>(*stream, heap, type, start, count, destination, offset);
    }
    D3D12_FORWARD_INLINE void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                    D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                                    UINT8 stencil, UINT count,
                                                    const D3D12_RECT* rects) {
        if (!stream) {
            immediate->ClearDepthStencilView(handle, flags, depth, stencil, count, rects);
            return;
        }
        const auto slot = descriptors->Acquire(*stream);
        const auto copy = descriptors->Dsv(slot);
        descriptors->Copy(copy, handle, true);
        EncodeCall<[](void* target, D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CLEAR_FLAGS flags,
                      FLOAT depth, UINT8 stencil, UINT count, const D3D12_RECT* rects,
                      DescriptorTicket) {
            static_cast<RecordingTarget*>(target)->list->ClearDepthStencilView(
                handle, flags, depth, stencil, count, rects);
        }>(*stream, copy, flags, depth, stencil, count, Snapshot(rects, count),
           DescriptorTicket{descriptors, slot});
    }
    D3D12_FORWARD_INLINE void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                    const FLOAT* color, UINT count,
                                                    const D3D12_RECT* rects) {
        if (!stream) {
            immediate->ClearRenderTargetView(handle, color, count, rects);
            return;
        }
        const auto slot = descriptors->Acquire(*stream);
        const auto copy = descriptors->Rtv(slot);
        descriptors->Copy(copy, handle, false);
        EncodeCall<[](void* target, D3D12_CPU_DESCRIPTOR_HANDLE handle, const FLOAT* color,
                      UINT count, const D3D12_RECT* rects, DescriptorTicket) {
            static_cast<RecordingTarget*>(target)->list->ClearRenderTargetView(handle, color, count,
                                                                               rects);
        }>(*stream, copy, Snapshot(color, 4), count, Snapshot(rects, count),
           DescriptorTicket{descriptors, slot});
    }
    D3D12_FORWARD_INLINE void CopyBufferRegion(ID3D12Resource* destination,
                                               UINT64 destination_offset, ID3D12Resource* source,
                                               UINT64 source_offset, UINT64 size) {
        validation.BufferCopy(destination, destination_offset, source, source_offset, size);
        if (!stream) {
            immediate->CopyBufferRegion(destination, destination_offset, source, source_offset,
                                        size);
            return;
        }
        EncodeCall<[](void* target, ID3D12Resource* destination, UINT64 destination_offset,
                      ID3D12Resource* source, UINT64 source_offset, UINT64 size) {
            static_cast<RecordingTarget*>(target)->list->CopyBufferRegion(
                destination, destination_offset, source, source_offset, size);
        }>(*stream, destination, destination_offset, source, source_offset, size);
    }
    D3D12_FORWARD_INLINE void CopyResource(ID3D12Resource* destination, ID3D12Resource* source) {
        if (!stream) {
            immediate->CopyResource(destination, source);
            return;
        }
        EncodeCall<[](void* target, ID3D12Resource* destination, ID3D12Resource* source) {
            static_cast<RecordingTarget*>(target)->list->CopyResource(destination, source);
        }>(*stream, destination, source);
    }
    D3D12_FORWARD_INLINE void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* destination,
                                                UINT x, UINT y, UINT z,
                                                const D3D12_TEXTURE_COPY_LOCATION* source,
                                                const D3D12_BOX* box) {
        validation.TextureCopy(device, destination, x, y, z, source, box);
        if (!stream) {
            immediate->CopyTextureRegion(destination, x, y, z, source, box);
            return;
        }
        EncodeCall<[](void* target, const D3D12_TEXTURE_COPY_LOCATION* destination, UINT x, UINT y,
                      UINT z, const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box) {
            static_cast<RecordingTarget*>(target)->list->CopyTextureRegion(destination, x, y, z,
                                                                           source, box);
        }>(*stream, Snapshot(destination, 1), x, y, z, Snapshot(source, 1), Snapshot(box, 1));
    }
    D3D12_FORWARD_INLINE void Dispatch(UINT x, UINT y, UINT z) {
        if (!stream) {
            immediate->Dispatch(x, y, z);
            return;
        }
        EncodeCall<[](void* target, UINT x, UINT y, UINT z) {
            static_cast<RecordingTarget*>(target)->list->Dispatch(x, y, z);
        }>(*stream, x, y, z);
    }
    D3D12_FORWARD_INLINE void DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index,
                                                   INT base_vertex, UINT first_instance) {
        if (!stream) {
            immediate->DrawIndexedInstanced(indices, instances, first_index, base_vertex,
                                            first_instance);
            return;
        }
        BeforeParallelDraw();
        if (compact_draws) {
            stream->Repeat(
                DrawPacket{indices, instances, first_index, first_instance, base_vertex, true});
            return;
        }
        EncodeCall<[](void* target, UINT indices, UINT instances, UINT first_index, INT base_vertex,
                      UINT first_instance) {
            static_cast<RecordingTarget*>(target)->list->DrawIndexedInstanced(
                indices, instances, first_index, base_vertex, first_instance);
        }>(*stream, indices, instances, first_index, base_vertex, first_instance);
    }
    D3D12_FORWARD_INLINE void DrawInstanced(UINT vertices, UINT instances, UINT first_vertex,
                                            UINT first_instance) {
        if (!stream) {
            immediate->DrawInstanced(vertices, instances, first_vertex, first_instance);
            return;
        }
        BeforeParallelDraw();
        if (compact_draws) {
            stream->Repeat(DrawPacket{vertices, instances, first_vertex, first_instance, 0, false});
            return;
        }
        EncodeCall<[](void* target, UINT vertices, UINT instances, UINT first_vertex,
                      UINT first_instance) {
            static_cast<RecordingTarget*>(target)->list->DrawInstanced(
                vertices, instances, first_vertex, first_instance);
        }>(*stream, vertices, instances, first_vertex, first_instance);
    }
    D3D12_FORWARD_INLINE void ExecuteIndirect(ID3D12CommandSignature* signature, UINT count,
                                              ID3D12Resource* arguments, UINT64 offset,
                                              ID3D12Resource* count_buffer, UINT64 count_offset) {
        if (!stream) {
            immediate->ExecuteIndirect(signature, count, arguments, offset, count_buffer,
                                       count_offset);
            return;
        }
        // Indirect signatures may write IA views; D3D12 clears those bindings
        // afterwards. The signature is opaque here, so discard all transport state.
        parallel_cuts.Disable();
        ResetDrawState();
        EncodeCall<[](void* target, ID3D12CommandSignature* signature, UINT count,
                      ID3D12Resource* arguments, UINT64 offset, ID3D12Resource* count_buffer,
                      UINT64 count_offset) {
            static_cast<RecordingTarget*>(target)->list->ExecuteIndirect(
                signature, count, arguments, offset, count_buffer, count_offset);
        }>(*stream, signature, count, arguments, offset, count_buffer, count_offset);
    }
    D3D12_FORWARD_INLINE void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
        if (!stream) {
            immediate->IASetIndexBuffer(view);
            return;
        }
        if (reuse_draw_state) {
            if (!view)
                transport_state.index.valid = false;
            else if (!transport_state.index.Update(*view))
                return;
        }
        EncodeState<[](void* target, const D3D12_INDEX_BUFFER_VIEW* view) {
            static_cast<RecordingTarget*>(target)->list->IASetIndexBuffer(view);
        }>({ParallelState::IndexBuffer}, Snapshot(view, 1));
    }
    D3D12_FORWARD_INLINE void IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY topology) {
        if (!stream) {
            immediate->IASetPrimitiveTopology(topology);
            return;
        }
        if (reuse_draw_state) {
            if (!transport_state.topology.Update(topology))
                return;
        }
        EncodeState<[](void* target, D3D_PRIMITIVE_TOPOLOGY topology) {
            static_cast<RecordingTarget*>(target)->list->IASetPrimitiveTopology(topology);
        }>({ParallelState::PrimitiveTopology}, topology);
    }
    D3D12_FORWARD_INLINE void IASetVertexBuffers(UINT start, UINT count,
                                                 const D3D12_VERTEX_BUFFER_VIEW* views) {
        if (!stream) {
            immediate->IASetVertexBuffers(start, count, views);
            return;
        }
        if (reuse_draw_state) {
            if (transport_state.vertex_start != start || !views)
                transport_state.vertices.valid = false;
            transport_state.vertex_start = start;
            if (views && !transport_state.vertices.Update(views, count))
                return;
        }
        if (CaptureParallelState()) {
            for (UINT i = 0; i < count; ++i)
                parallel_snapshot
                    .Remember<[](void* target, UINT slot, const D3D12_VERTEX_BUFFER_VIEW* view) {
                        static_cast<RecordingTarget*>(target)->list->IASetVertexBuffers(slot, 1,
                                                                                        view);
                    }>({ParallelState::VertexBuffer, start + i}, start + i,
                       Snapshot(views ? views + i : nullptr, 1));
        }
        EncodeCall<[](void* target, UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
            static_cast<RecordingTarget*>(target)->list->IASetVertexBuffers(start, count, views);
        }>(*stream, start, count, Snapshot(views, count));
    }
    D3D12_FORWARD_INLINE void OMSetBlendFactor(const FLOAT* factor) {
        if (!stream) {
            immediate->OMSetBlendFactor(factor);
            return;
        }
        if (reuse_draw_state) {
            if (!factor)
                transport_state.blend.valid = false;
            else if (!transport_state.blend.Update({factor[0], factor[1], factor[2], factor[3]}))
                return;
        }
        EncodeState<[](void* target, const FLOAT* factor) {
            static_cast<RecordingTarget*>(target)->list->OMSetBlendFactor(factor);
        }>({ParallelState::BlendFactor}, Snapshot(factor, 4));
    }
    D3D12_FORWARD_INLINE void OMSetDepthBounds(FLOAT minimum, FLOAT maximum) {
        if (!stream) {
            ComPtr<ID3D12GraphicsCommandList1> extended;
            Check(immediate->QueryInterface(IID_PPV_ARGS(&extended)),
                  "D3D12 depth-bounds command interface", device);
            extended->OMSetDepthBounds(minimum, maximum);
            return;
        }
        Check(depth_bounds_status, "D3D12 depth-bounds command interface", device);
        EncodeState<[](void* target, FLOAT minimum, FLOAT maximum) {
            static_cast<RecordingTarget*>(target)->OMSetDepthBounds(minimum, maximum);
        }>({ParallelState::DepthBounds}, minimum, maximum);
    }
    D3D12_FORWARD_INLINE void OMSetFrontAndBackStencilRef(UINT front, UINT back) {
        if (!stream) {
            ComPtr<ID3D12GraphicsCommandList8> extended;
            Check(immediate->QueryInterface(IID_PPV_ARGS(&extended)),
                  "D3D12 independent stencil command interface", device);
            extended->OMSetFrontAndBackStencilRef(front, back);
            return;
        }
        Check(independent_stencil_status, "D3D12 independent stencil command interface", device);
        if (reuse_draw_state) {
            if (!transport_state.stencil.Update({front, back}))
                return;
        }
        EncodeState<[](void* target, UINT front, UINT back) {
            static_cast<RecordingTarget*>(target)->OMSetFrontAndBackStencilRef(front, back);
        }>({ParallelState::StencilReference}, front, back);
    }
    D3D12_FORWARD_INLINE void OMSetRenderTargets(UINT count,
                                                 const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                                                 BOOL contiguous,
                                                 const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
        if (!stream) {
            immediate->OMSetRenderTargets(count, handles, contiguous, depth);
            return;
        }
        if (CaptureParallelState()) {
            const auto snapshot_slot = parallel_descriptors->Acquire(*stream);
            D3D12_CPU_DESCRIPTOR_HANDLE snapshot_handles[8]{};
            for (UINT i = 0; i < count; ++i) {
                snapshot_handles[i] = parallel_descriptors->Rtv(snapshot_slot, i);
                const D3D12_CPU_DESCRIPTOR_HANDLE source{
                    handles[contiguous ? 0 : i].ptr +
                    (contiguous ? i * parallel_descriptors->RtvStride() : 0)};
                parallel_descriptors->Copy(snapshot_handles[i], source, false);
            }
            const auto snapshot_depth = parallel_descriptors->Dsv(snapshot_slot);
            if (depth)
                parallel_descriptors->Copy(snapshot_depth, *depth, true);
            parallel_snapshot
                .Remember<[](void* target, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* depth, DescriptorTicket) {
                    static_cast<RecordingTarget*>(target)->list->OMSetRenderTargets(count, handles,
                                                                                    false, depth);
                }>({ParallelState::RenderTargets}, count,
                   Snapshot(handles ? snapshot_handles : nullptr, count),
                   Snapshot(depth ? &snapshot_depth : nullptr, 1),
                   DescriptorTicket{parallel_descriptors.get(), snapshot_slot});
        }
        const auto slot = descriptors->Acquire(*stream);
        D3D12_CPU_DESCRIPTOR_HANDLE copies[8]{};
        for (UINT i = 0; i < count; ++i) {
            copies[i] = descriptors->Rtv(slot, i);
            const D3D12_CPU_DESCRIPTOR_HANDLE source{
                handles[contiguous ? 0 : i].ptr + (contiguous ? i * descriptors->RtvStride() : 0)};
            descriptors->Copy(copies[i], source, false);
        }
        const auto depth_copy = descriptors->Dsv(slot);
        if (depth)
            descriptors->Copy(depth_copy, *depth, true);
        EncodeCall<[](void* target, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                      BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE* depth, DescriptorTicket) {
            static_cast<RecordingTarget*>(target)->list->OMSetRenderTargets(count, handles,
                                                                            contiguous, depth);
        }>(*stream, count,
           Snapshot(handles ? copies : nullptr, contiguous ? (count ? 1 : 0) : count), contiguous,
           Snapshot(depth ? &depth_copy : nullptr, 1), DescriptorTicket{descriptors, slot});
    }
    D3D12_FORWARD_INLINE void OMSetStencilRef(UINT stencil) {
        if (!stream) {
            immediate->OMSetStencilRef(stencil);
            return;
        }
        if (reuse_draw_state) {
            if (!transport_state.stencil.Update({stencil, stencil}))
                return;
        }
        EncodeState<[](void* target, UINT stencil) {
            static_cast<RecordingTarget*>(target)->list->OMSetStencilRef(stencil);
        }>({ParallelState::StencilReference}, stencil);
    }
    D3D12_FORWARD_INLINE void RSSetScissorRects(UINT count, const D3D12_RECT* rects) {
        if (!stream) {
            immediate->RSSetScissorRects(count, rects);
            return;
        }
        if (reuse_draw_state) {
            if (!rects)
                transport_state.scissors.valid = false;
            else if (!transport_state.scissors.Update(rects, count))
                return;
        }
        EncodeState<[](void* target, UINT count, const D3D12_RECT* rects) {
            static_cast<RecordingTarget*>(target)->list->RSSetScissorRects(count, rects);
        }>({ParallelState::Scissors}, count, Snapshot(rects, count));
    }
    D3D12_FORWARD_INLINE void RSSetViewports(UINT count, const D3D12_VIEWPORT* views) {
        if (!stream) {
            immediate->RSSetViewports(count, views);
            return;
        }
        if (reuse_draw_state) {
            if (!views)
                transport_state.viewports.valid = false;
            else if (!transport_state.viewports.Update(views, count))
                return;
        }
        EncodeState<[](void* target, UINT count, const D3D12_VIEWPORT* views) {
            static_cast<RecordingTarget*>(target)->list->RSSetViewports(count, views);
        }>({ParallelState::Viewports}, count, Snapshot(views, count));
    }
    D3D12_FORWARD_INLINE void ResolveSubresource(ID3D12Resource* destination,
                                                 UINT destination_subresource,
                                                 ID3D12Resource* source, UINT source_subresource,
                                                 DXGI_FORMAT format) {
        if (!stream) {
            immediate->ResolveSubresource(destination, destination_subresource, source,
                                          source_subresource, format);
            return;
        }
        EncodeCall<[](void* target, ID3D12Resource* destination, UINT destination_subresource,
                      ID3D12Resource* source, UINT source_subresource, DXGI_FORMAT format) {
            static_cast<RecordingTarget*>(target)->list->ResolveSubresource(
                destination, destination_subresource, source, source_subresource, format);
        }>(*stream, destination, destination_subresource, source, source_subresource, format);
    }
    D3D12_FORWARD_INLINE void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
        if (!stream) {
            immediate->ResourceBarrier(count, barriers);
            return;
        }
        if (parallel_enabled) {
            for (UINT i = 0; i < count; ++i) {
                // Split barriers cannot cross command lists. Fail closed for this batch.
                if (barriers[i].Flags != 0)
                    parallel_cuts.Disable();
            }
        }
        EncodeCall<[](void* target, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
            static_cast<RecordingTarget*>(target)->list->ResourceBarrier(count, barriers);
        }>(*stream, count, Snapshot(barriers, count));
    }
    D3D12_FORWARD_INLINE void SOSetTargets(UINT start, UINT count,
                                           const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views) {
        if (!stream) {
            immediate->SOSetTargets(start, count, views);
            return;
        }
        if (CaptureParallelState()) {
            for (UINT i = 0; i < count; ++i)
                parallel_snapshot.Remember<[](void* target, UINT slot,
                                              const D3D12_STREAM_OUTPUT_BUFFER_VIEW* view) {
                    static_cast<RecordingTarget*>(target)->list->SOSetTargets(slot, 1, view);
                }>({ParallelState::StreamOutput, start + i}, start + i,
                   Snapshot(views ? views + i : nullptr, 1));
        }
        EncodeCall<[](void* target, UINT start, UINT count,
                      const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views) {
            static_cast<RecordingTarget*>(target)->list->SOSetTargets(start, count, views);
        }>(*stream, start, count, Snapshot(views, count));
    }
    D3D12_FORWARD_INLINE void SetComputeRoot32BitConstants(UINT index, UINT count, const void* data,
                                                           UINT offset) {
        if (!stream) {
            immediate->SetComputeRoot32BitConstants(index, count, data, offset);
            return;
        }
        if (CaptureParallelState()) {
            for (UINT i = 0; i < count; ++i)
                parallel_snapshot.Remember<[](void* target, UINT slot, UINT value, UINT word) {
                    static_cast<RecordingTarget*>(target)->list->SetComputeRoot32BitConstants(
                        slot, 1, &value, word);
                }>({ParallelState::ComputeRootArgument, index, offset + i}, index,
                   static_cast<const UINT*>(data)[i], offset + i);
        }
        EncodeCall<[](void* target, UINT index, UINT count, const void* data, UINT offset) {
            static_cast<RecordingTarget*>(target)->list->SetComputeRoot32BitConstants(index, count,
                                                                                      data, offset);
        }>(*stream, index, count, Snapshot(static_cast<const UINT*>(data), count), offset);
    }
    D3D12_FORWARD_INLINE void SetComputeRootConstantBufferView(UINT index,
                                                               D3D12_GPU_VIRTUAL_ADDRESS address) {
        validation.Root("SetComputeRootConstantBufferView", index, address, true);
        if (!stream) {
            immediate->SetComputeRootConstantBufferView(index, address);
            return;
        }
        EncodeState<[](void* target, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
            static_cast<RecordingTarget*>(target)->list->SetComputeRootConstantBufferView(index,
                                                                                          address);
        }>({ParallelState::ComputeRootArgument, index}, index, address);
    }
    D3D12_FORWARD_INLINE void SetComputeRootShaderResourceView(UINT index,
                                                               D3D12_GPU_VIRTUAL_ADDRESS address) {
        validation.Root("SetComputeRootShaderResourceView", index, address, false);
        if (!stream) {
            immediate->SetComputeRootShaderResourceView(index, address);
            return;
        }
        EncodeState<[](void* target, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
            static_cast<RecordingTarget*>(target)->list->SetComputeRootShaderResourceView(index,
                                                                                          address);
        }>({ParallelState::ComputeRootArgument, index}, index, address);
    }
    D3D12_FORWARD_INLINE void SetComputeRootUnorderedAccessView(UINT index,
                                                                D3D12_GPU_VIRTUAL_ADDRESS address) {
        validation.Root("SetComputeRootUnorderedAccessView", index, address, false);
        if (!stream) {
            immediate->SetComputeRootUnorderedAccessView(index, address);
            return;
        }
        EncodeState<[](void* target, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
            static_cast<RecordingTarget*>(target)->list->SetComputeRootUnorderedAccessView(index,
                                                                                           address);
        }>({ParallelState::ComputeRootArgument, index}, index, address);
    }
    D3D12_FORWARD_INLINE void SetComputeRootDescriptorTable(UINT index,
                                                            D3D12_GPU_DESCRIPTOR_HANDLE handle) {
        validation.Table("SetComputeRootDescriptorTable", index, handle.ptr);
        if (!stream) {
            immediate->SetComputeRootDescriptorTable(index, handle);
            return;
        }
        EncodeState<[](void* target, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
            static_cast<RecordingTarget*>(target)->list->SetComputeRootDescriptorTable(index,
                                                                                       handle);
        }>({ParallelState::ComputeRootTable, index}, index, handle);
    }
    D3D12_FORWARD_INLINE void SetComputeRootSignature(ID3D12RootSignature* signature) {
        if (!stream) {
            immediate->SetComputeRootSignature(signature);
            return;
        }
        if (CaptureParallelState() && parallel_compute_signature != signature) {
            parallel_snapshot.Forget(ParallelState::ComputeRootArgument);
            parallel_snapshot.Forget(ParallelState::ComputeRootTable);
            parallel_compute_signature = signature;
        }
        EncodeState<[](void* target, ID3D12RootSignature* signature) {
            static_cast<RecordingTarget*>(target)->list->SetComputeRootSignature(signature);
        }>({ParallelState::ComputeRootSignature}, signature);
    }
    D3D12_FORWARD_INLINE void SetGraphicsRoot32BitConstants(UINT index, UINT count,
                                                            const void* data, UINT offset) {
        if (reuse_graphics_root_cbvs)
            graphics_root_cbvs.Invalidate(index);
        if (!stream) {
            immediate->SetGraphicsRoot32BitConstants(index, count, data, offset);
            return;
        }
        if (CaptureParallelState()) {
            for (UINT i = 0; i < count; ++i)
                parallel_snapshot.Remember<[](void* target, UINT slot, UINT value, UINT word) {
                    static_cast<RecordingTarget*>(target)->list->SetGraphicsRoot32BitConstants(
                        slot, 1, &value, word);
                }>({ParallelState::GraphicsRootArgument, index, offset + i}, index,
                   static_cast<const UINT*>(data)[i], offset + i);
        }
        EncodeCall<[](void* target, UINT index, UINT count, const void* data, UINT offset) {
            static_cast<RecordingTarget*>(target)->list->SetGraphicsRoot32BitConstants(
                index, count, data, offset);
        }>(*stream, index, count, Snapshot(static_cast<const UINT*>(data), count), offset);
    }
    D3D12_FORWARD_INLINE void SetGraphicsRootConstantBufferView(UINT index,
                                                                D3D12_GPU_VIRTUAL_ADDRESS address) {
        validation.Root("SetGraphicsRootConstantBufferView", index, address, true);
        if (reuse_graphics_root_cbvs)
            graphics_root_cbvs.cbvs.Update(index, address);
        if (!stream) {
            immediate->SetGraphicsRootConstantBufferView(index, address);
            return;
        }
        EncodeState<[](void* target, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
            static_cast<RecordingTarget*>(target)->list->SetGraphicsRootConstantBufferView(index,
                                                                                           address);
        }>({ParallelState::GraphicsRootArgument, index}, index, address);
    }
    D3D12_FORWARD_INLINE void SetGraphicsRootShaderResourceView(UINT index,
                                                                D3D12_GPU_VIRTUAL_ADDRESS address) {
        validation.Root("SetGraphicsRootShaderResourceView", index, address, false);
        if (reuse_graphics_root_cbvs)
            graphics_root_cbvs.Invalidate(index);
        if (!stream) {
            immediate->SetGraphicsRootShaderResourceView(index, address);
            return;
        }
        EncodeState<[](void* target, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
            static_cast<RecordingTarget*>(target)->list->SetGraphicsRootShaderResourceView(index,
                                                                                           address);
        }>({ParallelState::GraphicsRootArgument, index}, index, address);
    }
    D3D12_FORWARD_INLINE void SetGraphicsRootUnorderedAccessView(
        UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
        validation.Root("SetGraphicsRootUnorderedAccessView", index, address, false);
        if (reuse_graphics_root_cbvs)
            graphics_root_cbvs.Invalidate(index);
        if (!stream) {
            immediate->SetGraphicsRootUnorderedAccessView(index, address);
            return;
        }
        EncodeState<[](void* target, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
            static_cast<RecordingTarget*>(target)->list->SetGraphicsRootUnorderedAccessView(
                index, address);
        }>({ParallelState::GraphicsRootArgument, index}, index, address);
    }
    D3D12_FORWARD_INLINE void SetGraphicsRootDescriptorTable(UINT index,
                                                             D3D12_GPU_DESCRIPTOR_HANDLE handle) {
        validation.Table("SetGraphicsRootDescriptorTable", index, handle.ptr);
        if (reuse_graphics_root_cbvs)
            graphics_root_cbvs.Invalidate(index);
        if (!stream) {
            immediate->SetGraphicsRootDescriptorTable(index, handle);
            return;
        }
        EncodeState<[](void* target, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
            static_cast<RecordingTarget*>(target)->list->SetGraphicsRootDescriptorTable(index,
                                                                                        handle);
        }>({ParallelState::GraphicsRootTable, index}, index, handle);
    }
    D3D12_FORWARD_INLINE void SetGraphicsRootSignature(ID3D12RootSignature* signature) {
        if (reuse_graphics_root_cbvs)
            graphics_root_cbvs.SetSignature(reinterpret_cast<std::uintptr_t>(signature));
        if (!stream) {
            immediate->SetGraphicsRootSignature(signature);
            return;
        }
        if (CaptureParallelState() && parallel_graphics_signature != signature) {
            parallel_snapshot.Forget(ParallelState::GraphicsRootArgument);
            parallel_snapshot.Forget(ParallelState::GraphicsRootTable);
            parallel_graphics_signature = signature;
        }
        EncodeState<[](void* target, ID3D12RootSignature* signature) {
            static_cast<RecordingTarget*>(target)->list->SetGraphicsRootSignature(signature);
        }>({ParallelState::GraphicsRootSignature}, signature);
    }
    D3D12_FORWARD_INLINE void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps) {
        validation.Heaps(device, count, heaps);
        if (!stream) {
            immediate->SetDescriptorHeaps(count, heaps);
            return;
        }
        if (CaptureParallelState() &&
            (parallel_heaps.size() != count ||
             !std::is_permutation(parallel_heaps.begin(), parallel_heaps.end(), heaps))) {
            // A changed heap can invalidate only some root tables. Until the encoder
            // models the heap type of each table, do not cut after a heap switch.
            if (!parallel_heaps.empty())
                parallel_cuts.Disable();
            parallel_snapshot.Forget(ParallelState::GraphicsRootTable);
            parallel_snapshot.Forget(ParallelState::ComputeRootTable);
            parallel_heaps.clear();
            for (UINT i = 0; i < count; ++i)
                parallel_heaps.push_back(heaps[i]);
        }
        EncodeState<[](void* target, UINT count, ID3D12DescriptorHeap* const* heaps) {
            static_cast<RecordingTarget*>(target)->list->SetDescriptorHeaps(count, heaps);
        }>({ParallelState::DescriptorHeaps}, count, Snapshot(heaps, count));
    }
    D3D12_FORWARD_INLINE void SetMarker(UINT metadata, const void* data, UINT size) {
        if (!stream) {
            immediate->SetMarker(metadata, data, size);
            return;
        }
        EncodeCall<[](void* target, UINT metadata, const void* data, UINT size) {
            static_cast<RecordingTarget*>(target)->list->SetMarker(metadata, data, size);
        }>(*stream, metadata, Snapshot(static_cast<const UINT8*>(data), size), size);
    }
    D3D12_FORWARD_INLINE void SetPipelineState(ID3D12PipelineState* state) {
        if (!stream) {
            immediate->SetPipelineState(state);
            return;
        }
        EncodeState<[](void* target, ID3D12PipelineState* state) {
            static_cast<RecordingTarget*>(target)->list->SetPipelineState(state);
        }>({ParallelState::Pipeline}, state);
    }
    D3D12_FORWARD_INLINE void SetPredication(ID3D12Resource* buffer, UINT64 offset,
                                             D3D12_PREDICATION_OP operation) {
        if (!stream) {
            immediate->SetPredication(buffer, offset, operation);
            return;
        }
        EncodeState<[](void* target, ID3D12Resource* buffer, UINT64 offset,
                       D3D12_PREDICATION_OP operation) {
            static_cast<RecordingTarget*>(target)->list->SetPredication(buffer, offset, operation);
        }>({ParallelState::Predication}, buffer, offset, operation);
    }
};
#undef D3D12_FORWARD_INLINE
} // namespace D3D12
