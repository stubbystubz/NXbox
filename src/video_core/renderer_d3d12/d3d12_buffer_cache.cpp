// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include "common/nxbox_upload_diag.h"
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <fmt/format.h>
#include "common/alignment.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_footprint.h"

namespace D3D12 {
namespace {
bool UniformStreamEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_D3D12_UNIFORM_STREAM");
        return !value || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}
bool UploadTraceEnabled() {
    static const bool enabled = BisectEnabled("NXBOX_D3D12_UPLOAD_TRACE");
    return enabled;
}
u64 TraceHash(std::span<const u8> bytes) {
    u64 hash = 14695981039346656037ULL;
    for (const u8 byte : bytes)
        hash = (hash ^ byte) * 1099511628211ULL;
    return hash;
}
size_t TraceDifference(std::span<const u8> expected, std::span<const u8> actual) {
    return std::mismatch(expected.begin(), expected.end(), actual.begin()).first - expected.begin();
}
std::string TraceHex(std::span<const u8> bytes, size_t offset) {
    std::string hex;
    for (const u8 byte : bytes.subspan(offset, std::min(size_t{32}, bytes.size() - offset)))
        hex += fmt::format("{:02x}", byte);
    return hex;
}
void InvalidRange(u32 kind, const char* label, u64 offset, u64 size, u64 width) {
    static std::array<std::atomic_flag, 8> reported{};
    if (!reported[kind].test_and_set())
        Diagnostic(fmt::format(
            "D3D12 {} rejected offset={} size={} resource_bytes={}; substituting zero/skip", label,
            offset, size, width));
}
} // namespace

