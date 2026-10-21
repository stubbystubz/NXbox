#define FMT_HEADER_ONLY
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fmt/format.h>
using u64 = std::uint64_t;
using u32 = unsigned;
using u8 = unsigned char;
using DAddr = u64;
using GPUVAddr = u64;
using ImageId = unsigned;
using ImageMapId = unsigned;
constexpr unsigned YUZU_PAGEBITS = 12;
namespace boost::container {
template <class T, unsigned N>
using small_vector = std::vector<T>;
}
namespace Service::android {
enum class PixelFormat { Rgb565, Bgra8888, Rgba8888 };
}
enum class PixelFormat { R5G6B5_UNORM, B8G8R8A8_UNORM, A8B8G8R8_UNORM, A8B8G8R8_SRGB, Float };
enum class ImageType { e2D, e3D, Linear };
struct SubresourceBase {
    int level = 0, layer = 0;
};
struct SubresourceRange {
    SubresourceBase base;
    struct {
        int levels = 1, layers = 1;
    } extent;
};
enum class ImageViewType { e2D };
enum class SwizzleSource { R, G, B, OneFloat };
namespace Tegra {
enum class BlendMode { Opaque, Premultiplied };
struct FramebufferConfig {
    DAddr address = 324104192;
    unsigned offset = 0;
    unsigned width = 1920, height = 1080, stride = 1920;
    Service::android::PixelFormat pixel_format = Service::android::PixelFormat::Rgba8888;
    BlendMode blending = BlendMode::Opaque;
};
} // namespace Tegra
namespace ImageFlagBits {
constexpr unsigned CpuModified = 1, GpuModified = 2;
}
bool True(unsigned value) {
    return value != 0;
}
bool IsViewCompatible(PixelFormat a, PixelFormat b, bool broken_views, bool) {
    // D3D12 requires exact guest-format aliases; general Maxwell 32-bit view
    // compatibility would incorrectly accept both Float and SRGB here.
    if (broken_views)
        return a == b;
    return a == b || b == PixelFormat::A8B8G8R8_UNORM;
}

