// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <memory>
#include "video_core/renderer_d3d12/d3d12_format_cast.h"
using DXGI_FORMAT = unsigned;
namespace VideoCore::Surface {
enum class PixelFormat { A8B8G8R8_UNORM, A8B8G8R8_SRGB, B8G8R8A8_UNORM, B8G8R8A8_SRGB, Float };
}
using VideoCore::Surface::PixelFormat;
enum class FormatConversion { None, PackedColor };
struct Format {
    unsigned view_format = 28;
    FormatConversion conversion = FormatConversion::None;
    bool depth = false;
    bool Supported() const {
        return true;
    }
};
Format MaxwellFormat(PixelFormat format) {
    Format result;
    switch (format) {
    case PixelFormat::A8B8G8R8_UNORM:
        result.view_format = 28;
        break;
    case PixelFormat::A8B8G8R8_SRGB:
        result.view_format = 29;
        break;
    case PixelFormat::B8G8R8A8_UNORM:
        result.view_format = 87;
        break;
    case PixelFormat::B8G8R8A8_SRGB:
        result.view_format = 91;
        break;
    case PixelFormat::Float:
        result.view_format = 41;
        break;
    }
    return result;
}
struct Resource {
    unsigned format = 27;
    bool SupportsViewFormat(unsigned view) const {
        return D3D12::CompatibleViewFormat(format, view);
    }
};
struct Image {
    struct {
        PixelFormat format = PixelFormat::A8B8G8R8_SRGB;
    } info;
    std::shared_ptr<Resource> resource = std::make_shared<Resource>();
    Format host;
    auto Handle() const {
        return resource;
    }
    Format HostFormat() const {
        return host;
    }
};
struct TextureCacheRuntime {
    bool CanPresentImage(const Image&, PixelFormat) const;
};
// PRODUCTION_METHOD
int main() {
    TextureCacheRuntime runtime;
    Image image;
    assert(runtime.CanPresentImage(image, PixelFormat::A8B8G8R8_UNORM));
    assert(!runtime.CanPresentImage(image, PixelFormat::B8G8R8A8_UNORM));
    image.resource->format = 29; // Typed SRGB is not automatically castable.
    assert(!runtime.CanPresentImage(image, PixelFormat::A8B8G8R8_UNORM));
    image.resource->format = 27;
    image.info.format = PixelFormat::Float;
    assert(!runtime.CanPresentImage(image, PixelFormat::A8B8G8R8_UNORM));
    image.info.format = PixelFormat::A8B8G8R8_SRGB;
    image.host.conversion = FormatConversion::PackedColor;
    assert(!runtime.CanPresentImage(image, PixelFormat::A8B8G8R8_UNORM));
    image.host.conversion = FormatConversion::None;
    image.host.depth = true;
    assert(!runtime.CanPresentImage(image, PixelFormat::A8B8G8R8_UNORM));
    image.host.depth = false;
    image.info.format = PixelFormat::B8G8R8A8_SRGB;
    image.resource->format = 90;
    assert(runtime.CanPresentImage(image, PixelFormat::B8G8R8A8_UNORM));
    image.resource.reset();
    assert(!runtime.CanPresentImage(image, PixelFormat::B8G8R8A8_UNORM));
}