Buffer::Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams params) : BufferBase{params} {}
Buffer::Buffer(BufferCacheRuntime& runtime, VAddr address, u64 size, bool)
    : BufferBase{address, size}, context{&runtime.context},
      resource{context->CreateBuffer(size, D3D12_HEAP_TYPE_DEFAULT)}, usage{size} {
    if (UploadTraceEnabled())
        Diagnostic(fmt::format(
            "D3D12_UPLOAD_CREATE resource={:#x} guest_base={:#x} size={} tick={}",
            resource->handle->GetGPUVirtualAddress(), address, size, context->CurrentTick()));
}
void Buffer::MarkUsage(u64 offset, u64 size) {
    if (!context || !resource)
        return;
    // Retention remains unconditional: Retain can open a new command list. Usage
    // bits only need each exact range once until tracking resets.
    if (usage_reuse
            ? usage_ranges.Insert(offset, size)
            : (!binding_cache || !usage_cached || offset != usage_offset || size != usage_size)) {
        usage.Track(offset, size);
        usage_offset = offset;
        usage_size = size;
        usage_cached = true;
    }
    tick = context->CurrentTick();
    context->Retain(resource);
}
void BufferCacheRuntime::RecordBufferUpload(VideoCommon::BufferUploadSource source, u64 bytes) {
    FrameCount counter = FrameCount::OtherUploadBytes;
    switch (source) {
    case VideoCommon::BufferUploadSource::Other:
        break;
    case VideoCommon::BufferUploadSource::Vertex:
        counter = FrameCount::VertexUploadBytes;
        break;
    case VideoCommon::BufferUploadSource::Index:
        counter = FrameCount::IndexUploadBytes;
        break;
    case VideoCommon::BufferUploadSource::Uniform:
        counter = FrameCount::UniformUploadBytes;
        break;
    case VideoCommon::BufferUploadSource::Storage:
        counter = FrameCount::StorageUploadBytes;
        break;
    case VideoCommon::BufferUploadSource::Texture:
        counter = FrameCount::TexelUploadBytes;
        break;
    }
    upload_source = synchronizing_indices ? FrameCount::IndexUploadBytes : counter;
    (void)bytes; // The staging allocation records the actual transferred bytes.
}
namespace {
// Matches VideoCommon::WatchWindowOffset: NXBOX_WATCH_WINDOW (hex), 216 bytes from 0x958.
u32 WitnessWindowNonzero(std::span<const u8> page) {
    static const u32 offset = [] {
        const char* value = std::getenv("NXBOX_WATCH_WINDOW");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 16)) & 0xfffu : 0x958u;
    }();
    const u32 end = std::min<u32>(offset + 216, static_cast<u32>(page.size()));
    return static_cast<u32>(
        std::count_if(page.begin() + offset, page.begin() + end, [](u8 b) { return b != 0; }));
}
} // namespace
void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& buffers) {
    // Drop source leases and snapshot results at the frame boundary. The ring's
    // recycle callback also clears them before a snapshot page is reused.
    InvalidateUniformBindings();
    storage_bindings = {};
    bool trim_staging = false;
#ifdef NXBOX_UWP
    const auto usage = context.MemoryUsage();
    trim_staging = usage >= NativeTextureMemoryBudget(usage).critical;
#endif
    uniform_stream.Clear();
    upload.Collect(trim_staging);
    readback.Collect(trim_staging);
    if (witness_resource) {
        // Diagnostic: re-read the watched upload destination each frame, log content changes.
        auto probe = context.CreateBuffer(4096, D3D12_HEAP_TYPE_READBACK);
        const u64 size = std::min<u64>(4096, witness_resource->handle->GetDesc().Width -
                                                witness_offset);
        context.Transition(witness_resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
        context.Transition(probe, D3D12_RESOURCE_STATE_COPY_DEST);
        context.BeforeCopyWrite(probe, 0, size);
        context.Commands()->CopyBufferRegion(probe->handle.Get(), 0,
                                             witness_resource->handle.Get(), witness_offset, size);
        context.AfterCompletion([this, probe, size, frame = ++witness_frame] {
            const u64 hash = TraceHash(probe->mapped.first(static_cast<size_t>(size)));
            const auto nonzero =
                std::count_if(probe->mapped.begin(), probe->mapped.begin() + size,
                              [](u8 b) { return b != 0; });
            if (hash != witness_hash)
                Diagnostic(fmt::format(
                    "D3D12_WITNESS frame={} hash={:#x} nonzero={} window_nonzero={} bytes={}",
                    frame, hash, nonzero,
                    size >= 0x1000 ? WitnessWindowNonzero(probe->mapped.first(0x1000)) : 0u,
                    TraceHex(probe->mapped, 0)));
            witness_hash = hash;
        });
    }
    for (const auto [id, buffer] : buffers)
        buffer->ResetUsageTracking();
}
void BufferCacheRuntime::CopyBuffer(ResourcePtr dst, ResourcePtr src,
                                    std::span<const VideoCommon::BufferCopy> copies, bool, bool) {
    const FrameProfile::Scope profile_copy{
        context.Profile(), src && src->heap == D3D12_HEAP_TYPE_UPLOAD ? FramePhase::BufferUpload
                           : dst && dst->heap == D3D12_HEAP_TYPE_READBACK
                               ? FramePhase::BufferDownload
                               : FramePhase::BufferCache};
    if (!dst || !src) {
        InvalidRange(7, "null buffer copy", 0, 0, 0);
        return;
    }
    const auto src_size = src->handle->GetDesc().Width;
    const auto dst_size = dst->handle->GetDesc().Width;
    for (const auto& copy : copies) {
        if (copy.src_offset > src_size || copy.size > src_size - copy.src_offset) {
            InvalidRange(0, "buffer copy source", copy.src_offset, copy.size, src_size);
            return;
        }
        if (copy.dst_offset > dst_size || copy.size > dst_size - copy.dst_offset) {
            InvalidRange(1, "buffer copy destination", copy.dst_offset, copy.size, dst_size);
            return;
        }
    }
    if (dst == src) {
        // Snapshot every source range before changing any destination range.
        size_t scratch_size{};
        for (const auto& copy : copies)
            scratch_size = CheckedAdd(scratch_size, copy.size);
        if (!scratch_size)
            return;
        auto scratch = context.CreateBuffer(scratch_size, D3D12_HEAP_TYPE_DEFAULT);
        std::vector<VideoCommon::BufferCopy> reads, writes;
        u64 cursor{};
        for (const auto& copy : copies) {
            reads.push_back({copy.src_offset, cursor, copy.size});
            writes.push_back({cursor, copy.dst_offset, copy.size});
            cursor += copy.size;
        }
        CopyBuffer(scratch, src, reads, false);
        CopyBuffer(dst, scratch, writes, false);
        return;
    }
    // Witness the GPU's source bytes and destination immediately around each cache copy batch.
    // Downloads (including these witnesses) do not recurse. Snapshot the CPU bytes before
    // allocation: CreateBuffer can submit/wait under pressure. Keep all probes in this list.
    const bool watched = NxboxUpload::trace_next_copy.exchange(false);
    const bool trace = (UploadTraceEnabled() || watched) && dst->heap == D3D12_HEAP_TYPE_DEFAULT;
    if (watched && copies.size() == 1) {
        witness_resource = dst;
        witness_offset = copies[0].dst_offset;
        witness_hash = 0;
    }
    ResourcePtr trace_readback;
    std::vector<u8> trace_expected;
    std::vector<VideoCommon::BufferCopy> trace_copies, trace_reads, trace_writes;
    size_t trace_size{};
    u64 trace_id{};
    if (trace) {
        static u64 sequence{};
        trace_id = ++sequence;
        trace_copies.assign(copies.begin(), copies.end());
        for (const auto& copy : copies)
            trace_size = CheckedAdd(trace_size, copy.size);
        if (src->heap == D3D12_HEAP_TYPE_UPLOAD) {
            trace_expected.resize(trace_size);
            size_t cursor{};
            for (const auto& copy : copies) {
                std::memcpy(trace_expected.data() + cursor, src->mapped.data() + copy.src_offset,
                            copy.size);
                cursor += copy.size;
            }
        }
        if (trace_size) {
            trace_readback =
                context.CreateBuffer(CheckedMultiply(trace_size, 2), D3D12_HEAP_TYPE_READBACK);
            size_t cursor{};
            for (const auto& copy : copies) {
                trace_reads.push_back({copy.src_offset, cursor, copy.size});
                trace_writes.push_back({copy.dst_offset, trace_size + cursor, copy.size});
                cursor += copy.size;
            }
        }
    }
    {
        const FrameProfile::Scope profile_transitions{context.Profile(),
                                                      FramePhase::UploadTransitions};
        context.Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        context.Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (trace_readback)
        CopyBuffer(trace_readback, src, trace_reads, true);
    const u64 trace_tick = trace ? context.CurrentTick() : 0;
    {
        const FrameProfile::Scope profile_recording{context.Profile(), FramePhase::UploadRecording};
        for (const auto& copy : copies) {
            if (trace)
                Diagnostic(fmt::format(
                    "D3D12_UPLOAD_COPY id={} tick={} source={:#x} source_heap={} src_offset={} "
                    "destination={:#x} dst_offset={} size={}",
                    trace_id, trace_tick, src->handle->GetGPUVirtualAddress(),
                    static_cast<u32>(src->heap), copy.src_offset,
                    dst->handle->GetGPUVirtualAddress(), copy.dst_offset, copy.size));
            context.Profile().Count(src->heap == D3D12_HEAP_TYPE_UPLOAD
                                        ? FrameCount::StagingCopyBytes
                                        : FrameCount::GpuBufferCopyBytes,
                                    copy.size);
            context.MarkWork("upload/copy");
            // Diagnostic: order every buffer copy after all prior GPU work.
            static const bool copy_barrier = BisectEnabled("NXBOX_D3D12_COPY_BARRIER");
            if (copy_barrier) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                context.Commands()->ResourceBarrier(1, &barrier);
                context.Profile().Count(FrameCount::Barriers);
            }
            if (src->heap == D3D12_HEAP_TYPE_UPLOAD)
                context.Profile().Count(FrameCount::UploadCopyCommands);
            const u64 hazards_before = context.copy_hazard_count;
            context.BeforeCopyWrite(dst, copy.dst_offset, copy.size);
            context.Commands()->CopyBufferRegion(dst->handle.Get(), copy.dst_offset,
                                                 src->handle.Get(), copy.src_offset, copy.size);
            if (trace)
                Diagnostic(fmt::format("D3D12_UPLOAD_HAZARD id={} barrier={}", trace_id,
                                       context.copy_hazard_count - hazards_before));
        }
    }
    if (trace_readback) {
        CopyBuffer(trace_readback, dst, trace_writes, true);
        context.Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
        context.AfterCompletion([trace_readback, src, trace_id, trace_tick, trace_size,
                                 destination = dst->handle->GetGPUVirtualAddress(),
                                 copies = std::move(trace_copies),
                                 expected = std::move(trace_expected)] {
            size_t cursor{};
            for (const auto& copy : copies) {
                const auto gpu_source = trace_readback->mapped.subspan(cursor, copy.size);
                const auto gpu_destination =
                    trace_readback->mapped.subspan(trace_size + cursor, copy.size);
                const bool upload = src->heap == D3D12_HEAP_TYPE_UPLOAD;
                const std::span<const u8> cpu =
                    upload ? std::span<const u8>{expected}.subspan(cursor, copy.size) : gpu_source;
                const size_t source_diff = TraceDifference(cpu, gpu_source);
                const size_t destination_diff = TraceDifference(gpu_source, gpu_destination);
                const size_t staging_diff =
                    upload ? TraceDifference(cpu, src->mapped.subspan(copy.src_offset, copy.size))
                           : copy.size;
                const size_t first_diff = std::min(source_diff, destination_diff);
                const size_t window = first_diff == copy.size ? 0 : first_diff;
                Diagnostic(fmt::format(
                    "D3D12_UPLOAD_RESULT id={} tick={} destination={:#x} dst_offset={} size={} "
                    "upload={} cpu_hash={:#x} gpu_source_hash={:#x} gpu_destination_hash={:#x} "
                    "source_diff={} destination_diff={} staging_diff={} window={} "
                    "cpu={} gpu_source={} gpu_destination={}",
                    trace_id, trace_tick, destination, copy.dst_offset, copy.size, upload,
                    TraceHash(cpu), TraceHash(gpu_source), TraceHash(gpu_destination), source_diff,
                    destination_diff, staging_diff, window, TraceHex(cpu, window),
                    TraceHex(gpu_source, window), TraceHex(gpu_destination, window)));
                // Keep the measured fullscreen-stream offsets visible even when every
                // byte matches: a hash or a prefix cannot show an earlier zero snapshot.
                for (const u64 probe : {u64{72}, u64{26968}}) {
                    if (probe < copy.dst_offset || probe - copy.dst_offset >= copy.size)
                        continue;
                    const size_t relative = probe - copy.dst_offset;
                    Diagnostic(fmt::format("D3D12_UPLOAD_PROBE id={} destination={:#x} offset={} "
                                           "cpu={} gpu_source={} gpu_destination={}",
                                           trace_id, destination, probe, TraceHex(cpu, relative),
                                           TraceHex(gpu_source, relative),
                                           TraceHex(gpu_destination, relative)));
                }
                cursor += copy.size;
            }
        });
    }
}
void BufferCacheRuntime::ClearBuffer(ResourcePtr dst, u32 offset, size_t size, u32 value) {
    // Copy-based fill also handles buffers without UAV flags. Chunk large clears.
    const auto width = dst ? dst->handle->GetDesc().Width : 0;
    if (!dst || ((size | offset) & 3) || offset > width || size > width - offset) {
        InvalidRange(2, "unaligned or out-of-bounds buffer clear", offset, size, width);
        return;
    }
    if (UploadTraceEnabled())
        Diagnostic(fmt::format(
            "D3D12_UPLOAD_CLEAR resource={:#x} offset={} size={} value={} tick={}",
            dst->handle->GetGPUVirtualAddress(), offset, size, value, context.CurrentTick()));
    while (size) {
        const size_t count = std::min(size, size_t{1 << 20});
        const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
        context.Profile().Count(FrameCount::BufferUploadBytes, count);
        context.Profile().Count(FrameCount::OtherUploadBytes, count);
        auto staging = upload.Request(count);
        for (size_t i = 0; i < count; i += sizeof(value))
            std::memcpy(staging.mapped_span.data() + i, &value, sizeof(value));
        const VideoCommon::BufferCopy copy{staging.offset, offset, count};
        CopyBuffer(dst, staging.buffer, std::span{&copy, 1}, true);
        offset += static_cast<u32>(count);
        size -= count;
        // Request() already retires pages and finishes at the ring's bounded capacity.
        // Finishing every MiB serializes even clears that fit in a single upload page.
        if (size && BisectEnabled("NXBOX_D3D12_SYNC_BUFFER_CLEARS"))
            context.Finish(FrameCount::ForcedSubmitBufferClear);
    }
}
using Maxwell = Tegra::Engines::Maxwell3D::Regs;
bool BufferCacheRuntime::TryReuseIndexBinding(Maxwell::PrimitiveTopology topology,
                                              Maxwell::IndexFormat format, const Buffer& buffer,
                                              u32 offset, u32 size) {
    const auto& previous = bindings.geometry;
    const bool unsupported = format == Maxwell::IndexFormat::UnsignedByte ||
                             topology == Maxwell::PrimitiveTopology::Quads ||
                             topology == Maxwell::PrimitiveTopology::QuadStrip;
    return BindingResolutionCacheEnabled() && previous.index == buffer.Handle() &&
           previous.index_offset == offset && previous.index_size == size &&
           previous.index_is_32bit == (format == Maxwell::IndexFormat::UnsignedInt) &&
           previous.index_unsupported == unsupported;
}

bool BufferCacheRuntime::TryReuseVertexBinding(u32 index, const Buffer& buffer, u32 offset,
                                               u32 size, u32 stride) {
    if (!BindingResolutionCacheEnabled() || UploadTraceEnabled() ||
        index >= GeometryBindings::MaxVertexBuffers) {
        return false;
    }
    const auto& previous = bindings.geometry.vertex[index];
    return previous.resource == buffer.Handle() && previous.offset == offset &&
           previous.size == size && previous.stride == stride;
}

void BufferCacheRuntime::BindIndexBuffer(Maxwell::PrimitiveTopology topology,
                                         Maxwell::IndexFormat format, u32, u32, ResourcePtr buffer,
                                         u32 offset, u32 size) {
    auto& geometry = bindings.geometry;
    geometry.index = std::move(buffer);
    geometry.index_offset = offset;
    geometry.index_size = size;
    geometry.index_is_32bit = format == Maxwell::IndexFormat::UnsignedInt;
    // 8-bit indices and indexed quads need a conversion pass that is not implemented yet.
    geometry.index_unsupported = format == Maxwell::IndexFormat::UnsignedByte ||
                                 topology == Maxwell::PrimitiveTopology::Quads ||
                                 topology == Maxwell::PrimitiveTopology::QuadStrip;
}
void BufferCacheRuntime::BindQuadIndexBuffer(Maxwell::PrimitiveTopology topology, u32 first,
                                             u32 count) {
    // Quads are drawn as indexed triangle lists; indices already include the first vertex.
    static constexpr u32 quad_list[6] = {0, 1, 2, 0, 2, 3};
    static constexpr u32 strip_list[6] = {0, 1, 2, 2, 1, 3};
    const bool strip = topology == Maxwell::PrimitiveTopology::QuadStrip;
    const u32 quads = strip ? (count < 4 ? 0 : (count - 2) / 2) : count / 4;
    auto& geometry = bindings.geometry;
    geometry.index_unsupported = false;
    geometry.index_is_32bit = true;
    if (!quads) {
        geometry.index = nullptr;
        geometry.index_size = 0;
        return;
    }
    const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
    context.Profile().Count(FrameCount::BufferUploadBytes, size_t{quads} * 6 * sizeof(u32));
    context.Profile().Count(FrameCount::IndexUploadBytes, size_t{quads} * 6 * sizeof(u32));
    auto staging = upload.Request(size_t{quads} * 6 * sizeof(u32));
    auto* out = reinterpret_cast<u32*>(staging.mapped_span.data());
    for (u32 quad = 0; quad < quads; ++quad) {
        const u32 base = first + quad * (strip ? 2 : 4);
        for (u32 i = 0; i < 6; ++i)
            *out++ = base + (strip ? strip_list[i] : quad_list[i]);
    }
    geometry.index = staging.buffer;
    geometry.index_offset = staging.offset;
    geometry.index_size = staging.mapped_span.size();
}
void BufferCacheRuntime::BindVertexBuffer(u32 index, ResourcePtr buffer, u32 offset, u32 size,
                                          u32 stride) {
    if (index >= GeometryBindings::MaxVertexBuffers)
        return;
    if (BindingCacheEnabled()) {
        const auto& previous = bindings.geometry.vertex[index];
        if (previous.resource == buffer && previous.offset == offset && previous.size == size &&
            previous.stride == stride && !UploadTraceEnabled())
            return;
    }
    const auto width = buffer ? (BufferFastpathEnabled() ? buffer->Description().Width
                                                         : buffer->handle->GetDesc().Width)
                              : 0;
    if (offset > width || size > width - offset) {
        InvalidRange(3, "vertex binding", offset, size, width);
        bindings.geometry.vertex[index] = {};
        return;
    }
    // Size is the inclusive Maxwell limit minus start supplied by the shared cache.
    // Preserve stride zero (constant attributes) and the synchronized guest extent.
    if (UploadTraceEnabled())
        Diagnostic(fmt::format(
            "D3D12_UPLOAD_BIND slot={} resource={:#x} offset={} size={} stride={} tick={}", index,
            buffer ? buffer->handle->GetGPUVirtualAddress() : 0, offset, size, stride,
            context.CurrentTick()));
    bindings.geometry.vertex[index] = {std::move(buffer), offset, size, stride};
}
void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& host) {
    for (size_t i = 0; i < host.buffers.size(); ++i)
        BindVertexBuffer(host.min_index + static_cast<u32>(i), host.buffers[i]->Handle(),
                         static_cast<u32>(host.offsets[i]), static_cast<u32>(host.sizes[i]),
                         static_cast<u32>(host.strides[i]));
}
std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer(size_t stage, u32, u32 size) {
    const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
    const u32 padded = UniformViewSize(size);
    const u64 allocation_bytes = size > 65536 ? size : padded;
    context.Profile().Count(FrameCount::BufferUploadBytes, allocation_bytes);
    context.Profile().Count(FrameCount::UniformUploadBytes, allocation_bytes);
    // Own only the alignment tail; reads outside the declared CBV return zero.
    if (size > 65536) {
        InvalidRange(4, "mapped uniform exceeds Maxwell 64 KiB limit", 0, size, 65536);
        // Keep the caller's memcpy in bounds while exposing only legal bytes to D3D12.
        auto rejected = upload.Request(size);
        std::fill(rejected.mapped_span.begin(), rejected.mapped_span.end(), u8{0});
        bindings.stages[stage].uniform.push_back({nullptr, 0, 0});
        return rejected.mapped_span;
    }
    if (!size) {
        bindings.stages[stage].uniform.push_back({nullptr, 0, 0});
        return {};
    }
    auto staging = UniformStreamEnabled()
                       ? uniform_stream.Request(
                             padded, [this](size_t bytes) { return upload.Request(bytes); },
                             [this] { return context.CurrentTick(); })
                       : upload.Request(padded);
    std::fill(staging.mapped_span.begin() + size, staging.mapped_span.end(), u8{0});
    bindings.stages[stage].uniform.push_back(
        {staging.buffer, static_cast<u32>(staging.offset), padded});
    return staging.mapped_span.first(size);
}
bool BufferCacheRuntime::TryReuseUniformBinding(const Buffer& buffer, u32 offset, u32 size,
                                                u32 read_limit) {
    auto& destination = bindings.stages[bindings.current_stage].uniform;
    if ((!BindingResolutionCacheEnabled() && !BindingCacheEnabled()) ||
        destination.size() >= uniform_bindings[0].size()) {
        return false;
    }
    const auto& resolution = uniform_bindings[bindings.current_stage][destination.size()];
    const auto& resource = buffer.Handle();
    // Snapshot reuse remains experimental; direct views do not depend on allocator lifetime.
    if (resource && resolution.source == resource &&
        (resolution.resolved.resource == resource || BindingCacheEnabled()) &&
        resolution.version == resource->content_version && resolution.offset == offset &&
        resolution.size == size && resolution.read_limit == read_limit) {
        destination.push_back(resolution.resolved);
        return true;
    }
    return false;
}