namespace PassDump {
struct Config {
    bool enabled = false;
    u64 frame = 1200;
} config;
const Config& GetConfig() {
    return config;
}
const char* FormatName(PixelFormat) {
    return "test";
}
} // namespace PassDump
struct ImageViewInfo {
    ImageViewType type;
    PixelFormat format;
    SubresourceRange range;
    u8 x_source = 0, y_source = 1, z_source = 2, w_source = 3;
};
struct ImageBase {
    DAddr cpu_addr = 0x261c1000;
    DAddr cpu_addr_end = cpu_addr + 16 * 1024 * 1024;
    std::vector<unsigned> aliased_images;
    GPUVAddr gpu_addr = 0x55ceb0000;
    u64 modification_tick = 1;
    unsigned flags = 0;
    struct Info {
        struct {
            int levels = 1, layers = 1;
        } resources;
        struct {
            unsigned width = 0, height = 4, depth = 0;
        } block;
        unsigned tile_width_spacing = 0, num_samples = 1, layer_stride = 0;
        ImageType type = ImageType::e2D;
        PixelFormat format = PixelFormat::A8B8G8R8_UNORM;
        struct Size {
            unsigned width = 1920, height = 1080;
        } size;
    } info;
    std::vector<unsigned> image_view_ids;
    std::optional<SubresourceBase> TryFindBase(GPUVAddr address) const {
        if (address == gpu_addr)
            return SubresourceBase{};
        if (info.layer_stride && address == gpu_addr + info.layer_stride &&
            info.resources.layers > 1)
            return SubresourceBase{0, 1};
        return std::nullopt;
    }
    bool IsRescaled() const {
        return false;
    }
};
struct ImageMapView {
    ImageId image_id;
    DAddr cpu_addr = 0x261c1000;
    GPUVAddr gpu_addr = 0x55ceb0000;
    bool Overlaps(DAddr address, unsigned) const {
        return address >= cpu_addr && address < cpu_addr + 16 * 1024 * 1024;
    }
};
struct View {
    ImageId image_id = 0;
    SubresourceRange range;
};
struct Params {
    using ImageView = View;
};
template <class P>
struct TextureCache {
    using ImageView = typename P::ImageView;
    struct Runtime {
        bool CanPresentImage(const ImageBase& image, PixelFormat format) const {
            return image.info.format == format || (format == PixelFormat::A8B8G8R8_UNORM &&
                                                   image.info.format == PixelFormat::A8B8G8R8_SRGB);
        }
        bool HasBrokenTextureViewFormats() const {
            return true;
        }
        bool HasNativeBgr() const {
            return true;
        }
    } runtime;
    std::map<u64, std::vector<ImageMapId>> page_table;
    std::array<ImageBase, 8> slot_images;
    std::array<ImageMapView, 8> slot_map_views;
    std::vector<View> slot_image_views = std::vector<View>(8);
    struct PassDumpWriter {
        ImageId id;
        u64 frame, order;
        GPUVAddr source;
        std::string writer;
    };
    std::vector<PassDumpWriter> pass_dump_writers;
    std::vector<std::string> reports;
    u64 CaptureFrame() const {
        return 1200;
    }
    void ReportPassDump(const std::string& s) {
        reports.push_back(s);
    }
    unsigned FindOrEmplaceImageView(ImageId id, ImageViewInfo info) {
        assert(info.w_source == static_cast<u8>(SwizzleSource::OneFloat));
        slot_image_views[id].image_id = id;
        slot_image_views[id].range = info.range;
        slot_images[id].image_view_ids = {id};
        return id;
    }
    void PrepareImage(ImageId, bool, bool) {
        slot_image_views.reserve(slot_image_views.capacity() + 16);
    }
    ImageView* PrepareImageForPresentation(ImageView*);
    std::pair<typename P::ImageView*, bool> TryFindFramebufferImageView(
        const Tegra::FramebufferConfig&, DAddr, bool = false, bool = false);
};
// PRODUCTION_METHOD
int main() {
    TextureCache<Params> cache;
    Tegra::FramebufferConfig config;
    constexpr DAddr address = 0x261c1000;
    assert(!cache.TryFindFramebufferImageView(config, address).first);
    cache.page_table[address >> YUZU_PAGEBITS] = {1};
    cache.slot_map_views[1] = {1};
    // A fresh compatible image is valid even before any descriptor view exists.
    auto [first, scaled] = cache.TryFindFramebufferImageView(config, address);
    assert(first && first->image_id == 1 && !scaled);
    auto prepared = cache.PrepareImageForPresentation(first);
    assert(prepared && prepared != first && prepared->image_id == 1);
    for (unsigned i = 2; i <= 6; ++i) {
        cache.page_table[address >> YUZU_PAGEBITS].push_back(i);
        cache.slot_map_views[i] = {i};
        cache.slot_images[i].modification_tick = 100 + i;
        cache.slot_images[i].image_view_ids = {i};
    }
    cache.slot_images[2].info.format = PixelFormat::Float;
    cache.slot_images[3].info.size.width = 64;
    cache.slot_images[4].info.size.height = 64;
    cache.slot_images[5].info.type = ImageType::e3D;
    cache.slot_images[6].cpu_addr += 4096;
    PassDump::config.enabled = true;
    assert(cache.TryFindFramebufferImageView(config, address).first->image_id == 1);
    assert(cache.reports.size() == 5); // different-base image is never a candidate
    cache.slot_images[2].info.format = PixelFormat::A8B8G8R8_SRGB;
    assert(cache.TryFindFramebufferImageView(config, address).first->image_id == 1);
    cache.slot_images[2].info.format = PixelFormat::A8B8G8R8_UNORM;
    assert(cache.TryFindFramebufferImageView(config, address).first->image_id == 2);
    cache.slot_images[2].info.format = PixelFormat::Float;
    cache.slot_images[1].info.type = ImageType::e3D;
    assert(!cache.TryFindFramebufferImageView(config, address).first);
    // Padded stride must match the storage geometry, not just the visible width.
    cache.slot_images[1].info.type = ImageType::e2D;
    config.stride = 2048;
    assert(!cache.TryFindFramebufferImageView(config, address).first);
    cache.slot_images[1].info.size.width = 2048;
    assert(cache.TryFindFramebufferImageView(config, address).first->image_id == 1);
    // MK8D config: RGBA8 display with a live GPU-owned sRGB RT alias.
    TextureCache<Params> mk8;
    config = {};
    const auto mk8_address = config.address;
    mk8.page_table[mk8_address >> YUZU_PAGEBITS] = {1, 2};
    for (unsigned i : {1u, 2u}) {
        mk8.slot_map_views[i] = {i, mk8_address, 0x55ceb0000};
        mk8.slot_images[i].cpu_addr = mk8_address;
        mk8.slot_images[i].cpu_addr_end = mk8_address + 16 * 1024 * 1024;
        mk8.slot_images[i].flags = ImageFlagBits::GpuModified;
        mk8.slot_images[i].modification_tick = i;
    }
    mk8.slot_images[1].info.format = PixelFormat::Float;
    mk8.slot_images[2].info.format = PixelFormat::A8B8G8R8_SRGB;
    mk8.slot_images[2].aliased_images = {1};
    assert(!mk8.TryFindFramebufferImageView(config, mk8_address).first);
    auto gpu = mk8.TryFindFramebufferImageView(config, mk8_address, true, true).first;
    assert(gpu && gpu->image_id == 2);
    assert(std::ranges::any_of(mk8.reports, [](const auto& s) {
        return s.find("legacy_reason=format gpu_reason=accepted") != std::string::npos;
    }));
    // Height padding inside the same 128-row GOB allocation is safe.
    mk8.slot_images[2].info.size.height = 1152;
    assert(mk8.TryFindFramebufferImageView(config, mk8_address, true).first);
    mk8.slot_images[2].info.size.height = 1153;
    assert(!mk8.TryFindFramebufferImageView(config, mk8_address, true).first);
    mk8.slot_images[2].info.size.height = 1080;
    // A newer incompatible owner must win over the older compatible view.
    mk8.slot_images[1].modification_tick = 3;
    assert(!mk8.TryFindFramebufferImageView(config, mk8_address, true, true).first);
    assert(mk8.reports.back().find("newer_gpu_overlap") != std::string::npos);
    mk8.slot_images[1].modification_tick = 1;
    for (auto type : {ImageType::Linear, ImageType::e3D}) {
        mk8.slot_images[2].info.type = type;
        assert(!mk8.TryFindFramebufferImageView(config, mk8_address, true).first);
    }
    mk8.slot_images[2].info.type = ImageType::e2D;
    mk8.slot_images[2].info.block.height = 3;
    assert(!mk8.TryFindFramebufferImageView(config, mk8_address, true).first);
    mk8.slot_images[2].info.block.height = 4;
    mk8.slot_images[2].info.num_samples = 4;
    assert(!mk8.TryFindFramebufferImageView(config, mk8_address, true).first);
    mk8.slot_images[2].info.num_samples = 1;
    // A display offset can name an array layer, but not arbitrary bytes in it.
    constexpr unsigned layer_stride = 1920 * 1152 * 4;
    mk8.slot_images[2].info.layer_stride = layer_stride;
    mk8.slot_images[2].info.resources.layers = 2;
    config.offset = layer_stride;
    mk8.page_table[(mk8_address + config.offset) >> YUZU_PAGEBITS] = {2};
    gpu = mk8.TryFindFramebufferImageView(config, mk8_address + config.offset, true).first;
    assert(gpu && gpu->range.base.layer == 1);
    assert(!mk8.TryFindFramebufferImageView(config, mk8_address + config.offset + 4, true).first);
}
