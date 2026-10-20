// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <span>

#include "video_core/engines/fermi_2d.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_formats.h"
#include "video_core/texture_cache/types.h"

namespace D3D12 {

// TextureCacheRuntime integration: BlitImage calls BlitColor/BlitDepthStencil/Resolve;
// CopyImage calls CopyImage; ReinterpretImage and ConvertImage call ConvertImage;
// CopyImageMSAA calls CopyImageMSAA (sample-grid copy, never an averaging resolve).
// The runtime owns one helper constructed at startup, alongside Context. These
// entry points do not bind guest pipelines: callers must invalidate graphics state
// after use. Existing runtime stubs are deliberately not changed by this module.
struct BlitSurface {
    ResourcePtr resource;
    FormatInfo format;
    u32 level{};
    u32 layer{};
};

enum class BlitConversion : u32 {
    Identity,
    D24S8ToRGBA8,
    RGBA8ToD24S8,
    D32FToRGBA8,
    RGBA8ToD32F,
    SwapRedBlue,
    S8D24ToRGBA8,
    RGBA8ToS8D24,
};

class BlitImageHelper {
public:
    explicit BlitImageHelper(Context& context);
    ~BlitImageHelper();
    BlitImageHelper(const BlitImageHelper&) = delete;
    BlitImageHelper& operator=(const BlitImageHelper&) = delete;

    void BlitColor(
        const BlitSurface& dst, const BlitSurface& src, const VideoCommon::Region2D& dst_region,
        const VideoCommon::Region2D& src_region, Tegra::Engines::Fermi2D::Filter filter,
        Tegra::Engines::Fermi2D::Operation operation = Tegra::Engines::Fermi2D::Operation::SrcCopy);
    void BlitDepthStencil(const BlitSurface& dst, const BlitSurface& src,
                          const VideoCommon::Region2D& dst_region,
                          const VideoCommon::Region2D& src_region);
    // Color: average floating/normalized samples; integers and depth/stencil: sample zero.
    void Resolve(const BlitSurface& dst, const BlitSurface& src,
                 const VideoCommon::Region2D& dst_region, const VideoCommon::Region2D& src_region);
    void ConvertImage(const BlitSurface& dst, const BlitSurface& src,
                      const VideoCommon::Region2D& dst_region,
                      const VideoCommon::Region2D& src_region, BlitConversion conversion);
    void CopyImage(const BlitSurface& dst, const BlitSurface& src,
                   std::span<const VideoCommon::ImageCopy> copies);
    // Copy extents/offsets follow Vulkan's sample-grid convention: MSAA->single
    // uses destination texels; single->MSAA uses destination pixels. Samples are
    // arranged 2x1, 2x2, 4x2, 4x4 for 2/4/8/16 samples respectively.
    void CopyImageMSAA(const BlitSurface& dst, const BlitSurface& src,
                       std::span<const VideoCommon::ImageCopy> copies);
    static bool CopyCompatible(const BlitSurface& dst, const BlitSurface& src);
    // Whether BlitColor can draw src into dst: color, uncompressed, same value class, and
    // integer surfaces only with point sampling.
    static bool DrawCompatible(const BlitSurface& dst, const BlitSurface& src, bool linear);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12