void BufferCacheRuntime::BindUniformBuffer(const Buffer& buffer, u32 offset, u32 size) {
    const u32 read_limit = std::exchange(uniform_read_limit, ~0U);
    auto resource = buffer.Handle();
    auto& destination = bindings.stages[bindings.current_stage].uniform;
    UniformBindingResolution* resolution = nullptr;
    if ((BindingResolutionCacheEnabled() || BindingCacheEnabled()) &&
        destination.size() < uniform_bindings[0].size()) {
        resolution = &uniform_bindings[bindings.current_stage][destination.size()];
        if (TryReuseUniformBinding(buffer, offset, size, read_limit)) {
            return;
        }
    }
    const auto remember = [&] {
        if (resolution) {
            const auto source = buffer.Handle();
            *resolution = {source,     source->content_version, offset, size,
                           read_limit, destination.back()};
        }
    };
    const auto width = resource ? (BufferFastpathEnabled() ? resource->Description().Width
                                                           : resource->handle->GetDesc().Width)
                                : 0;
    if (!resource || !size) {
        bindings.stages[bindings.current_stage].uniform.push_back({nullptr, 0, 0});
        return;
    }
    if (size > 65536 || offset > width || size > width - offset) {
        InvalidRange(5, "uniform binding exceeds synchronized buffer range or 64 KiB", offset, size,
                     width);
        bindings.stages[bindings.current_stage].uniform.push_back({nullptr, 0, 0});
        return;
    }
    // A copied CBV owns its padding; rounding a direct view must never expose unrelated bytes
    // the shader can read. When the shader's own read size bounds the binding, the rounded
    // view's tail is unobservable and the direct view needs no zero-padded copy (Kirby made
    // ~600 such copies a frame).
    const auto padded = UniformViewSize(size);
    const bool unread_tail = read_limit <= size;
    if ((offset & 255) || (size != padded && !unread_tail) || padded > width - offset) {
        bindings.stages[bindings.current_stage].uniform.push_back(
            CopyUniform({std::move(resource), offset, size}));
        remember();
        return;
    }
    bindings.stages[bindings.current_stage].uniform.push_back(
        {std::move(resource), offset, padded});
    remember();
}
BoundBuffer BufferCacheRuntime::CopyUniform(const BoundBuffer& uniform) {
    const auto& resource = uniform.resource;
    const u32 offset = uniform.offset, size = uniform.size;
    const auto padded = UniformViewSize(size);
    // A version changes on every write-state use, including same-state UAV/copy writes.
    ResourcePtr copy;
    u32 copy_offset{};
    {
        const FrameProfile::Scope profile_lookup{context.Profile(), FramePhase::UniformCopyLookup};
        copy = uniform_copies.Find(resource, resource->content_version, offset, size, &copy_offset);
    }
    if (copy) {
        context.Profile().Count(FrameCount::UniformCopyCacheHits);
        return {std::move(copy), copy_offset, padded};
    }
    context.Profile().Count(FrameCount::UniformCopyCacheMisses);
    const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
    context.Profile().Count(FrameCount::BufferUploadBytes, padded);
    context.Profile().Count(FrameCount::UniformUploadBytes, padded);
    // GPU-produced sources need a DEFAULT snapshot, not a CPU upload. Share large
    // pages; cache entries and command lists lease the page until it is safe to reset.
    auto allocation = uniform_snapshots.Request(padded);
    auto target = allocation.buffer;
    const auto target_offset = static_cast<u32>(allocation.offset);
    const VideoCommon::BufferCopy data{offset, target_offset, size};
    CopyBuffer(target, resource, std::span{&data, 1}, false);
    if (padded > size) {
        auto staging = upload.Request(padded - size);
        std::fill(staging.mapped_span.begin(), staging.mapped_span.end(), u8{0});
        const VideoCommon::BufferCopy zero{staging.offset, target_offset + size, padded - size};
        CopyBuffer(target, staging.buffer, std::span{&zero, 1}, true);
    }
    // Bounded cache; command-list resource leases keep evicted copies alive for the GPU.
    uniform_copies.Insert(resource, resource->content_version, offset, size, target, target_offset);
    return {std::move(target), target_offset, padded};
}
bool BufferCacheRuntime::TryReuseStorageBinding(const Buffer& buffer, u32 offset, u32 size,
                                                bool is_written) {
    auto& destination = bindings.stages[bindings.current_stage].storage;
    if (!BindingResolutionCacheEnabled() || destination.size() >= storage_bindings[0].size()) {
        return false;
    }
    const auto& previous = storage_bindings[bindings.current_stage][destination.size()];
    // Invalid/zero views take the validation path; content and write tracking are done by
    // the shared cache even on a hit. D3D12 resource extents are immutable.
    if (previous.resource && previous.resource == buffer.Handle() && previous.offset == offset &&
        previous.size == size && previous.written == is_written) {
        destination.push_back(previous);
        return true;
    }
    return false;
}

