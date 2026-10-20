#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
using u32 = unsigned;
using u64 = std::uint64_t;
using DXGI_FORMAT = unsigned;
constexpr unsigned D3D12_SRV_DIMENSION_TEXTURE2D = 1;
constexpr unsigned D3D12_SRV_DIMENSION_TEXTURE2DARRAY = 2;
constexpr unsigned D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING = 5768;
struct D3D12_SHADER_RESOURCE_VIEW_DESC {
    unsigned Format = 28, ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    unsigned Shader4ComponentMapping = 0;
    struct {
        unsigned MostDetailedMip = 0;
    } Texture2D;
    struct {
        unsigned MostDetailedMip, MipLevels, FirstArraySlice, ArraySize, PlaneSlice, MinLOD;
    } Texture2DArray{};
};
namespace Tegra {
struct FramebufferConfig {
    u64 address = 629936120;
    unsigned offset = 8, pixel_format = 0;
};
} // namespace Tegra
namespace VideoCore::Surface {
unsigned PixelFormatFromGPUPixelFormat(unsigned f) {
    return f;
}
} // namespace VideoCore::Surface
struct Format {
    unsigned view_format = 28;
};
Format MaxwellFormat(unsigned) {
    return {};
}
void Diagnostic(const std::string&) {}
struct Description {
    unsigned Width = 1920, Height = 1080;
    struct {
        unsigned Count = 1;
    } SampleDesc;
};
struct Handle {
    Description GetDesc() {
        return {};
    }
};
struct HandlePtr : std::shared_ptr<Handle> {
    HandlePtr() : std::shared_ptr<Handle>(std::make_shared<Handle>()) {}
    Handle* Get() const {
        return get();
    }
};
struct Resource {
    HandlePtr handle;
    unsigned pixel = 0;
};
using ResourcePtr = std::shared_ptr<Resource>;
struct View {
    unsigned image_id = 1;
    struct {
        unsigned width = 1920, height = 1080;
    } size;
    ResourcePtr resource;
    ResourcePtr ImageHandle() {
        return resource;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC Description() {
        return {};
    }
    void LogUnsupported(const char*) {
        assert(false);
    }
};
struct GuestMemory {
    unsigned pixel = 0;
};
struct Cache {
    std::mutex mutex;
    View* selected = nullptr;
    GuestMemory* memory = nullptr;
    unsigned gpu_pixel = 76, downloads = 0, prepares = 0, snapshots = 0;
    std::pair<View*, bool> TryFindFramebufferImageView(const Tegra::FramebufferConfig&,
                                                       u64 address) {
        assert(address == 629936128);
        return {selected, false};
    }
    View* PrepareImageForPresentation(View* view) {
        ++prepares;
        return view;
    }
    void SnapshotPassDumpPresentation(View&) {
        ++snapshots;
    }
    std::string RecordPassDumpPresentation(u64, View*) {
        return {};
    }
    void DownloadMemory(u64 address, unsigned size) {
        assert(address == 629936128 && size == 1);
        ++downloads;
        memory->pixel = gpu_pixel;
    }
};
struct Runtime {
    ResourcePtr ResolveFramebuffer(View&) {
        assert(false);
        return {};
    }
    ResourcePtr UploadFramebuffer(GuestMemory& memory, const Tegra::FramebufferConfig&) {
        // Only the opt-in download makes the newer GPU pixels visible in guest RAM.
        assert(memory.pixel == (EXPECT_FALLBACK_DOWNLOAD ? 76u : 0u));
        auto result = std::make_shared<Resource>();
        result->pixel = memory.pixel;
        return result;
    }
};
struct RasterizerD3D12 {
    struct DisplayTexture {
        ResourcePtr resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        bool scaled = false;
        unsigned width = 0, height = 0;
    };
    GuestMemory device_memory;
    Cache texture_cache;
    Runtime texture_runtime;
    bool presentation_logged = false;
    RasterizerD3D12() {
        texture_cache.memory = &device_memory;
    }
    bool OnGpuPipeline() const {
        return false;
    }
    template <class F>
    auto RunGpuPipeline(F f) {
        return f();
    }
    DisplayTexture FramebufferTexture(const Tegra::FramebufferConfig&);
};
// PRODUCTION_METHOD
int main() {
    constexpr unsigned downloads_per_fallback = EXPECT_FALLBACK_DOWNLOAD;
    constexpr unsigned expected_pixel = EXPECT_FALLBACK_DOWNLOAD ? 76 : 0;
    RasterizerD3D12 rasterizer;
    Tegra::FramebufferConfig config;
    auto fallback = rasterizer.FramebufferTexture(config);
    assert(fallback.resource->pixel == expected_pixel);
    assert(rasterizer.texture_cache.downloads == downloads_per_fallback);
    View cached;
    cached.resource = std::make_shared<Resource>();
    rasterizer.texture_cache.selected = &cached;
    auto direct = rasterizer.FramebufferTexture(config);
    assert(direct.resource == cached.resource);
    assert(rasterizer.texture_cache.downloads == downloads_per_fallback &&
           rasterizer.texture_cache.prepares == 1);
    assert(rasterizer.texture_cache.snapshots == 1);
    // A view without a resource takes the same opt-in fallback.
    cached.resource.reset();
    rasterizer.device_memory.pixel = 0;
    assert(rasterizer.FramebufferTexture(config).resource->pixel == expected_pixel);
    assert(rasterizer.texture_cache.downloads == 2 * downloads_per_fallback);
}
