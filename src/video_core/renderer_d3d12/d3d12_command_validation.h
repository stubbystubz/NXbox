// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <fmt/format.h>
#include "video_core/renderer_d3d12/d3d12_format_cast.h"

namespace D3D12 {
constexpr bool ValidCopyRange(std::uint64_t width, std::uint64_t offset, std::uint64_t size) {
    return size && offset <= width && size <= width - offset;
}
constexpr bool ValidDescriptorRange(std::uint64_t base, std::uint64_t address, std::uint32_t stride,
                                    std::uint32_t capacity, std::uint32_t count = 1) {
    if (!base || address < base || !stride || !count || (address - base) % stride)
        return false;
    const auto index = (address - base) / stride;
    return index < capacity && count <= capacity - index;
}

// Producer-owned, including in deferred mode. Reject malformed arguments before
// publishing them to the recorder; snapshot the summary at submission boundaries.
class CommandValidation {
public:
    const bool enabled{BisectEnabled("NXBOX_D3D12_VALIDATE_COMMANDS")};
    std::uint64_t tick{};
    std::array<std::uint64_t, 5> counts{};
    struct Heap {
        std::uint64_t base{};
        UINT stride{}, capacity{};
    };
    std::array<Heap, 2> heaps{};

    void Begin(std::uint64_t value) {
        tick = value;
        counts = {};
        heaps = {};
    }
    void Require(bool valid, const char* call, const std::string& args) const {
        if (valid)
            return;
        const auto message =
            fmt::format("D3D12_VALIDATION invalid tick={} call={} {}", tick, call, args);
        Diagnostic(message);
        throw std::invalid_argument(message);
    }
    void ViewFormat(const char* call, std::uint32_t resource, std::uint32_t view,
                    std::span<const std::uint32_t> castable = {}) const {
        if (!enabled)
            return;
        Require(CompatibleViewFormat(resource, view, castable), call,
                fmt::format("resource_format={} view_format={} resource_family={} view_family={} "
                            "castable_count={}",
                            resource, view, FormatFamily(resource), FormatFamily(view),
                            castable.size()));
    }
    void Root(const char* call, UINT index, std::uint64_t address, bool cbv) {
        if (!enabled)
            return;
        ++counts[0];
        Require(index < 64 && address && (!cbv || address % 256 == 0), call,
                fmt::format("parameter={} address={:#x} cbv={}", index, address, cbv));
    }
    void Heaps(ID3D12Device* device, UINT count, ID3D12DescriptorHeap* const* values) {
        if (!enabled)
            return;
        heaps = {};
        Require(count <= 2 && (!count || values), "SetDescriptorHeaps",
                fmt::format("count={}", count));
        for (UINT i = 0; i < count; ++i) {
            Require(values[i] != nullptr, "SetDescriptorHeaps", fmt::format("heap={}", i));
            const auto desc = values[i]->GetDesc();
            Require(static_cast<UINT>(desc.Type) <= D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
                        (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
                    "SetDescriptorHeaps",
                    fmt::format("heap={} type={}", i, static_cast<UINT>(desc.Type)));
            heaps[desc.Type] = {values[i]->GetGPUDescriptorHandleForHeapStart().ptr,
                                device->GetDescriptorHandleIncrementSize(desc.Type),
                                desc.NumDescriptors};
        }
    }
    void Table(const char* call, UINT index, std::uint64_t address) {
        if (!enabled)
            return;
        ++counts[1];
        const bool valid = std::any_of(heaps.begin(), heaps.end(), [&](const Heap& heap) {
            return ValidDescriptorRange(heap.base, address, heap.stride, heap.capacity);
        });
        Require(index < 64 && valid, call,
                fmt::format("parameter={} address={:#x} views={:#x}/{} samplers={:#x}/{}", index,
                            address, heaps[0].base, heaps[0].capacity, heaps[1].base,
                            heaps[1].capacity));
    }
    void BufferCopy(ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src, UINT64 src_offset,
                    UINT64 size) {
        if (!enabled)
            return;
        ++counts[2];
        if (!size) {
            Diagnostic(fmt::format(
                "D3D12_VALIDATION zero_size_copy tick={} call=CopyBufferRegion dst={} src={}", tick,
                dst_offset, src_offset));
            return;
        }
        const auto d = dst ? dst->GetDesc() : D3D12_RESOURCE_DESC{};
        const auto s = src ? src->GetDesc() : D3D12_RESOURCE_DESC{};
        Require(dst && src && d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                    s.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                    ValidCopyRange(d.Width, dst_offset, size) &&
                    ValidCopyRange(s.Width, src_offset, size),
                "CopyBufferRegion",
                fmt::format("dst={}/{} src={}/{} bytes={}", dst_offset, d.Width, src_offset,
                            s.Width, size));
    }
    void Resolve(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT first, UINT count,
                 ID3D12Resource* dst, UINT64 offset) {
        if (!enabled)
            return;
        ++counts[4];
        // ID3D12QueryHeap exposes no GetDesc, so the slot range is not checked here.
        const auto d = dst ? dst->GetDesc() : D3D12_RESOURCE_DESC{};
        const UINT stride = type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS
                                ? sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)
                            : type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 &&
                                    type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3
                                ? sizeof(D3D12_QUERY_DATA_SO_STATISTICS)
                                : 8;
        Require(heap && dst && count &&
                    d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && offset % 8 == 0 &&
                    ValidCopyRange(d.Width, offset, std::uint64_t{count} * stride),
                "ResolveQueryData",
                fmt::format("type={} first={} count={} offset={} width={} stride={}",
                            static_cast<UINT>(type), first, count, offset, d.Width,
                            stride));
    }
    struct Extent {
        std::uint64_t width{};
        UINT height{}, depth{};
        DXGI_FORMAT format{};
        bool valid{};
    };
    static Extent CopyExtent(ID3D12Device* device, const D3D12_TEXTURE_COPY_LOCATION* location) {
        if (!location || !location->pResource)
            return {};
        const auto desc = location->pResource->GetDesc();
        if (location->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            const auto& p = location->PlacedFootprint;
            const auto& f = p.Footprint;
            // Compressed formats count block rows, not texel rows. The extent check
            // below is independent of that format-specific footprint byte count.
            return {f.Width, f.Height, f.Depth, f.Format,
                    desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && f.Width && f.Height &&
                        f.Depth && f.RowPitch && p.Offset % 512 == 0 && f.RowPitch % 256 == 0 &&
                        p.Offset < desc.Width};
        }
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || !desc.MipLevels ||
            !desc.DepthOrArraySize || desc.MipLevels > 32)
            return {};
        const auto layers =
            desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize;
        D3D12_FEATURE_DATA_FORMAT_INFO format_info{desc.Format, 0};
        if (!device)
            return {};
        const bool known_planes =
            device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format_info,
                                        sizeof(format_info)) >= 0 &&
            format_info.PlaneCount;
        if (!known_planes)
            Diagnostic(fmt::format("D3D12_VALIDATION unavailable call=FormatInfo format={}",
                                   static_cast<UINT>(desc.Format)));
        const auto mip = location->SubresourceIndex % desc.MipLevels;
        const auto plane = location->SubresourceIndex / (desc.MipLevels * layers);
        const bool compressed =
            (desc.Format >= DXGI_FORMAT_BC1_TYPELESS && desc.Format <= DXGI_FORMAT_BC5_SNORM) ||
            (desc.Format >= DXGI_FORMAT_BC6H_TYPELESS && desc.Format <= DXGI_FORMAT_BC7_UNORM_SRGB);
        const auto width = std::max<std::uint64_t>(1, desc.Width >> mip);
        const auto height = std::max(1u, desc.Height >> mip);
        // BC copies can include the padded block at the edge of a small mip.
        return {compressed ? (width + 3) & ~std::uint64_t{3} : width,
                compressed ? (height + 3) & ~UINT{3} : height,
                desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                    ? std::max(1u, static_cast<UINT>(desc.DepthOrArraySize) >> mip)
                    : 1u,
                desc.Format, !known_planes || plane < format_info.PlaneCount};
    }
    void TextureCopy(ID3D12Device* device, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y,
                     UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
        if (!enabled)
            return;
        ++counts[3];
        const auto d = CopyExtent(device, dst), s = CopyExtent(device, src);
        const std::uint64_t left = box ? box->left : 0, right = box ? box->right : s.width;
        const UINT top = box ? box->top : 0, bottom = box ? box->bottom : s.height;
        const UINT front = box ? box->front : 0, back = box ? box->back : s.depth;
        const auto args = fmt::format(
            "dst={}x{}x{} at={},{},{} src={}x{}x{} box={},{},{}:{},{},{} formats={}/{}", d.width,
            d.height, d.depth, x, y, z, s.width, s.height, s.depth, left, top, front, right, bottom,
            back, static_cast<UINT>(s.format), static_cast<UINT>(d.format));
        Require(d.valid && s.valid, "CopyTextureRegion", args);
        if (right == left || bottom == top || back == front) {
            Diagnostic(fmt::format(
                "D3D12_VALIDATION zero_size_copy tick={} call=CopyTextureRegion {}", tick, args));
            return;
        }
        Require(d.valid && s.valid && right > left && bottom > top && back > front &&
                    right <= s.width && bottom <= s.height && back <= s.depth &&
                    ValidCopyRange(d.width, x, right - left) &&
                    ValidCopyRange(d.height, y, bottom - top) &&
                    ValidCopyRange(d.depth, z, back - front),
                "CopyTextureRegion", args);
        // Include formats/subresources for legal-size copies whose format compatibility
        // requires driver/debug-layer knowledge. Do not assume equal bytes imply compatibility.
        Diagnostic(fmt::format(
            "D3D12_VALIDATION texture_copy tick={} {} dst_subresource={} src_subresource={}", tick,
            args,
            dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? dst->SubresourceIndex : 0,
            src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? src->SubresourceIndex : 0));
    }
    std::string Summary() const {
        return fmt::format("D3D12_VALIDATION before_execute tick={} roots={} tables={} "
                           "buffer_copies={} texture_copies={} resolves={}",
                           tick, counts[0], counts[1], counts[2], counts[3], counts[4]);
    }
};
} // namespace D3D12