void BufferCacheRuntime::BindStorageBuffer(const Buffer& buffer, u32 offset, u32 size,
                                           bool is_written) {
    auto resource = buffer.Handle();
    const auto width = resource ? (BufferFastpathEnabled() ? resource->Description().Width
                                                           : resource->handle->GetDesc().Width)
                                : 0;
    if (offset > width || size > width - offset || (offset & 3)) {
        InvalidRange(6, "raw storage binding requires an in-bounds word-aligned range", offset,
                     size, width);
        bindings.stages[bindings.current_stage].storage.push_back({nullptr, 0, 0, is_written});
        return;
    }
    auto& destination = bindings.stages[bindings.current_stage].storage;
    if (BindingResolutionCacheEnabled() && destination.size() < storage_bindings[0].size()) {
        storage_bindings[bindings.current_stage][destination.size()] = {resource, offset, size,
                                                                        is_written};
    }
    bindings.stages[bindings.current_stage].storage.push_back(
        {std::move(resource), offset, size, is_written});
}
void BufferCacheRuntime::BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                                           VideoCore::Surface::PixelFormat format) {
    bindings.stages[bindings.current_stage].texel.push_back(
        {buffer.Handle(), offset, size, false, static_cast<u32>(format)});
}
void BufferCacheRuntime::BindImageBuffer(Buffer& buffer, u32 offset, u32 size,
                                         VideoCore::Surface::PixelFormat format) {
    BindTextureBuffer(buffer, offset, size, format);
    bindings.stages[bindings.current_stage].texel.back().image = true;
}
} // namespace D3D12
