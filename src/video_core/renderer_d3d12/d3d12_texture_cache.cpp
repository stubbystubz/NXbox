// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <bc_decoder.h>
#include <fmt/format.h>
#include "common/alignment.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/framebuffer_config.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_astc_bc.h"
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
#include "video_core/renderer_d3d12/d3d12_footprint.h"
#include "video_core/renderer_d3d12/d3d12_resolution.h"
#include "video_core/texture_cache/accelerated_swizzle.h"
#include "video_core/texture_cache/image_view_info.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"

namespace D3D12 {
FormatInfo MaxwellFormat(VideoCore::Surface::PixelFormat format) {
    switch (format) {
#define PIXEL_FORMAT_ELEM(name, ...)                                                               \
    case VideoCore::Surface::PixelFormat::name:                                                    \
        return LookupFormat(#name);
        PIXEL_FORMAT_LIST
#undef PIXEL_FORMAT_ELEM
    default:
        return LookupFormat("");
    }
}
namespace {
void UnsupportedImage(const VideoCommon::ImageInfo& info, FormatInfo format,
                      std::string_view condition) {
    static std::mutex mutex;
    static std::set<std::tuple<u32, u32, u32, u32, std::string>> logged;
    std::scoped_lock lock{mutex};
    if (!logged
             .emplace(static_cast<u32>(info.format), format.planes, info.num_samples,
                      static_cast<u32>(info.type), std::string{condition})
             .second)
        return;
    std::string_view name = "Unknown";
    switch (info.format) {
#define PIXEL_FORMAT_ELEM(name_, ...)                                                              \
    case VideoCore::Surface::PixelFormat::name_:                                                   \
        name = #name_;                                                                             \
        break;
        PIXEL_FORMAT_LIST
#undef PIXEL_FORMAT_ELEM
    default:
        break;
    }
    LOG_WARNING(Render,
                "D3D12 image unsupported: format={} ({}) planes={} samples={} type={} "
                "condition={}; attachment/transfer skipped",
                name, static_cast<u32>(info.format), format.planes, info.num_samples,
                static_cast<u32>(info.type), condition);
}
ResourcePtr MakeTexture(Context& context, const VideoCommon::ImageInfo& info, FormatInfo format) {
    const auto skip = [&](std::string_view condition) -> ResourcePtr {
        UnsupportedImage(info, format, condition);
        return {};
    };
    if (!format.Supported())
        return skip("!format.Supported()");
    if (format.planes != 1 && !(format.depth && format.planes == 2))
        return skip("planes != 1 and not a two-plane depth/stencil format");
    if (format.depth && info.type == VideoCommon::ImageType::e3D)
        return skip("depth/stencil resources cannot be 3D");
    if (info.type == VideoCommon::ImageType::Buffer)
        return skip("image type is Buffer");
    if (!info.size.width || !info.size.height || !info.size.depth || info.resources.levels <= 0 ||
        info.resources.levels > 16 || info.resources.layers <= 0 || info.resources.layers > 2048 ||
        info.size.depth > 2048)
        return skip("invalid dimensions/levels/layers");
    if (info.num_samples == 0 || (info.num_samples & (info.num_samples - 1)) ||
        info.num_samples > 16)
        return skip("sample count is not 1/2/4/8/16");
    if (info.num_samples > 1 &&
        (format.converted_astc || format.conversion == FormatConversion::Etc2 ||
         format.conversion == FormatConversion::Eac || format.conversion == FormatConversion::Bcn))
        return skip("guest block-compressed formats cannot be multisampled");
    if (info.num_samples > 1 &&
        (info.type != VideoCommon::ImageType::e2D || format.block_width != 1 ||
         format.block_height != 1 || info.resources.levels != 1))
        return skip("MSAA requires uncompressed 2D image with one mip level");
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = info.type == VideoCommon::ImageType::e1D ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                     : info.type == VideoCommon::ImageType::e3D
                         ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                         : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);
    const u32 width = info.size.width >> samples_x;
    const u32 height = info.size.height >> samples_y;
    if (!width || !height || (width << samples_x) != info.size.width ||
        (height << samples_y) != info.size.height)
        return skip("image dimensions are not aligned to the MSAA sample grid");
    desc.Width = (u64{width} + format.block_width - 1) / format.block_width * format.block_width;
    desc.Height =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
            ? 1
            : (height + format.block_height - 1) / format.block_height * format.block_height;
    desc.DepthOrArraySize = static_cast<UINT16>(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                    ? info.size.depth
                                                    : info.resources.layers);
    desc.MipLevels = static_cast<UINT16>(info.resources.levels);
    const u32 max_dimension = info.type == VideoCommon::ImageType::e3D ? 2048u : 16384u;
    if (desc.Width > max_dimension || desc.Height > max_dimension)
        return skip("dimensions exceed the D3D12 texture limit");
    const u32 max_extent =
        std::max({static_cast<u32>(desc.Width), desc.Height,
                  info.type == VideoCommon::ImageType::e3D ? info.size.depth : 1u});
    if (desc.MipLevels > std::bit_width(max_extent))
        return skip("mip count exceeds the image dimension chain");
    desc.Format = static_cast<DXGI_FORMAT>(format.resource_format);
    desc.SampleDesc.Count = info.num_samples;
    if (info.num_samples > 1) {
        desc.MipLevels = 1;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
        quality.Format = format.depth ? DsvFormat(static_cast<DXGI_FORMAT>(format.view_format))
                                      : static_cast<DXGI_FORMAT>(format.view_format);
        quality.SampleCount = info.num_samples;
        quality.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        const HRESULT result = context.Device()->CheckFeatureSupport(
            D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality));
        if (FAILED(result)) {
            Check(context.Device()->GetDeviceRemovedReason(), "MSAA support device removed",
                  context.Device());
            return skip(
                fmt::format("CheckFeatureSupport(MULTISAMPLE_QUALITY_LEVELS) HRESULT={:#010x}",
                            static_cast<u32>(result)));
        }
        if (!quality.NumQualityLevels)
            return skip("MULTISAMPLE_QUALITY_LEVELS.NumQualityLevels == 0");
        // Quality zero is shared by every supported attachment format. Do not
        // reduce samples: that would break guest Texture2DMS loads and PSO matching.
        desc.SampleDesc.Quality = 0;
    }
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{static_cast<DXGI_FORMAT>(format.view_format)};
    const auto query_format = [&](D3D12_FEATURE_DATA_FORMAT_SUPPORT& data,
                                  std::string_view condition) {
        const HRESULT result = context.Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                                     &data, sizeof(data));
        if (SUCCEEDED(result))
            return true;
        Check(context.Device()->GetDeviceRemovedReason(), "format support device removed",
              context.Device());
        UnsupportedImage(info, format,
                         fmt::format("{} HRESULT={:#010x}", condition, static_cast<u32>(result)));
        return false;
    };
    if (!query_format(support, "texture FORMAT_SUPPORT query"))
        return {};
    const auto required =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D   ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
        : desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_FORMAT_SUPPORT1_TEXTURE3D
                                                               : D3D12_FORMAT_SUPPORT1_TEXTURE2D;
    if (!(support.Support1 & required))
        return skip("FORMAT_SUPPORT lacks requested texture dimension");
    // P3: guest draws render into cached images.
    if (format.depth) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT depth_support{
            DsvFormat(static_cast<DXGI_FORMAT>(format.view_format))};
        if (!query_format(depth_support, "depth FORMAT_SUPPORT query"))
            return {};
        if (depth_support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    } else if ((support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) &&
               desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    // Diagnostic: UAV-capable render targets may lose DCC on AMD; measure without the flag.
    static const bool rt_no_uav = BisectEnabled("NXBOX_D3D12_RT_NO_UAV");
    const bool render_target = desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (info.num_samples == 1 && !format.depth && format.block_width == 1 &&
        format.block_height == 1 && !(rt_no_uav && render_target)) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT uav_support{
            format.converted_astc ? DXGI_FORMAT_R8G8B8A8_UNORM
                                  : static_cast<DXGI_FORMAT>(format.view_format)};
        if (!query_format(uav_support, "UAV FORMAT_SUPPORT query"))
            return {};
        if (uav_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (format.depth && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return skip("FORMAT_SUPPORT lacks DEPTH_STENCIL");
    if (info.num_samples > 1 && !format.depth &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
        return skip("MSAA format lacks RENDER_TARGET support");
    try {
        return context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    } catch (const std::exception& error) {
        return skip(fmt::format("CreateResource failed: {}", error.what()));
    }
}
UINT Subresource(const VideoCommon::ImageInfo& info, s32 level, s32 layer) {
    if (level < 0 || level >= info.resources.levels || layer < 0 || layer >= info.resources.layers)
        throw std::out_of_range("D3D12 image subresource");
    return static_cast<UINT>(level + layer * info.resources.levels);
}
} // namespace
Image::Image(TextureCacheRuntime& runtime, const VideoCommon::ImageInfo& info, GPUVAddr gpu,
             VAddr cpu)
    : ImageBase{info, gpu, cpu}, allocation_tick{runtime.context.CurrentTick()}, runtime{&runtime},
      host_format{MaxwellFormat(info.format)} {
    if (host_format.converted_astc) {
        flags |= VideoCommon::ImageFlagBits::Converted;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
        astc_bc_candidate = Settings::values.astc_recompression.GetValue() !=
                            Settings::AstcRecompression::Uncompressed;
        static const std::string skip_rule = [] {
            const char* value = std::getenv("NXBOX_ASTC_BC_SKIP");
            return value ? std::string{value} : std::string{};
        }();
        astc_uncompressed =
            astc_bc_candidate &&
            (!AstcBcEligible(info.type == VideoCommon::ImageType::e2D, info.size.width,
                             info.size.height, info.size.depth, info.resources.layers,
                             info.num_samples) ||
             (!skip_rule.empty() &&
              AstcBcSkip(skip_rule,
                         AstcBcClasses(info.size.width, info.size.height, info.resources.levels,
                                       info.resources.layers, host_format.name, gpu_addr))));
        if (astc_uncompressed)
            converted_size_bytes = VideoCommon::CalculateConvertedSizeBytes(info, true);
        const auto recompression = astc_uncompressed
                                       ? Settings::AstcRecompression::Uncompressed
                                       : Settings::values.astc_recompression.GetValue();
        switch (recompression) {
        case Settings::AstcRecompression::Bc1:
            host_format = LookupFormat("BC1_RGBA_UNORM");
            break;
        case Settings::AstcRecompression::Bc3:
            host_format = LookupFormat("BC3_UNORM");
            break;
        default:
            break;
        }
        if (VideoCore::Surface::IsPixelFormatSRGB(info.format)) {
            if (host_format.block_width == 4)
                host_format = LookupFormat(Settings::values.astc_recompression.GetValue() ==
                                                   Settings::AstcRecompression::Bc1
                                               ? "BC1_RGBA_SRGB"
                                               : "BC3_SRGB");
        }
    }
    // The Xbox samples small native BCn cubes wrong: SM3DW's vertex shaders read its 8x8 BC3
    // ambient cubes ~20x too dark although the uploaded blocks are right, which blacked out
    // shaded walls. Decoding them on the CPU fixes it and costs little at this size.
    // ponytail: size cap is from the one case seen; raise it if larger BC cubes misbehave.
    // NXBOX_D3D12_NATIVE_SMALL_BC_CUBES=1 keeps them native.
    static const bool native_small_bc_cubes = BisectEnabled("NXBOX_D3D12_NATIVE_SMALL_BC_CUBES");
    if (!native_small_bc_cubes && host_format.block_width == 4 &&
        VideoCore::Surface::IsPixelFormatBCn(info.format) && info.resources.layers % 6 == 0 &&
        info.size.width == info.size.height && info.size.width <= 32) {
        host_format = BcnFallbackFormat(host_format);
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    }
    // The same Xbox quirk hits native BCn 2D arrays: SM3DW's glyph sheets are BC4 1024x1024
    // arrays whose layers 1 and 2 sample about 130 rows below where CopyTextureRegion and
    // readback place them (button icons showed as katakana, a copy of the resource did not
    // help). Decoding the layers on the CPU keeps the data where the sampler reads it.
    // The texel cap keeps big arrays native. NXBOX_D3D12_NATIVE_BC_ARRAYS=1 keeps all native.
    static const bool native_bc_arrays = BisectEnabled("NXBOX_D3D12_NATIVE_BC_ARRAYS");
    constexpr u64 MaxDecodedArrayTexels = u64{4} * 1024 * 1024;
    if (!native_bc_arrays && host_format.block_width == 4 &&
        VideoCore::Surface::IsPixelFormatBCn(info.format) &&
        info.type == VideoCommon::ImageType::e2D && info.resources.layers > 1 &&
        info.resources.layers % 6 != 0 && info.num_samples == 1 &&
        u64{info.size.width} * info.size.height * info.resources.layers <= MaxDecodedArrayTexels) {
        host_format = BcnFallbackFormat(host_format);
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    }
    resource = MakeTexture(runtime.context, info, host_format);
    if (!resource && host_format.block_width == 4 &&
        (VideoCore::Surface::IsPixelFormatBCn(info.format) ||
         VideoCore::Surface::IsPixelFormatASTC(info.format))) {
        // Native BCn upload still consumes original encoded guest blocks. ASTC
        // recompression already produces BC1/BC3 blocks through Converted.
        host_format = BcnFallbackFormat(host_format);
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
        resource = MakeTexture(runtime.context, info, host_format);
    }
    if (resource && BisectEnabled("NXBOX_D3D12_GPU_ASTC") &&
        VideoCore::Surface::IsPixelFormatASTC(info.format) &&
        info.type == VideoCommon::ImageType::e2D && info.num_samples == 1 &&
        host_format.block_width == 1 &&
        (resource->handle->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
    }
    // The scaling blitter handles uncompressed single-sample 2D color/depth arrays.
    // Keep unsupported attachments native as a group through ImageCanRescale.
    this->info.rescaleable &= resource && info.type == VideoCommon::ImageType::e2D &&
                              info.num_samples == 1 && info.resources.levels == 1 &&
                              host_format.block_width == 1 && host_format.block_height == 1;
    original_resource = resource;
    TraceAstcBc("create");
    // CPU ASTC decoding runs inline on the GPU thread: Wonder spent up to 1.3 s of every 5 s
    // there while streaming a new area. accelerate_astc=2 moves it to the texture decode worker;
    // the texture samples empty until its upload lands at a later frame tick.
    if (True(flags & VideoCommon::ImageFlagBits::Converted) &&
        False(flags & VideoCommon::ImageFlagBits::AcceleratedUpload) &&
        Settings::values.accelerate_astc.GetValue() == Settings::AstcDecodeMode::CpuAsynchronous) {
        flags |= VideoCommon::ImageFlagBits::AsynchronousDecode;
    }
}
void Image::BlitScale(const ResourcePtr& destination, const ResourcePtr& source) {
    const auto region = [](const ResourcePtr& texture) {
        const auto desc = texture->Description();
        return VideoCommon::Region2D{{0, 0},
                                     {static_cast<s32>(desc.Width), static_cast<s32>(desc.Height)}};
    };
    for (s32 layer = 0; layer < info.resources.layers; ++layer) {
        const BlitSurface dst{destination, host_format, 0, static_cast<u32>(layer)};
        const BlitSurface src{source, host_format, 0, static_cast<u32>(layer)};
        if (host_format.depth) {
            runtime->BlitHelpers().BlitDepthStencil(dst, src, region(destination), region(source));
        } else {
            const auto filter = VideoCore::Surface::IsPixelFormatInteger(info.format)
                                    ? Tegra::Engines::Fermi2D::Filter::Point
                                    : Tegra::Engines::Fermi2D::Filter::Bilinear;
            runtime->BlitHelpers().BlitColor(dst, src, region(destination), region(source), filter);
        }
    }
}

bool Image::ScaleUp(bool ignore) {
    const auto& resolution = Settings::values.resolution_info;
    if (!resolution.active || IsRescaled() || !info.rescaleable || !resource)
        return false;
    if (!scaled_resource) {
        const ResolutionScale scale{resolution.up_scale, resolution.down_shift};
        auto scaled_info = info;
        scaled_info.size.width = scale.Size(info.size.width);
        scaled_info.size.height = scale.Size(info.size.height);
        scaled_resource = MakeTexture(runtime->context, scaled_info, host_format);
        if (!scaled_resource)
            throw std::runtime_error("D3D12 scaled render-target allocation failed");
        ignore = false;
        has_scaled = true;
    }
    if (!ignore)
        BlitScale(scaled_resource, original_resource);
    resource = scaled_resource;
    flags |= VideoCommon::ImageFlagBits::Rescaled;
    return true;
}

bool Image::ScaleDown(bool ignore) {
    if (!IsRescaled())
        return false;
    if (!ignore)
        BlitScale(original_resource, scaled_resource);
    resource = original_resource;
    flags &= ~VideoCommon::ImageFlagBits::Rescaled;
    return true;
}

bool Image::AstcBcDiagnosticEnabled() const {
    static const bool enabled = BisectEnabled("NXBOX_ASTC_BC_DIAG");
    return enabled && astc_bc_candidate;
}
void Image::TraceAstcBc(std::string_view event, std::string_view detail) const {
    if (!AstcBcDiagnosticEnabled())
        return;
    Diagnostic(fmt::format(
        "ASTC_BC event={} gpu={:x} size={}x{}x{} mips={} layers={} type={} "
        "block={}x{} format={} srgb={} host={} host_block={}x{} rgba={} rt_view={} copy_dst={} {}",
        event, gpu_addr, info.size.width, info.size.height, info.size.depth, info.resources.levels,
        info.resources.layers, static_cast<u32>(info.type),
        VideoCore::Surface::DefaultBlockWidth(info.format),
        VideoCore::Surface::DefaultBlockHeight(info.format), MaxwellFormat(info.format).name,
        VideoCore::Surface::IsPixelFormatSRGB(info.format), host_format.name,
        host_format.block_width, host_format.block_height, astc_uncompressed, astc_bc_rt,
        astc_bc_copy_dst, detail));
}
void Image::NoteAstcBcUse(bool render_target) {
    bool& seen = render_target ? astc_bc_rt : astc_bc_copy_dst;
    if (seen)
        return;
    seen = true;
    TraceAstcBc(render_target ? "rt_view" : "copy_dst");
}
void Image::UploadMemory(const StagingBufferRef& map,
                         std::span<const VideoCommon::BufferImageCopy> copies) {
    if (AstcBcDiagnosticEnabled()) {
        for (const auto& upload_copy : copies) {
            const auto upload_level = upload_copy.image_subresource.base_level;
            const auto& upload_extent = upload_copy.image_extent;
            const auto& upload_offset = upload_copy.image_offset;
            const bool partial =
                upload_level < 0 || upload_level >= info.resources.levels || upload_level >= 32 ||
                upload_offset.x || upload_offset.y || upload_offset.z ||
                upload_extent.width != std::max(1u, info.size.width >> upload_level) ||
                upload_extent.height != std::max(1u, info.size.height >> upload_level) ||
                upload_extent.depth != (info.type == VideoCommon::ImageType::e3D
                                            ? std::max(1u, info.size.depth >> upload_level)
                                            : 1u) ||
                upload_copy.image_subresource.base_layer != 0 ||
                upload_copy.image_subresource.num_layers != info.resources.layers;
            TraceAstcBc(partial ? "partial_upload" : "full_upload",
                        fmt::format("level={} layer={} count={} origin={},{},{} extent={}x{}x{} "
                                    "buffer_offset={} buffer_size={} row_length={} image_height={}",
                                    upload_level, upload_copy.image_subresource.base_layer,
                                    upload_copy.image_subresource.num_layers, upload_offset.x,
                                    upload_offset.y, upload_offset.z, upload_extent.width,
                                    upload_extent.height, upload_extent.depth,
                                    upload_copy.buffer_offset, upload_copy.buffer_size,
                                    upload_copy.buffer_row_length,
                                    upload_copy.buffer_image_height));
        }
    }
    Transfer(map, copies, false);
}
void Image::DownloadMemory(const StagingBufferRef& map,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    if (host_format.converted_astc || VideoCore::Surface::IsPixelFormatASTC(info.format)) {
        UnsupportedImage(info, host_format, "converted ASTC readback is not reversible");
        return;
    }
    Transfer(map, copies, true);
}
void Image::UploadMemory(ResourcePtr buffer, size_t offset,
                         std::span<const VideoCommon::BufferImageCopy> copies) {
    if (!buffer || offset > buffer->mapped.size())
        throw std::runtime_error("D3D12 P2 upload requires mapped staging");
    UploadMemory({buffer, offset, buffer->mapped.subspan(offset)}, copies);
}
void Image::DownloadMemory(ResourcePtr buffer, size_t offset,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    if (!buffer || offset > buffer->mapped.size())
        throw std::runtime_error("D3D12 P2 download requires mapped staging");
    DownloadMemory({buffer, offset, buffer->mapped.subspan(offset)}, copies);
}
void Image::DownloadMemory(std::span<const ResourcePtr> buffers, std::span<const size_t> offsets,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    if (buffers.size() != 2 || offsets.size() != 2 || !buffers[0] || !buffers[1] ||
        buffers[0]->heap != D3D12_HEAP_TYPE_DEFAULT || offsets[1] > buffers[1]->mapped.size())
        throw std::invalid_argument("D3D12 invalid DMA download destinations");
    if (host_format.converted_astc || VideoCore::Surface::IsPixelFormatASTC(info.format)) {
        UnsupportedImage(info, host_format, "converted ASTC readback is not reversible");
        return;
    }
    Transfer({buffers[1], offsets[1], buffers[1]->mapped.subspan(offsets[1])}, copies, true,
             buffers[0], offsets[0]);
}
void Image::Transfer(const StagingBufferRef& input,
                     std::span<const VideoCommon::BufferImageCopy> copies, bool download,
                     const ResourcePtr& gpu_destination, size_t gpu_offset) {
    const bool restore_scaled = ScaleDown();
    SCOPE_EXIT {
        if (restore_scaled)
            ScaleUp(download);
    };
    const FrameProfile::Scope profile_transfer{runtime->context.Profile(),
                                               download ? FramePhase::TextureDownload
                                                        : FramePhase::TextureUpload};
    if (!resource)
        return;
    if (download && VideoCore::Surface::IsPixelFormatBCn(info.format) &&
        host_format.block_width == 1) {
        UnsupportedImage(
            info, host_format,
            "decoded BCn readback needs a lossy encoder; original guest blocks retained");
        return;
    }
    if (resource->handle->GetDesc().SampleDesc.Count > 1) {
        // Guest memory stores every sample as a texel in an expanded grid. Keep
        // staging on a single-sample image and scatter/gather with shader loads.
        auto single_info = info;
        single_info.num_samples = 1;
        Image single{*runtime, single_info, 0, 0};
        if (!single.Handle())
            return;
        const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);
        std::vector<VideoCommon::ImageCopy> image_copies;
        image_copies.reserve(copies.size());
        for (const auto& copy : copies) {
            const auto& offset = copy.image_offset;
            const auto& extent = copy.image_extent;
            if (offset.x < 0 || offset.y < 0 ||
                (static_cast<u32>(offset.x) & ((1u << samples_x) - 1)) ||
                (static_cast<u32>(offset.y) & ((1u << samples_y) - 1)) ||
                (extent.width & ((1u << samples_x) - 1)) ||
                (extent.height & ((1u << samples_y) - 1))) {
                UnsupportedImage(info, host_format,
                                 "MSAA staging region is not sample-grid aligned");
                return;
            }
            VideoCommon::ImageCopy region{};
            region.src_subresource = region.dst_subresource = copy.image_subresource;
            region.src_offset = offset; // Helper MSAA source offsets use expanded-grid units.
            region.dst_offset = download ? offset
                                         : VideoCommon::Offset3D{offset.x >> samples_x,
                                                                 offset.y >> samples_y, offset.z};
            region.extent = download
                                ? extent
                                : VideoCommon::Extent3D{extent.width >> samples_x,
                                                        extent.height >> samples_y, extent.depth};
            image_copies.push_back(region);
        }
        if (download) {
            runtime->CopyImageMSAA(single, *this, image_copies);
            single.Transfer(input, copies, true, gpu_destination, gpu_offset);
        } else {
            single.Transfer(input, copies, false);
            runtime->CopyImageMSAA(*this, single, image_copies);
        }
        return;
    }
    if (host_format.conversion != FormatConversion::None) {
        if (!TransferConverted(input, copies, download) || !gpu_destination)
            return;
        // Planar/packed GPU DMA needs the same guest byte representation as CPU
        // readback. Wait for packing, then upload it; correctness takes priority
        // over the native unconverted DMA fast path.
        auto& context = runtime->context;
        context.Finish(FrameCount::ForcedSubmitTextureDownload);
        const auto origin =
            static_cast<size_t>(input.mapped_span.data() - input.buffer->mapped.data());
        const auto tight = input.mapped_span.subspan(input.offset - origin);
        const u32 guest_bytes = VideoCore::Surface::BytesPerBlock(info.format);
        for (const auto& copy : copies) {
            const u32 row_length =
                copy.buffer_row_length ? copy.buffer_row_length : copy.image_extent.width;
            const u32 image_height =
                copy.buffer_image_height ? copy.buffer_image_height : copy.image_extent.height;
            const u64 row_stride = u64{row_length} * guest_bytes;
            const u64 slice_stride = row_stride * image_height;
            const u64 row_bytes = u64{copy.image_extent.width} * guest_bytes;
            for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer)
                for (u32 z = 0; z < copy.image_extent.depth; ++z)
                    for (u32 y = 0; y < copy.image_extent.height; ++y) {
                        const u64 base =
                            copy.buffer_offset +
                            slice_stride *
                                (u64{static_cast<u32>(layer)} * copy.image_extent.depth + z) +
                            row_stride * y;
                        const u64 destination = CheckedAdd(gpu_offset, base);
                        if (destination > gpu_destination->handle->GetDesc().Width ||
                            row_bytes > gpu_destination->handle->GetDesc().Width - destination) {
                            UnsupportedImage(info, host_format,
                                             "converted DMA destination outside buffer");
                            return;
                        }
                        auto staging = runtime->UploadStagingBuffer(row_bytes);
                        std::memcpy(staging.mapped_span.data(), tight.data() + base, row_bytes);
                        context.Transition(gpu_destination, D3D12_RESOURCE_STATE_COPY_DEST);
                        context.Retain(staging.buffer);
                        context.MarkWork("upload/copy");
                        context.BeforeCopyWrite(gpu_destination, destination, row_bytes);
                        context.Commands()->CopyBufferRegion(
                            gpu_destination->handle.Get(), destination,
                            staging.buffer->handle.Get(), staging.offset, row_bytes);
                    }
        }
        return;
    }
    auto map = input;
    if (!map.buffer || map.mapped_span.empty())
        throw std::invalid_argument("D3D12 missing mapped texture staging");
    const auto origin = static_cast<size_t>(map.mapped_span.data() - map.buffer->mapped.data());
    if (map.offset < origin || map.offset - origin > map.mapped_span.size())
        throw std::out_of_range("D3D12 texture staging offset");
    map.mapped_span = map.mapped_span.subspan(map.offset - origin);
    auto& context = runtime->context;
    context.Transition(resource, download ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                          : D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& copy : copies) {
        const auto& extent = copy.image_extent;
        if (!extent.width || !extent.height || !extent.depth)
            throw std::invalid_argument("D3D12 empty image copy");
        const auto rows =
            (u64{extent.height} + host_format.block_height - 1) / host_format.block_height;
        const auto row_length = copy.buffer_row_length ? copy.buffer_row_length : extent.width;
        const auto image_height =
            copy.buffer_image_height ? copy.buffer_image_height : extent.height;
        if (row_length < extent.width || image_height < extent.height ||
            copy.image_subresource.num_layers <= 0)
            throw std::invalid_argument("D3D12 invalid guest texture stride");
        const auto row_stride = CheckedMultiply((u64{row_length} + host_format.block_width - 1) /
                                                    host_format.block_width,
                                                host_format.bytes_per_block);
        const auto slice_stride =
            CheckedMultiply(row_stride, (u64{image_height} + host_format.block_height - 1) /
                                            host_format.block_height);
        // Use the SDK's layout for each transfer extent, then repack guest rows. Block
        // formats need whole blocks: the D3D12 runtime rejects a 2x2 or 1x1 BC footprint
        // (vkd3d accepts it), which left the small mips of BC textures never uploaded.
        auto desc = resource->handle->GetDesc();
        desc.Width = Common::AlignUp(u64{extent.width}, host_format.block_width);
        desc.Height = static_cast<UINT>(Common::AlignUp(extent.height, host_format.block_height));
        desc.DepthOrArraySize =
            static_cast<UINT16>(info.type == VideoCommon::ImageType::e3D ? extent.depth : 1);
        desc.MipLevels = 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT num_rows{};
        UINT64 row_bytes{}, total{};
        context.Device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_bytes,
                                                &total);
        if (total == UINT64_MAX || !row_bytes || num_rows != rows)
            throw std::runtime_error("D3D12 invalid SDK texture footprint");
        const auto padded_slice = CheckedMultiply(footprint.Footprint.RowPitch, num_rows);
        const auto padded_size = CheckedMultiply(padded_slice, extent.depth);
        for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
            const auto subresource = Subresource(info, copy.image_subresource.base_level,
                                                 copy.image_subresource.base_layer + layer);
            const auto base =
                CheckedAdd(copy.buffer_offset,
                           CheckedMultiply(slice_stride, CheckedMultiply(extent.depth, layer)));
            const auto required = CheckedAdd(
                base, CheckedAdd(CheckedMultiply(slice_stride, extent.depth - 1),
                                 CheckedAdd(CheckedMultiply(row_stride, rows - 1), row_bytes)));
            if (required > map.mapped_span.size())
                throw std::out_of_range("D3D12 guest texture staging bounds");
            const auto level = copy.image_subresource.base_level;
            const auto mip_width = std::max(1u, info.size.width >> level);
            const auto mip_height = std::max(1u, info.size.height >> level);
            const auto mip_depth = info.type == VideoCommon::ImageType::e3D
                                       ? std::max(1u, info.size.depth >> level)
                                       : 1u;
            const auto& offset = copy.image_offset;
            if (offset.x < 0 || offset.y < 0 || offset.z < 0 ||
                u64{static_cast<u32>(offset.x)} + extent.width > mip_width ||
                u64{static_cast<u32>(offset.y)} + extent.height > mip_height ||
                u64{static_cast<u32>(offset.z)} + extent.depth > mip_depth)
                throw std::out_of_range("D3D12 texture copy bounds");
            const bool full_subresource = offset.x == 0 && offset.y == 0 && offset.z == 0 &&
                                          extent.width == mip_width &&
                                          extent.height == mip_height && extent.depth == mip_depth;
            if (host_format.block_width > 1 && !full_subresource &&
                ((static_cast<u32>(offset.x) % host_format.block_width) ||
                 (static_cast<u32>(offset.y) % host_format.block_height) ||
                 (extent.width % host_format.block_width) ||
                 (extent.height % host_format.block_height)))
                throw std::invalid_argument("D3D12 partial BC copy must be block aligned");
            const bool direct_upload =
                !download && map.buffer->heap == D3D12_HEAP_TYPE_UPLOAD &&
                BisectEnabled("NXBOX_D3D12_DIRECT_TEXTURE_UPLOAD") &&
                CanDirectTextureUpload(CheckedAdd(map.offset, base), row_stride, slice_stride,
                                       footprint.Footprint.RowPitch, padded_slice) &&
                padded_size <= map.mapped_span.size() - base;
            auto aligned =
                direct_upload ? StagingBufferRef{map.buffer, static_cast<size_t>(map.offset + base),
                                                 map.mapped_span.subspan(base, padded_size)}
                : download    ? runtime->DownloadStagingBuffer(padded_size)
                              : runtime->UploadStagingBuffer(padded_size);
            ResourcePtr padded_gpu;
            if (gpu_destination) {
                // READBACK heaps cannot become COPY_SOURCE. Download once into a
                // DEFAULT footprint, then fan out to tight GPU storage and readback.
                padded_gpu = context.CreateBuffer(padded_size, D3D12_HEAP_TYPE_DEFAULT);
                context.Transition(padded_gpu, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            context.Retain(resource); // Staging pressure may have submitted the earlier retain.
            footprint.Offset = padded_gpu ? 0 : aligned.offset;
            D3D12_TEXTURE_COPY_LOCATION linear{};
            linear.pResource = padded_gpu ? padded_gpu->handle.Get() : aligned.buffer->handle.Get();
            linear.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            linear.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION texture{};
            texture.pResource = resource->handle.Get();
            texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            texture.SubresourceIndex = subresource;
            context.Retain(aligned.buffer);
            if (!download) {
                if (direct_upload) {
                    context.Profile().Count(FrameCount::TextureDirectUploadBytes,
                                            row_bytes * num_rows * extent.depth);
                } else {
                    context.Profile().Count(FrameCount::StagingCopyBytes, padded_size);
                    context.Profile().Count(FrameCount::TextureRepackBytes,
                                            row_bytes * num_rows * extent.depth);
                    const FrameProfile::Scope profile_repack{context.Profile(),
                                                             FramePhase::TextureRepack};
                    RepackRows(aligned.mapped_span, map.mapped_span.subspan(base), row_bytes,
                               num_rows, extent.depth, footprint.Footprint.RowPitch, padded_slice,
                               row_stride, slice_stride);
                }
                context.MarkWork("upload/copy");
                // Texture copy hazards are tracked per subresource (offset = index, size 1).
                context.BeforeCopyWrite(resource, subresource, 1);
                context.Commands()->CopyTextureRegion(&texture, offset.x, offset.y, offset.z,
                                                      &linear, nullptr);
            } else {
                D3D12_BOX box{static_cast<UINT>(offset.x),
                              static_cast<UINT>(offset.y),
                              static_cast<UINT>(offset.z),
                              static_cast<UINT>(offset.x) + extent.width,
                              static_cast<UINT>(offset.y) + extent.height,
                              static_cast<UINT>(offset.z) + extent.depth};
                context.MarkWork("upload/copy");
                context.BeforeCopyWrite(padded_gpu ? padded_gpu : aligned.buffer, footprint.Offset,
                                        padded_size);
                context.Commands()->CopyTextureRegion(&linear, 0, 0, 0, &texture,
                                                      full_subresource ? nullptr : &box);
                if (padded_gpu) {
                    context.Transition(padded_gpu, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    context.Transition(gpu_destination, D3D12_RESOURCE_STATE_COPY_DEST);
                    ForEachRowCopy(gpu_destination->handle->GetDesc().Width, padded_size,
                                   CheckedAdd(gpu_offset, base), 0, row_bytes, num_rows,
                                   extent.depth, row_stride, slice_stride,
                                   footprint.Footprint.RowPitch, padded_slice,
                                   [&](u64 destination, u64 source, u64 size) {
                                       context.BeforeCopyWrite(gpu_destination, destination, size);
                                       context.Commands()->CopyBufferRegion(
                                           gpu_destination->handle.Get(), destination,
                                           padded_gpu->handle.Get(), source, size);
                                   });
                    context.BeforeCopyWrite(aligned.buffer, aligned.offset, padded_size);
                    context.Commands()->CopyBufferRegion(aligned.buffer->handle.Get(),
                                                         aligned.offset, padded_gpu->handle.Get(),
                                                         0, padded_size);
                }
                // The caller's tight staging stays leased through CPU unpack after the fence.
                context.AfterCompletion([map, aligned, base, slice_stride, row_stride, padded_slice,
                                         row_bytes, num_rows, depth = extent.depth,
                                         pitch = footprint.Footprint.RowPitch] {
                    RepackRows(map.mapped_span.subspan(base), aligned.mapped_span, row_bytes,
                               num_rows, depth, row_stride, slice_stride, pitch, padded_slice);
                });
            }
        }
    }
}
// Packed and ETC/EAC conversions consume the original unswizzled guest layout.
// ASTC decompression/recompression uses the shared Converted upload path.
bool Image::TransferConverted(const StagingBufferRef& input,
                              std::span<const VideoCommon::BufferImageCopy> copies, bool download) {
    const auto conversion = host_format.conversion;
    const bool compressed = conversion == FormatConversion::Etc2 ||
                            conversion == FormatConversion::Eac ||
                            conversion == FormatConversion::Bcn;
    if (download && compressed) {
        UnsupportedImage(
            info, host_format,
            conversion == FormatConversion::Bcn
                ? "decoded BCn readback needs a lossy encoder; original guest blocks retained"
                : "ETC2/EAC readback needs a lossy encoder; original guest blocks retained");
        return false;
    }
    auto map = input;
    if (!map.buffer || map.mapped_span.empty()) {
        UnsupportedImage(info, host_format, "converted transfer missing mapped staging");
        return false;
    }
    const auto origin = static_cast<size_t>(map.mapped_span.data() - map.buffer->mapped.data());
    if (map.offset < origin || map.offset - origin > map.mapped_span.size()) {
        UnsupportedImage(info, host_format, "converted staging offset outside mapped range");
        return false;
    }
    map.mapped_span = map.mapped_span.subspan(map.offset - origin);
    auto& context = runtime->context;
    const auto encoded = conversion == FormatConversion::Bcn ? LookupFormat(host_format.name)
                                                             : MaxwellFormat(info.format);
    const u32 guest_bytes = conversion == FormatConversion::Bcn
                                ? encoded.bytes_per_block
                                : VideoCore::Surface::BytesPerBlock(info.format);
    const u32 block_width = conversion == FormatConversion::Bcn
                                ? encoded.block_width
                                : VideoCore::Surface::DefaultBlockWidth(info.format);
    const u32 block_height = conversion == FormatConversion::Bcn
                                 ? encoded.block_height
                                 : VideoCore::Surface::DefaultBlockHeight(info.format);
    const auto resource_desc = resource->handle->GetDesc();
    context.Transition(resource, download ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                          : D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& copy : copies) {
        const auto& extent = copy.image_extent;
        const auto& offset = copy.image_offset;
        const auto level = copy.image_subresource.base_level;
        if (level < 0 || level >= info.resources.levels || !extent.width || !extent.height ||
            !extent.depth || copy.image_subresource.num_layers <= 0 ||
            copy.image_subresource.base_layer < 0 ||
            copy.image_subresource.base_layer + copy.image_subresource.num_layers >
                info.resources.layers ||
            offset.x < 0 || offset.y < 0 || offset.z < 0 ||
            u64{static_cast<u32>(offset.x)} + extent.width >
                std::max(1u, info.size.width >> level) ||
            u64{static_cast<u32>(offset.y)} + extent.height >
                std::max(1u, info.size.height >> level) ||
            u64{static_cast<u32>(offset.z)} + extent.depth >
                (info.type == VideoCommon::ImageType::e3D ? std::max(1u, info.size.depth >> level)
                                                          : 1u)) {
            UnsupportedImage(info, host_format, "converted copy outside image subresources");
            return false;
        }
        const u32 row_length = copy.buffer_row_length ? copy.buffer_row_length : extent.width;
        const u32 image_height =
            copy.buffer_image_height ? copy.buffer_image_height : extent.height;
        if (row_length < extent.width || image_height < extent.height) {
            UnsupportedImage(info, host_format, "converted copy stride smaller than extent");
            return false;
        }
        // Depth resources require a whole-subresource CopyTextureRegion, with no box.
        if (host_format.depth && (offset.x || offset.y || offset.z ||
                                  extent.width != std::max(1u, info.size.width >> level) ||
                                  extent.height != std::max(1u, info.size.height >> level))) {
            UnsupportedImage(info, host_format,
                             "partial depth/stencil copy requires shader conversion");
            return false;
        }
        const u64 row_stride = ((u64{row_length} + block_width - 1) / block_width) * guest_bytes;
        const u64 slice_stride =
            row_stride * ((u64{image_height} + block_height - 1) / block_height);
        const u64 guest_row_bytes =
            ((u64{extent.width} + block_width - 1) / block_width) * guest_bytes;
        const u64 guest_rows = (u64{extent.height} + block_height - 1) / block_height;
        for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
            const u64 base =
                copy.buffer_offset + slice_stride * extent.depth * static_cast<u32>(layer);
            const u64 size =
                slice_stride * (extent.depth - 1) + row_stride * (guest_rows - 1) + guest_row_bytes;
            if (base > map.mapped_span.size() || size > map.mapped_span.size() - base) {
                UnsupportedImage(info, host_format, "converted copy outside guest staging");
                return false;
            }
            for (u32 plane = 0; plane < host_format.planes; ++plane) {
                auto desc = resource_desc;
                desc.Width = extent.width;
                desc.Height = extent.height;
                desc.DepthOrArraySize = static_cast<UINT16>(
                    info.type == VideoCommon::ImageType::e3D ? extent.depth : 1);
                desc.MipLevels = 1;
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
                UINT rows{};
                UINT64 row_bytes{}, total{};
                context.Device()->GetCopyableFootprints(&desc, plane, 1, 0, &footprint, &rows,
                                                        &row_bytes, &total);
                if (total == UINT64_MAX || rows != extent.height || !row_bytes) {
                    UnsupportedImage(info, host_format, "invalid converted SDK footprint");
                    return false;
                }
                const u64 padded_slice = u64{footprint.Footprint.RowPitch} * rows;
                auto aligned = download
                                   ? runtime->DownloadStagingBuffer(padded_slice * extent.depth)
                                   : runtime->UploadStagingBuffer(padded_slice * extent.depth);
                // Conversion has no allocation and all byte ranges have been checked above.
                const auto convert = [map, aligned, base, row_stride, slice_stride, padded_slice,
                                      pitch = footprint.Footprint.RowPitch, extent, plane,
                                      format = host_format, download] {
                    auto* guest = map.mapped_span.data() + base;
                    auto* host = aligned.mapped_span.data();
                    for (u32 z = 0; z < extent.depth; ++z) {
                        if (format.conversion == FormatConversion::Etc2 ||
                            format.conversion == FormatConversion::Eac ||
                            format.conversion == FormatConversion::Bcn) {
                            const u32 block_bytes =
                                format.conversion == FormatConversion::Bcn
                                    ? LookupFormat(format.name).bytes_per_block
                                : format.conversion == FormatConversion::Etc2
                                    ? (format.name.find("RGBA") != std::string_view::npos ? 16u
                                                                                          : 8u)
                                    : format.bytes_per_block * 4;
                            for (u32 by = 0; by < (extent.height + 3) / 4; ++by)
                                for (u32 bx = 0; bx < (extent.width + 3) / 4; ++bx) {
                                    alignas(16) std::array<u8, 128> decoded{};
                                    const auto* block = guest + z * slice_stride + by * row_stride +
                                                        bx * block_bytes;
                                    if (format.conversion == FormatConversion::Bcn) {
                                        alignas(16) std::array<u8, 16> aligned_block{};
                                        std::memcpy(aligned_block.data(), block, block_bytes);
                                        const auto* block_input = aligned_block.data();
                                        auto* block_output = decoded.data();
                                        if (format.name.starts_with("BC1"))
                                            bcn::DecodeBc1(block_input, block_output, 0, 0, 4, 4);
                                        else if (format.name.starts_with("BC2"))
                                            bcn::DecodeBc2(block_input, block_output, 0, 0, 4, 4);
                                        else if (format.name.starts_with("BC3"))
                                            bcn::DecodeBc3(block_input, block_output, 0, 0, 4, 4);
                                        else if (format.name.starts_with("BC4"))
                                            bcn::DecodeBc4(block_input, block_output, 0, 0, 4, 4,
                                                           format.name.ends_with("SNORM"));
                                        else if (format.name.starts_with("BC5"))
                                            bcn::DecodeBc5(block_input, block_output, 0, 0, 4, 4,
                                                           format.name.ends_with("SNORM"));
                                        else if (format.name.starts_with("BC6"))
                                            bcn::DecodeBc6(block_input, block_output, 0, 0, 4, 4,
                                                           format.name.ends_with("SFLOAT"));
                                        else
                                            bcn::DecodeBc7(block_input, block_output, 0, 0, 4, 4);
                                    } else
                                        DecodeEtcBlock(format, block, decoded.data());
                                    for (u32 y = 0; y < std::min(4u, extent.height - by * 4); ++y)
                                        std::memcpy(host + z * padded_slice + (by * 4 + y) * pitch +
                                                        bx * 4 * format.bytes_per_block,
                                                    decoded.data() + y * 4 * format.bytes_per_block,
                                                    std::min(4u, extent.width - bx * 4) *
                                                        format.bytes_per_block);
                                }
                        } else
                            for (u32 y = 0; y < extent.height; ++y) {
                                auto* host_row = host + z * padded_slice + y * pitch;
                                auto* guest_row = guest + z * slice_stride + y * row_stride;
                                if (format.depth)
                                    ConvertDepthRow(format.name, host_row, guest_row, extent.width,
                                                    plane, download);
                                else
                                    ConvertPackedRow(format.name, host_row, guest_row, extent.width,
                                                     download);
                            }
                    }
                };
                footprint.Offset = aligned.offset;
                D3D12_TEXTURE_COPY_LOCATION linear{}, texture{};
                linear.pResource = aligned.buffer->handle.Get();
                linear.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                linear.PlacedFootprint = footprint;
                texture.pResource = resource->handle.Get();
                texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                texture.SubresourceIndex =
                    Subresource(info, level, copy.image_subresource.base_layer + layer) +
                    plane * static_cast<UINT>(info.resources.levels * info.resources.layers);
                context.Retain(resource);
                context.Retain(aligned.buffer);
                context.MarkWork("upload/copy");
                if (download) {
                    D3D12_BOX box{static_cast<UINT>(offset.x),
                                  static_cast<UINT>(offset.y),
                                  static_cast<UINT>(offset.z),
                                  static_cast<UINT>(offset.x) + extent.width,
                                  static_cast<UINT>(offset.y) + extent.height,
                                  static_cast<UINT>(offset.z) + extent.depth};
                    context.BeforeCopyWrite(aligned.buffer, footprint.Offset,
                                            padded_slice * extent.depth);
                    context.Commands()->CopyTextureRegion(&linear, 0, 0, 0, &texture,
                                                          host_format.depth ? nullptr : &box);
                    context.AfterCompletion(convert);
                } else {
                    convert();
                    context.Profile().Count(FrameCount::StagingCopyBytes,
                                            padded_slice * extent.depth);
                    context.BeforeCopyWrite(resource, texture.SubresourceIndex, 1);
                    context.Commands()->CopyTextureRegion(&texture, offset.x, offset.y, offset.z,
                                                          &linear, nullptr);
                }
            }
        }
    }
    return true;
}
ImageView::ImageView(TextureCacheRuntime&, const VideoCommon::ImageViewInfo& info,
                     VideoCommon::ImageId id, Image& image)
    : ImageViewBase{info, image.info, id, image.gpu_addr}, is_rescaled{image.IsRescaled()},
      sample_count{image.info.num_samples}, image_type{image.info.type},
      host_format{image.HostFormat()}, resource{image.Handle()} {
    astc_bc_diag = image.AstcBcDiagnosticEnabled();
    if (image.AstcBcDiagnosticEnabled())
        image.TraceAstcBc(
            "view",
            fmt::format("view_format={} view_type={} level={} levels={} layer={} layers={} rt={}",
                        MaxwellFormat(info.format).name, static_cast<u32>(type), range.base.level,
                        range.extent.levels, range.base.layer, range.extent.layers,
                        info.IsRenderTarget()));
    if (info.IsRenderTarget())
        image.NoteAstcBcUse(true);
    if (!resource)
        return;
    resource->Description();
    auto view_info = MaxwellFormat(info.format);
    const bool decoded_bcn = VideoCore::Surface::IsPixelFormatBCn(image.info.format) &&
                             image.HostFormat().block_width == 1;
    if (decoded_bcn && info.format == image.info.format)
        view_info = image.HostFormat();
    if (image.HostFormat().conversion != FormatConversion::None &&
        info.format != image.info.format) {
        UnsupportedImage(
            image.info, image.HostFormat(),
            "converted storage cannot alias a different guest format without repacking");
        resource.reset();
        return;
    }
    if (!view_info.Supported() ||
        (!VideoCore::Surface::IsPixelFormatASTC(info.format) &&
         !resource->SupportsViewFormat(static_cast<DXGI_FORMAT>(view_info.view_format)))) {
        UnsupportedImage(image.info, image.HostFormat(), "incompatible texture view cast");
        resource.reset();
        return;
    }
    description.Format = static_cast<DXGI_FORMAT>(VideoCore::Surface::IsPixelFormatASTC(info.format)
                                                      ? image.HostFormat().view_format
                                                      : view_info.view_format);
    const auto sources =
        info.IsRenderTarget()
            ? std::array{Tegra::Texture::SwizzleSource::R, Tegra::Texture::SwizzleSource::G,
                         Tegra::Texture::SwizzleSource::B, Tegra::Texture::SwizzleSource::A}
            : info.Swizzle();
    UINT mapping[4]{};
    for (size_t i = 0; i < 4; ++i) {
        const auto source = sources[i];
        const auto value = static_cast<UINT>(source);
        if (value == 1 || value > 7) {
            UnsupportedImage(image.info, image.HostFormat(),
                             fmt::format("invalid component swizzle selector {}", value));
            resource.reset();
            return;
        }
        mapping[i] = FormatComponent(view_info, value);
    }
    description.Shader4ComponentMapping =
        D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(mapping[0], mapping[1], mapping[2], mapping[3]);
    UINT view_plane{};
    if (host_format.depth) {
        const bool any_red =
            std::ranges::find(sources, Tegra::Texture::SwizzleSource::R) != sources.end();
        view_plane = DepthViewPlane(host_format, any_red, info.IsRenderTarget());
        if (view_plane)
            description.Format = host_format.resource_format == 19
                                     ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT
                                     : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        for (size_t i = 0; i < 4; ++i) {
            if (mapping[i] < 2)
                mapping[i] = view_plane ? 1u : 0u;
        }
        description.Shader4ComponentMapping =
            D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(mapping[0], mapping[1], mapping[2], mapping[3]);
    }
    const auto desc = resource->handle->GetDesc();
    if (range.base.level < 0 || range.extent.levels <= 0 ||
        range.base.level + range.extent.levels > desc.MipLevels || range.base.layer < 0 ||
        range.extent.layers <= 0 ||
        (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
         range.base.layer + range.extent.layers > desc.DepthOrArraySize)) {
        UnsupportedImage(image.info, image.HostFormat(),
                         "image view subresources outside resource");
        resource.reset();
        return;
    }
    const UINT level = range.base.level;
    const UINT levels = range.extent.levels;
    const UINT first_layer = range.base.layer;
    const UINT layers = range.extent.layers;
    if (desc.SampleDesc.Count > 1) {
        if (type != VideoCommon::ImageViewType::e2D &&
            type != VideoCommon::ImageViewType::e2DArray &&
            type != VideoCommon::ImageViewType::Rect) {
            UnsupportedImage(image.info, image.HostFormat(), "MSAA view must be 2D or 2D array");
            resource.reset();
            return;
        }
        if (desc.DepthOrArraySize == 1 && type != VideoCommon::ImageViewType::e2DArray) {
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            description.Texture2DMSArray = {first_layer, layers};
        }
        return;
    }
    switch (type) {
    case VideoCommon::ImageViewType::e1D:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        description.Texture1D = {level, levels, 0};
        break;
    case VideoCommon::ImageViewType::e1DArray:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        description.Texture1DArray = {level, levels, first_layer, layers, 0};
        break;
    case VideoCommon::ImageViewType::e2D:
    case VideoCommon::ImageViewType::Rect:
        if (resource->handle->GetDesc().DepthOrArraySize == 1) {
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            description.Texture2D = {level, levels, view_plane, 0};
            break;
        }
        [[fallthrough]];
    case VideoCommon::ImageViewType::e2DArray:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        description.Texture2DArray = {level, levels, first_layer, layers, view_plane, 0};
        break;
    case VideoCommon::ImageViewType::Cube:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        description.TextureCube = {level, levels, 0};
        break;
    case VideoCommon::ImageViewType::CubeArray:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        description.TextureCubeArray = {level, levels, first_layer, layers / 6, 0};
        break;
    case VideoCommon::ImageViewType::e3D:
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        description.Texture3D = {level, levels, 0};
        break;
    default:
        UnsupportedImage(image.info, image.HostFormat(), "unsupported texture view type");
        resource.reset();
        return;
    }
}
ImageView::ImageView(TextureCacheRuntime&, const VideoCommon::NullImageViewParams& params)
    : ImageViewBase{params} {
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2D.MipLevels = 1;
}
ImageView::ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo& info,
                     const VideoCommon::ImageViewInfo& view, GPUVAddr addr)
    : ImageViewBase{info, view, addr}, buffer_size{VideoCommon::CalculateGuestSizeInBytes(info)},
      sample_count{info.num_samples}, image_type{info.type},
      host_format{MaxwellFormat(info.format)} {}
void ImageView::NoteAstcBcUse(bool render_target) const {
    if (!astc_bc_diag)
        return;
    bool& seen = render_target ? astc_bc_rt_bound : astc_bc_copy_dst;
    if (seen)
        return;
    seen = true;
    Diagnostic(fmt::format("ASTC_BC event={} gpu={:x} view_format={} host={} level={} layers={} "
                           "rt_bound={} copy_dst={}",
                           render_target ? "rt_bind" : "view_copy_dst", gpu_addr,
                           MaxwellFormat(format).name, host_format.name, range.base.level,
                           range.extent.layers, astc_bc_rt_bound, astc_bc_copy_dst));
}
void ImageView::LogUnsupported(std::string_view condition) const {
    VideoCommon::ImageInfo info;
    info.format = format;
    info.num_samples = sample_count;
    info.type = image_type;
    UnsupportedImage(info, HostFormat(), condition);
}
bool TextureCacheRuntime::CanPresentImage(const Image& image,
                                          VideoCore::Surface::PixelFormat format) const {
    using VideoCore::Surface::PixelFormat;
    // These pairs have identical guest bytes and component order. Sampling the
    // UNORM view preserves the fallback's transfer function (no sRGB decoding).
    // Do not use Maxwell's broad 32-bit class: BGRA/RGBA and float need repacking.
    const bool family =
        image.info.format == format ||
        (format == PixelFormat::A8B8G8R8_UNORM &&
         image.info.format == PixelFormat::A8B8G8R8_SRGB) ||
        (format == PixelFormat::B8G8R8A8_UNORM && image.info.format == PixelFormat::B8G8R8A8_SRGB);
    const auto resource = image.Handle();
    const auto host = image.HostFormat();
    const auto view = MaxwellFormat(format);
    return family && resource && host.conversion == FormatConversion::None && !host.depth &&
           view.Supported() &&
           resource->SupportsViewFormat(static_cast<DXGI_FORMAT>(view.view_format));
}
bool TextureCacheRuntime::CanViewFormat(const ImageView& view,
                                        VideoCore::Surface::PixelFormat format) const {
    if (view.IsBuffer())
        return true;
    const auto resource = view.ImageHandle();
    if (!resource || !(resource->Description().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return false;
    const auto host = MaxwellFormat(format);
    return host.Supported() &&
           (format == view.format || (view.HostFormat().conversion == FormatConversion::None &&
                                      !view.HostFormat().converted_astc &&
                                      !VideoCore::Surface::IsPixelFormatASTC(view.format))) &&
           resource->SupportsViewFormat(static_cast<DXGI_FORMAT>(host.view_format));
}
void TextureCacheRuntime::TransitionImageLayout(Image& image) {
    if (image.Handle())
        context.Transition(image.Handle(), D3D12_RESOURCE_STATE_COMMON);
}
ResourcePtr TextureCacheRuntime::ResolveFramebuffer(ImageView& view) {
    const auto source = view.ImageHandle();
    if (!source)
        return {};
    const auto format = view.HostFormat();
    if (format.depth) {
        view.LogUnsupported("depth/stencil image cannot be presented as color");
        return {};
    }
    const auto desc = source->handle->GetDesc();
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = view.format;
    info.size = {static_cast<u32>(desc.Width >> view.range.base.level),
                 desc.Height >> view.range.base.level, 1};
    auto target = MakeTexture(context, info, format);
    if (!target)
        return {};
    const VideoCommon::Region2D region{
        {0, 0}, {static_cast<s32>(info.size.width), static_cast<s32>(info.size.height)}};
    try {
        BlitHelpers().Resolve({target, format},
                              {source, format, static_cast<u32>(view.range.base.level),
                               static_cast<u32>(view.range.base.layer)},
                              region, region);
    } catch (const std::logic_error& error) {
        view.LogUnsupported(error.what());
        return {};
    }
    return target;
}
bool TextureCacheRuntime::CanDownloadImage(const Image& image) const {
    const auto format = image.HostFormat();
    if (!image.Handle())
        return false;
    if (format.converted_astc || VideoCore::Surface::IsPixelFormatASTC(image.info.format)) {
        UnsupportedImage(image.info, format, "converted ASTC readback is not reversible");
        return false;
    }
    if (VideoCore::Surface::IsPixelFormatBCn(image.info.format) && format.block_width == 1) {
        UnsupportedImage(
            image.info, format,
            "decoded BCn readback needs a lossy encoder; original guest blocks retained");
        return false;
    }
    if (format.conversion == FormatConversion::Etc2 || format.conversion == FormatConversion::Eac) {
        UnsupportedImage(image.info, format,
                         "ETC2/EAC readback needs a lossy encoder; original guest blocks retained");
        return false;
    }
    return true;
}
ComputePasses& TextureCacheRuntime::ComputeHelpers() {
    if (!compute_passes)
        compute_passes = std::make_unique<ComputePasses>(context);
    return *compute_passes;
}
void TextureCacheRuntime::AccelerateImageUpload(
    Image& image, const StagingBufferRef& map,
    std::span<const VideoCommon::SwizzleParameters> swizzles, u32, u32) {
    if (!VideoCore::Surface::IsPixelFormatASTC(image.info.format))
        throw std::runtime_error("D3D12 accelerated upload currently requires ASTC");
    auto& helpers = ComputeHelpers();
    for (const auto& swizzle : swizzles) {
        const auto params =
            VideoCommon::Accelerated::MakeBlockLinearSwizzle2DParams(swizzle, image.info);
        const u64 size = image.guest_size_bytes - swizzle.buffer_offset;
        auto input = helpers.UploadInput(map.buffer, map.offset + swizzle.buffer_offset, size);
        const std::array buffers{ComputeBufferView{input, 0, size}};
        ComputeTextureView output;
        output.resource = image.Handle();
        output.descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        output.descriptor.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        output.descriptor.Texture2DArray = {static_cast<UINT>(swizzle.level), 0,
                                            static_cast<UINT>(image.info.resources.layers), 0};
        const AstcDecodeParameters constants{
            VideoCore::Surface::DefaultBlockWidth(image.info.format),
            VideoCore::Surface::DefaultBlockHeight(image.info.format),
            params.layer_stride,
            params.block_size,
            params.x_shift,
            params.block_height,
            params.block_height_mask};
        helpers.Run(HelperComputePass::DecodeAstc, constants.Constants(),
                    {swizzle.num_tiles.width, swizzle.num_tiles.height,
                     static_cast<u32>(image.info.resources.layers)},
                    buffers, output);
    }
}
bool TextureCacheRuntime::CanDiscardBlitDestination(
    const Image& image, const Tegra::Engines::Fermi2D::Surface& surface,
    const Tegra::Engines::Fermi2D::Config& copy) const {
    // The caller excludes self-copies and aliases. Discard only a complete
    // single-subresource destination; partial blits still preserve guest bytes.
    return image.gpu_addr == surface.Address() &&
           CoversSingleSubresource(copy.dst_x0, copy.dst_y0, copy.dst_x1, copy.dst_y1,
                                   image.info.size.width, image.info.size.height,
                                   image.info.size.depth, image.info.resources.levels,
                                   image.info.resources.layers);
}
void TextureCacheRuntime::RecordBlitDiscard(const Image& image) {
    if (True(image.flags & VideoCommon::ImageFlagBits::CpuModified))
        context.Profile().Count(FrameCount::FermiDiscardUploadBytes, image.unswizzled_size_bytes);
}
void TextureCacheRuntime::RecordTextureWrite(Image& image) {
    image.upload_reason = Tegra::Engines::Fermi2D::IsSoftwareBlitActive()
                              ? "fermi_software_write"
                              : "cpu_write_notification";
}
void TextureCacheRuntime::RecordTextureUpload(Image& image) {
    context.Profile().Count(FrameCount::TextureRefreshBytes, image.unswizzled_size_bytes);
    static const bool trace = BisectEnabled("NXBOX_D3D12_TEXTURE_UPLOAD_TRACE") ||
                              std::getenv("NXBOX_D3D12_UPLOAD_DIMS") != nullptr;
    if (!trace && !context.Profile().Enabled())
        return;
    const FrameProfile::Scope profile_diagnostic{context.Profile(),
                                                 FramePhase::TextureUploadDiagnostic};
    if (image.upload_reason == "initial_or_recreated") {
        for (const auto& write : software_writes) {
            if (write.address == image.gpu_addr && write.valid && frame_tick - write.frame <= 1) {
                image.upload_reason = "initial_after_fermi_software";
                break;
            }
        }
    }
    // NXBOX_D3D12_UPLOAD_DIMS=WxH logs every upload of images with that size.
    static const auto watched_dims = [] {
        const char* value = std::getenv("NXBOX_D3D12_UPLOAD_DIMS");
        u32 width{}, height{};
        if (value == nullptr || std::sscanf(value, "%ux%u", &width, &height) != 2)
            return std::pair<u32, u32>{};
        return std::pair<u32, u32>{width, height};
    }();
    if (watched_dims.first == image.info.size.width &&
        watched_dims.second == image.info.size.height) {
        Diagnostic(fmt::format("D3D12_UPLOAD_WATCH gpu_address={:#x} frame={} reason={} "
                               "gpu_modified={} layers={} levels={}",
                               image.gpu_addr, frame_tick, image.upload_reason,
                               True(image.flags & VideoCommon::ImageFlagBits::GpuModified),
                               image.info.resources.layers, image.info.resources.levels));
    }
    repeated_uploads.Add({image.gpu_addr, image.unswizzled_size_bytes, 0,
                          static_cast<u32>(image.info.format), image.info.size.width,
                          image.info.size.height, image.upload_reason,
                          True(image.flags & VideoCommon::ImageFlagBits::GpuModified)});
    // RefreshContents clears CpuModified after this hook. If another invalidation
    // source does not supply a reason, do not reuse the previous write's reason.
    image.upload_reason = "cache_refresh_or_remap";
    const auto now = std::chrono::steady_clock::now();
    if (now - upload_log_time < std::chrono::seconds{5})
        return;
    const auto entry = repeated_uploads.LargestRepeated();
    if (entry.count) {
        Diagnostic(fmt::format("D3D12_TEXTURE_UPLOAD_REPEAT gpu_address={:#x} bytes={} format={} "
                               "width={} height={} uploads={} reason={} gpu_modified={}",
                               entry.address, entry.size, entry.format, entry.width, entry.height,
                               entry.count, entry.reason, entry.gpu_modified));
    }
    repeated_uploads.Reset();
    upload_log_time = now;
}
bool TextureCacheRuntime::CanBlitImages(const Image& dst, const Image& src,
                                        const Tegra::Engines::Fermi2D::Surface& dst_surface,
                                        const Tegra::Engines::Fermi2D::Surface& src_surface,
                                        const Tegra::Engines::Fermi2D::Config& copy) const {
    dst.TraceAstcBc("copy_dst_candidate");
    const auto reject = [this](const char* reason) {
        blit_reject = reason;
        return false;
    };
    if (!dst.Handle() || !src.Handle())
        return reject("missing image handle");
    // Games blit depth buffers through color Fermi surfaces (Scarlet downsamples its depth
    // this way); both cached images must then be depth with the same aspects.
    const bool depth = dst.HostFormat().depth || src.HostFormat().depth;
    if (depth && (!dst.HostFormat().depth || !src.HostFormat().depth ||
                  dst.HostFormat().planes != src.HostFormat().planes))
        return reject("depth blit between images with different aspects");
    // DepthStencil conversion only repacks guest bytes on CPU transfers; two depth images in
    // the same host format hold native data that BlitDepthStencil reads and writes directly.
    const auto native = [](const FormatInfo& format) {
        return format.conversion == FormatConversion::None ||
               (format.depth && format.conversion == FormatConversion::DepthStencil);
    };
    if (!native(dst.HostFormat()) || !native(src.HostFormat()) || dst.HostFormat().converted_astc ||
        src.HostFormat().converted_astc ||
        (depth && dst.HostFormat().resource_format != src.HostFormat().resource_format))
        return reject("cached image format is converted");
    if (dst.info.num_samples != 1 || src.info.num_samples != 1)
        return reject("multisampled image");
    if (copy.operation != Tegra::Engines::Fermi2D::Operation::SrcCopy)
        return reject("operation is not SrcCopy");
    // Use each actual cached mip, which need not match the surface's dimensions.
    const auto source_base = src.TryFindBase(src_surface.Address());
    const auto target_base = dst.TryFindBase(dst_surface.Address());
    if (!source_base || !target_base)
        return reject("surface address is not a subresource base of the cached image");
    const BlitSurface source{src.OriginalHandle(), src.HostFormat(),
                             static_cast<u32>(source_base->level),
                             static_cast<u32>(source_base->layer)};
    const BlitSurface target{dst.OriginalHandle(), dst.HostFormat(),
                             static_cast<u32>(target_base->level),
                             static_cast<u32>(target_base->layer)};
    const auto fits = [](const BlitSurface& surface, s32 x, s32 y) {
        const auto desc = surface.resource->handle->GetDesc();
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               surface.level < u32{desc.MipLevels} && surface.layer < u32{desc.DepthOrArraySize} &&
               static_cast<u64>(x) <= std::max(u64{1}, desc.Width >> surface.level) &&
               static_cast<u32>(y) <= std::max(1u, desc.Height >> surface.level);
    };
    if (!fits(source, copy.src_x1, copy.src_y1))
        return reject("source region exceeds the cached image");
    if (!fits(target, copy.dst_x1, copy.dst_y1))
        return reject("destination region exceeds the cached image");
    // BlitImage draws depth through BlitDepthStencil, which samples point and writes both planes.
    if (depth)
        return true;
    // Exact same-format copies keep bits; anything else (scaling, format change) is drawn.
    if (dst.info.format == src.info.format &&
        IsExactBlit(copy.src_x0, copy.src_y0, copy.src_x1, copy.src_y1, copy.dst_x0, copy.dst_y0,
                    copy.dst_x1, copy.dst_y1) &&
        BlitImageHelper::CopyCompatible(target, source))
        return true;
    if (!BlitImageHelper::DrawCompatible(target, source,
                                         copy.filter == Tegra::Engines::Fermi2D::Filter::Bilinear))
        return reject("formats cannot be drawn into each other");
    return true;
}
void TextureCacheRuntime::BlitImage(Framebuffer*, ImageView& dst, ImageView& src,
                                    const VideoCommon::Region2D& dst_region,
                                    const VideoCommon::Region2D& src_region,
                                    Tegra::Engines::Fermi2D::Filter filter,
                                    Tegra::Engines::Fermi2D::Operation operation) {
    dst.NoteAstcBcUse(false);
    if (!dst.ImageHandle() || !src.ImageHandle())
        return;
    const BlitSurface source{src.ImageHandle(), src.HostFormat(),
                             static_cast<u32>(src.range.base.level),
                             static_cast<u32>(src.range.base.layer)};
    const BlitSurface target{dst.ImageHandle(), dst.HostFormat(),
                             static_cast<u32>(dst.range.base.level),
                             static_cast<u32>(dst.range.base.layer)};
    // CanBlitImages preflights the Fermi path before either image is marked
    // modified. Exact native copies do not sample, blend, clamp or convert bits.
    if (operation == Tegra::Engines::Fermi2D::Operation::SrcCopy &&
        IsExactBlit(src_region.start.x, src_region.start.y, src_region.end.x, src_region.end.y,
                    dst_region.start.x, dst_region.start.y, dst_region.end.x, dst_region.end.y) &&
        !source.format.depth && !target.format.depth &&
        source.resource->handle->GetDesc().SampleDesc.Count == 1 &&
        BlitImageHelper::CopyCompatible(target, source)) {
        VideoCommon::ImageCopy copy{};
        const auto layers = std::min(src.range.extent.layers, dst.range.extent.layers);
        copy.src_subresource = {static_cast<s32>(source.level), static_cast<s32>(source.layer),
                                layers};
        copy.dst_subresource = {static_cast<s32>(target.level), static_cast<s32>(target.layer),
                                layers};
        copy.src_offset = {src_region.start.x, src_region.start.y, 0};
        copy.dst_offset = {dst_region.start.x, dst_region.start.y, 0};
        copy.extent = {static_cast<u32>(src_region.end.x - src_region.start.x),
                       static_cast<u32>(src_region.end.y - src_region.start.y), 1};
        BlitHelpers().CopyImage(target, source, std::span{&copy, 1});
        return;
    }
    try {
        const u32 layers = std::min(src.range.extent.layers, dst.range.extent.layers);
        for (u32 layer = 0; layer < layers; ++layer) {
            auto s = source, d = target;
            s.layer += layer;
            d.layer += layer;
            if (source.resource->handle->GetDesc().SampleDesc.Count > 1 &&
                target.resource->handle->GetDesc().SampleDesc.Count == 1 &&
                operation == Tegra::Engines::Fermi2D::Operation::SrcCopy)
                BlitHelpers().Resolve(d, s, dst_region, src_region);
            else if (source.format.depth && target.format.depth &&
                     operation == Tegra::Engines::Fermi2D::Operation::SrcCopy)
                BlitHelpers().BlitDepthStencil(d, s, dst_region, src_region);
            else
                BlitHelpers().BlitColor(d, s, dst_region, src_region, filter, operation);
        }
    } catch (const std::logic_error& error) {
        src.LogUnsupported(error.what());
    }
}
BlitImageHelper& TextureCacheRuntime::BlitHelpers() {
    if (!blit_helper)
        blit_helper = std::make_unique<BlitImageHelper>(context);
    return *blit_helper;
}
void TextureCacheRuntime::CopyImageMSAA(Image& dst, Image& src,
                                        std::span<const VideoCommon::ImageCopy> copies) {
    dst.NoteAstcBcUse(false);
    if (!dst.Handle() || !src.Handle())
        return;
    try {
        BlitHelpers().CopyImageMSAA({dst.Handle(), dst.HostFormat()},
                                    {src.Handle(), src.HostFormat()}, copies);
    } catch (const std::logic_error& error) {
        UnsupportedImage(src.info, src.HostFormat(), error.what());
    }
}
void TextureCacheRuntime::ConvertImage(Framebuffer*, ImageView& dst, ImageView& src) {
    dst.NoteAstcBcUse(false);
    if (!dst.ImageHandle() || !src.ImageHandle())
        return;
    VideoCommon::ImageCopy copy{};
    copy.src_subresource = {src.range.base.level, src.range.base.layer, src.range.extent.layers};
    copy.dst_subresource = {dst.range.base.level, dst.range.base.layer, dst.range.extent.layers};
    const auto desc = dst.ImageHandle()->handle->GetDesc();
    copy.extent = {static_cast<u32>(desc.Width >> dst.range.base.level),
                   desc.Height >> dst.range.base.level, 1};
    try {
        BlitHelpers().CopyImage({dst.ImageHandle(), dst.HostFormat()},
                                {src.ImageHandle(), src.HostFormat()}, std::span{&copy, 1});
    } catch (const std::logic_error& error) {
        src.LogUnsupported(error.what());
    }
}
// Equal-size color blocks with no host conversion have the same bytes on both sides: bounce
// them through a GPU buffer with each side's footprint. The CPU path waited on the GPU for every
// such copy (~10 per frame in BotW).
bool TextureCacheRuntime::ReinterpretOnGpu(Image& dst, Image& src,
                                           std::span<const VideoCommon::ImageCopy> copies) {
    const auto& from = src.HostFormat();
    const auto& to = dst.HostFormat();
    // Block-compressed copies need 4x4-aligned boxes that small mips do not have; keep those on
    // the CPU path, which handles partial blocks.
    if (&dst == &src || BisectEnabled("NXBOX_D3D12_CPU_REINTERPRET") || from.block_width != 1 ||
        from.block_height != 1 || to.block_width != 1 || to.block_height != 1 ||
        from.conversion != FormatConversion::None ||
        to.conversion != FormatConversion::None || from.depth || to.depth || from.planes > 1 ||
        to.planes > 1 || from.bytes_per_block != to.bytes_per_block ||
        src.info.num_samples != 1 || dst.info.num_samples != 1)
        return false;
    const auto src_format = src.Handle()->handle->GetDesc().Format;
    const auto dst_format = dst.Handle()->handle->GetDesc().Format;
    struct Step {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT read, write;
        UINT src_subresource, dst_subresource;
        D3D12_BOX src_box, dst_box;
        UINT x, y, z;
    };
    std::vector<Step> steps;
    u64 total = 0;
    for (const auto& copy : copies) {
        const auto& extent = copy.extent;
        if (!extent.width || !extent.height || !extent.depth || copy.src_offset.x < 0 ||
            copy.src_offset.y < 0 || copy.src_offset.z < 0 || copy.dst_offset.x < 0 ||
            copy.dst_offset.y < 0 || copy.dst_offset.z < 0 ||
            copy.src_subresource.num_layers != copy.dst_subresource.num_layers)
            return false;
        const s32 src_level = copy.src_subresource.base_level;
        const u32 src_depth = src.info.type == VideoCommon::ImageType::e3D
                                  ? std::max(1u, src.info.size.depth >> src_level)
                                  : 1u;
        if (u64{static_cast<u32>(copy.src_offset.x)} + extent.width >
                std::max(1u, src.info.size.width >> src_level) ||
            u64{static_cast<u32>(copy.src_offset.y)} + extent.height >
                std::max(1u, src.info.size.height >> src_level) ||
            u64{static_cast<u32>(copy.src_offset.z)} + extent.depth > src_depth)
            return false;
        const u32 columns = (extent.width + from.block_width - 1) / from.block_width;
        const u32 rows = (extent.height + from.block_height - 1) / from.block_height;
        const s32 level = copy.dst_subresource.base_level;
        const u32 mip_width = std::max(1u, dst.info.size.width >> level);
        const u32 mip_height = std::max(1u, dst.info.size.height >> level);
        const auto dx = static_cast<u32>(copy.dst_offset.x);
        const auto dy = static_cast<u32>(copy.dst_offset.y);
        const u32 dst_depth = dst.info.type == VideoCommon::ImageType::e3D
                                  ? std::max(1u, dst.info.size.depth >> level)
                                  : 1u;
        if (dx >= mip_width || dy >= mip_height ||
            u64{static_cast<u32>(copy.dst_offset.z)} + extent.depth > dst_depth)
            return false;
        const u32 width = std::min(columns * to.block_width, mip_width - dx);
        const u32 height = std::min(rows * to.block_height, mip_height - dy);
        if ((width + to.block_width - 1) / to.block_width != columns ||
            (height + to.block_height - 1) / to.block_height != rows)
            return false;
        const u32 pitch = Common::AlignUp(columns * from.bytes_per_block,
                                          u32{D3D12_TEXTURE_DATA_PITCH_ALIGNMENT});
        const u64 size = u64{pitch} * rows * extent.depth;
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            Step step{};
            const u64 offset =
                Common::AlignUp(total, u64{D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT});
            step.read = {offset,
                         {src_format, Common::AlignUp(extent.width, from.block_width),
                          Common::AlignUp(extent.height, from.block_height), extent.depth, pitch}};
            step.write = {offset,
                          {dst_format, Common::AlignUp(width, to.block_width),
                           Common::AlignUp(height, to.block_height), extent.depth, pitch}};
            step.src_subresource = Subresource(src.info, copy.src_subresource.base_level,
                                               copy.src_subresource.base_layer + layer);
            step.dst_subresource = Subresource(dst.info, level,
                                               copy.dst_subresource.base_layer + layer);
            const auto sx = static_cast<UINT>(copy.src_offset.x);
            const auto sy = static_cast<UINT>(copy.src_offset.y);
            const auto sz = static_cast<UINT>(copy.src_offset.z);
            step.src_box = {sx, sy, sz, sx + extent.width, sy + extent.height, sz + extent.depth};
            step.dst_box = {0, 0, 0, width, height, extent.depth};
            step.x = dx;
            step.y = dy;
            step.z = static_cast<UINT>(copy.dst_offset.z);
            steps.push_back(step);
            total = offset + size;
        }
    }
    if (steps.empty())
        return true;
    auto bounce = context.CreateBuffer(total, D3D12_HEAP_TYPE_DEFAULT);
    const auto location = [](ID3D12Resource* resource, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp,
                             UINT subresource) {
        D3D12_TEXTURE_COPY_LOCATION result{};
        result.pResource = resource;
        if (fp) {
            result.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            result.PlacedFootprint = *fp;
        } else {
            result.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            result.SubresourceIndex = subresource;
        }
        return result;
    };
    context.Retain(bounce);
    context.Retain(src.Handle());
    context.Retain(dst.Handle());
    context.MarkWork("upload/copy");
    context.Transition(src.Handle(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.Transition(bounce, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& step : steps) {
        const auto out = location(bounce->handle.Get(), &step.read, 0);
        const auto in = location(src.Handle()->handle.Get(), nullptr, step.src_subresource);
        context.BeforeCopyWrite(bounce, step.read.Offset,
                                u64{step.read.Footprint.RowPitch} * step.read.Footprint.Height *
                                    step.read.Footprint.Depth);
        context.Commands()->CopyTextureRegion(&out, 0, 0, 0, &in, &step.src_box);
    }
    context.Transition(bounce, D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.Transition(dst.Handle(), D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& step : steps) {
        const auto out = location(dst.Handle()->handle.Get(), nullptr, step.dst_subresource);
        const auto in = location(bounce->handle.Get(), &step.write, 0);
        context.BeforeCopyWrite(dst.Handle(), step.dst_subresource, 1);
        context.Commands()->CopyTextureRegion(&out, step.x, step.y, step.z, &in, &step.dst_box);
    }
    return true;
}
// Depth/color aliases need guest bit packing, not a numeric color blit. The existing
// raw CopyImage shaders implement exactly these layouts; keep other packed formats
// on the reversible CPU path. Preflight the entire batch before recording any write.
bool TextureCacheRuntime::ReinterpretDepthOnGpu(Image& dst, Image& src,
                                                std::span<const VideoCommon::ImageCopy> copies) {
    static const bool enabled = [] {
        char value[2]{};
        return !(GetEnvironmentVariableA("NXBOX_D3D12_GPU_DEPTH_REINTERPRET", value,
                                         sizeof(value)) == 1 &&
                 value[0] == '0');
    }();
    if (!enabled || BisectEnabled("NXBOX_D3D12_CPU_REINTERPRET") || &dst == &src ||
        copies.empty() || !src.Handle() || !dst.Handle() || src.Handle() == dst.Handle() ||
        src.IsRescaled() || dst.IsRescaled() || src.info.type != VideoCommon::ImageType::e2D ||
        dst.info.type != VideoCommon::ImageType::e2D || src.info.num_samples != 1 ||
        dst.info.num_samples != 1)
        return false;
    const auto depth = [](const FormatInfo& format) {
        return format.name == "D24_UNORM_S8_UINT" || format.name == "S8_UINT_D24_UNORM";
    };
    const auto color = [](const FormatInfo& format) {
        return format.name == "A8B8G8R8_UNORM" || format.name == "A8B8G8R8_SRGB";
    };
    if (!((depth(src.HostFormat()) && color(dst.HostFormat())) ||
          (color(src.HostFormat()) && depth(dst.HostFormat()))))
        return false;
    const auto fits = [](const Image& image, const VideoCommon::SubresourceLayers& subresource,
                         const VideoCommon::Offset3D& offset, const VideoCommon::Extent3D& extent) {
        if (subresource.base_level < 0 || subresource.base_level >= image.info.resources.levels ||
            subresource.base_layer < 0 || subresource.num_layers <= 0 ||
            subresource.num_layers > image.info.resources.layers ||
            subresource.base_layer > image.info.resources.layers - subresource.num_layers ||
            offset.x < 0 || offset.y < 0 || offset.z != 0 || !extent.width || !extent.height ||
            extent.depth != 1)
            return false;
        return u64{static_cast<u32>(offset.x)} + extent.width <=
                   std::max(1u, image.info.size.width >> subresource.base_level) &&
               u64{static_cast<u32>(offset.y)} + extent.height <=
                   std::max(1u, image.info.size.height >> subresource.base_level);
    };
    u64 bytes{};
    for (const auto& copy : copies) {
        if (copy.src_subresource.num_layers != copy.dst_subresource.num_layers ||
            !fits(src, copy.src_subresource, copy.src_offset, copy.extent) ||
            !fits(dst, copy.dst_subresource, copy.dst_offset, copy.extent))
            return false;
        bytes += u64{copy.extent.width} * copy.extent.height *
                 static_cast<u32>(copy.src_subresource.num_layers) * 4;
    }
    BlitHelpers().CopyImage({dst.Handle(), dst.HostFormat()}, {src.Handle(), src.HostFormat()},
                            copies);
    context.Profile().Count(FrameCount::GpuDepthReinterpretBytes, bytes);
    return true;
}
bool TextureCacheRuntime::ShouldReinterpret(Image& dst, Image& src) {
    return dst.HostFormat().resource_format != src.HostFormat().resource_format ||
           (dst.info.format != src.info.format &&
            (dst.HostFormat().conversion != FormatConversion::None ||
             src.HostFormat().conversion != FormatConversion::None));
}
void TextureCacheRuntime::ReinterpretImage(Image& dst, Image& src,
                                           std::span<const VideoCommon::ImageCopy> copies) {
    dst.NoteAstcBcUse(false);
    if (!dst.Handle() || !src.Handle())
        return;
    if (!ShouldReinterpret(dst, src)) {
        CopyImage(dst, src, copies);
        return;
    }
    const bool native_raw_alias = src.info.num_samples == 1 && dst.info.num_samples == 1 &&
                                  VideoCore::Surface::BytesPerBlock(src.info.format) ==
                                      VideoCore::Surface::BytesPerBlock(dst.info.format);
    if (ReinterpretDepthOnGpu(dst, src, copies))
        return;
    if (native_raw_alias && ReinterpretOnGpu(dst, src, copies))
        return;
    if (native_raw_alias || dst.HostFormat().conversion != FormatConversion::None ||
        src.HostFormat().conversion != FormatConversion::None) {
        // A canonical host pixel is not the guest bit pattern. Reinterpret via
        // guest packing instead of copying RGBA bytes between emulated formats.
        const auto reject = [&](std::string_view reason) {
            UnsupportedImage(src.info, src.HostFormat(), reason);
        };
        const u32 bytes = VideoCore::Surface::BytesPerBlock(src.info.format);
        if (!CanDownloadImage(src) || bytes != VideoCore::Surface::BytesPerBlock(dst.info.format) ||
            src.info.num_samples != 1 || dst.info.num_samples != 1) {
            reject("converted reinterpretation requires reversible, equal-size guest blocks and "
                   "one sample");
            return;
        }
        try {
            std::vector<VideoCommon::BufferImageCopy> downloads, uploads;
            downloads.reserve(copies.size());
            uploads.reserve(copies.size());
            u64 total{};
            for (const auto& copy : copies) {
                if (copy.src_subresource.num_layers <= 0 ||
                    copy.src_subresource.num_layers != copy.dst_subresource.num_layers ||
                    !copy.extent.width || !copy.extent.height || !copy.extent.depth) {
                    reject("invalid converted reinterpretation layers/extent");
                    return;
                }
                const auto full_depth = [&](const Image& image,
                                            const VideoCommon::SubresourceLayers& subresource,
                                            const VideoCommon::Offset3D& offset) {
                    const auto level = subresource.base_level;
                    return !image.HostFormat().depth ||
                           (level >= 0 && level < image.info.resources.levels && offset.x == 0 &&
                            offset.y == 0 && offset.z == 0 &&
                            copy.extent.width == std::max(1u, image.info.size.width >> level) &&
                            copy.extent.height == std::max(1u, image.info.size.height >> level) &&
                            copy.extent.depth == 1);
                };
                if (!full_depth(src, copy.src_subresource, copy.src_offset) ||
                    !full_depth(dst, copy.dst_subresource, copy.dst_offset)) {
                    reject("partial converted depth reinterpretation requires shader conversion");
                    return;
                }
                const u32 src_bw = VideoCore::Surface::DefaultBlockWidth(src.info.format);
                const u32 src_bh = VideoCore::Surface::DefaultBlockHeight(src.info.format);
                const u32 dst_bw = VideoCore::Surface::DefaultBlockWidth(dst.info.format);
                const u32 dst_bh = VideoCore::Surface::DefaultBlockHeight(dst.info.format);
                const u64 columns = (u64{copy.extent.width} + src_bw - 1) / src_bw;
                const u64 rows = (u64{copy.extent.height} + src_bh - 1) / src_bh;
                const auto dst_level = copy.dst_subresource.base_level;
                if (dst_level < 0 || dst_level >= dst.info.resources.levels ||
                    copy.dst_offset.x < 0 || copy.dst_offset.y < 0) {
                    reject("invalid raw alias destination mip/offset");
                    return;
                }
                const u32 mip_width = std::max(1u, dst.info.size.width >> dst_level);
                const u32 mip_height = std::max(1u, dst.info.size.height >> dst_level);
                if (static_cast<u32>(copy.dst_offset.x) >= mip_width ||
                    static_cast<u32>(copy.dst_offset.y) >= mip_height) {
                    reject("raw alias destination offset outside mip");
                    return;
                }
                auto dst_extent = copy.extent;
                dst_extent.width = static_cast<u32>(std::min(
                    columns * dst_bw, u64{mip_width - static_cast<u32>(copy.dst_offset.x)}));
                dst_extent.height = static_cast<u32>(
                    std::min(rows * dst_bh, u64{mip_height - static_cast<u32>(copy.dst_offset.y)}));
                if ((u64{dst_extent.width} + dst_bw - 1) / dst_bw != columns ||
                    (u64{dst_extent.height} + dst_bh - 1) / dst_bh != rows) {
                    reject("raw alias guest block counts differ");
                    return;
                }
                const u64 size = CheckedMultiply(
                    CheckedMultiply(columns, rows),
                    CheckedMultiply(
                        CheckedMultiply(copy.extent.depth,
                                        static_cast<u32>(copy.src_subresource.num_layers)),
                        bytes));
                VideoCommon::BufferImageCopy download{};
                download.buffer_offset = total;
                download.buffer_size = size;
                download.buffer_row_length = copy.extent.width;
                download.buffer_image_height = copy.extent.height;
                download.image_subresource = copy.src_subresource;
                download.image_offset = copy.src_offset;
                download.image_extent = copy.extent;
                auto upload_copy = download;
                upload_copy.image_subresource = copy.dst_subresource;
                upload_copy.image_offset = copy.dst_offset;
                upload_copy.image_extent = dst_extent;
                upload_copy.buffer_row_length = dst_extent.width;
                upload_copy.buffer_image_height = dst_extent.height;
                downloads.push_back(download);
                uploads.push_back(upload_copy);
                total = CheckedAdd(total, size);
            }
            if (!total)
                return;
            context.Profile().Count(FrameCount::CpuReinterpretBytes, total);
            // Bounded evidence for format/range attribution in console captures.
            static u32 diagnostic_budget = 16;
            if (context.Profile().Enabled() && diagnostic_budget) {
                --diagnostic_budget;
                Diagnostic(fmt::format("D3D12_CPU_REINTERPRET src={} dst={} bytes={}",
                                       src.HostFormat().name, dst.HostFormat().name, total));
            }
            auto staging = DownloadStagingBuffer(total);
            // Reserved guest X/padding bits have no host plane; initialize them.
            std::fill(staging.mapped_span.begin(), staging.mapped_span.end(), u8{0});
            src.DownloadMemory(staging, downloads);
            context.Finish(FrameCount::ForcedSubmitTextureDownload);
            dst.UploadMemory(staging, uploads);
        } catch (const std::exception& error) {
            reject(error.what());
        }
        return;
    }
    try {
        BlitHelpers().CopyImage({dst.Handle(), dst.HostFormat()}, {src.Handle(), src.HostFormat()},
                                copies);
    } catch (const std::logic_error& error) {
        UnsupportedImage(src.info, src.HostFormat(), error.what());
    }
}
void TextureCacheRuntime::CopyImage(Image& dst, Image& src,
                                    std::span<const VideoCommon::ImageCopy> copies) {
    dst.NoteAstcBcUse(false);
    if (!dst.Handle() || !src.Handle())
        return;
    if (dst.Handle()->handle->GetDesc().SampleDesc.Count !=
        src.Handle()->handle->GetDesc().SampleDesc.Count) {
        CopyImageMSAA(dst, src, copies);
        return;
    }
    if (ShouldReinterpret(dst, src)) {
        ReinterpretImage(dst, src, copies);
        return;
    }
    try {
        BlitHelpers().CopyImage({dst.Handle(), dst.HostFormat()}, {src.Handle(), src.HostFormat()},
                                copies);
    } catch (const std::logic_error& error) {
        UnsupportedImage(src.info, src.HostFormat(), error.what());
    }
}
void TextureCacheRuntime::TickFrame() {
    bool trim_staging = false;
#ifdef NXBOX_UWP
    const auto usage = context.MemoryUsage();
    trim_staging = usage >= NativeTextureMemoryBudget(usage).critical;
#endif
    upload.Collect(trim_staging);
    readback.Collect(trim_staging);
    ++frame_tick;
    std::erase_if(framebuffers,
                  [this](const auto& entry) { return frame_tick - entry.last_frame > 2; });
}
ResourcePtr TextureCacheRuntime::UploadFramebuffer(Tegra::MaxwellDeviceMemoryManager& memory,
                                                   const Tegra::FramebufferConfig& config) {
    if (!config.width || !config.height || config.stride < config.width || config.stride > 16384 ||
        config.height > 16384)
        throw std::invalid_argument("D3D12 invalid framebuffer dimensions");
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.size = {config.width, config.height, 1};
    info.format = VideoCore::Surface::PixelFormatFromGPUPixelFormat(config.pixel_format);
    const auto format = MaxwellFormat(info.format);
    if (!format.Supported() || format.block_width != 1 || format.depth) {
        UnsupportedImage(info, format, "framebuffer requires supported uncompressed color format");
        return {};
    }
    const auto address = CheckedAdd(config.address, config.offset);
    auto entry = std::ranges::find(framebuffers, address, &FramebufferEntry::address);
    if (entry == framebuffers.end()) {
        if (framebuffers.size() == 64)
            framebuffers.erase(framebuffers.begin());
        framebuffers.push_back({address, {}, frame_tick});
        entry = std::prev(framebuffers.end());
    }
    entry->last_frame = frame_tick;
    auto& framebuffer = entry->resource;
    if (!framebuffer || framebuffer->handle->GetDesc().Width != config.width ||
        framebuffer->handle->GetDesc().Height != config.height ||
        framebuffer->handle->GetDesc().Format != static_cast<DXGI_FORMAT>(format.resource_format))
        framebuffer = MakeTexture(context, info, format);
    if (!framebuffer)
        return {};
    // NVFlinger framebuffer block height matches the existing GL/Vulkan fallback.
    constexpr u32 block_height = 4;
    const u32 guest_bytes = VideoCore::Surface::BytesPerBlock(info.format);
    const auto tiled_size = Tegra::Texture::CalculateSize(true, guest_bytes, config.stride,
                                                          config.height, 1, block_height, 0);
    if (tiled_size > (64ULL << 20))
        throw std::runtime_error("D3D12 framebuffer exceeds staging limit");
    framebuffer_tiled.resize(tiled_size);
    framebuffer_linear.resize(
        CheckedMultiply(CheckedMultiply(config.stride, config.height), guest_bytes));
    memory.ReadBlockUnsafe(address, framebuffer_tiled.data(), tiled_size);
    Tegra::Texture::UnswizzleTexture(framebuffer_linear, framebuffer_tiled, guest_bytes,
                                     config.stride, config.height, 1, block_height, 0);
    const auto footprint = MakeFootprint(format, config.width, config.height);
    auto staging = UploadStagingBuffer(footprint.size);
    for (u32 y = 0; y < config.height; ++y) {
        auto* host_row = staging.mapped_span.data() + y * footprint.row_pitch;
        auto* guest_row = framebuffer_linear.data() + u64{y} * config.stride * guest_bytes;
        if (format.conversion == FormatConversion::PackedColor)
            ConvertPackedRow(format.name, host_row, guest_row, config.width, false);
        else
            std::memcpy(host_row, guest_row, footprint.row_bytes);
    }
    context.Transition(framebuffer, D3D12_RESOURCE_STATE_COPY_DEST);
    context.Retain(staging.buffer);
    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
    source.pResource = staging.buffer->handle.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = {staging.offset,
                              {static_cast<DXGI_FORMAT>(format.view_format), config.width,
                               config.height, 1, footprint.row_pitch}};
    target.pResource = framebuffer->handle.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    context.MarkWork("upload/copy");
    context.Profile().Count(FrameCount::StagingCopyBytes, staging.mapped_span.size());
    context.Commands()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    return framebuffer;
}
} // namespace D3D12
