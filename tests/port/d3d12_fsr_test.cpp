// SPDX-License-Identifier: GPL-3.0-or-later
// Execute the production DrawLayer with a recording backend, without Windows headers.
#include <cassert>
#include <memory>
#include <vector>
#include "common/maxwell_method_profile.h"
#include "video_core/renderer_d3d12/d3d12_present/present_math.h"
#include "video_core/renderer_d3d12/d3d12_present/present_policy.h"
using UINT64 = unsigned long long;
using u64 = unsigned long long;
constexpr unsigned D3D12_SRV_DIMENSION_TEXTURE2DARRAY = 1;
constexpr unsigned D3D12_RESOURCE_DIMENSION_TEXTURE2D = 2;
struct D3D12_CPU_DESCRIPTOR_HANDLE {};
struct D3D12_VIEWPORT {
    float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth;
};
struct D3D12_RECT {};
struct D3D12_SHADER_RESOURCE_VIEW_DESC {
    unsigned ViewDimension{1};
    struct Array {
        unsigned MipLevels{1}, ArraySize{1}, PlaneSlice{}, MostDetailedMip{}, FirstArraySlice{};
    } Texture2DArray;
};
struct Description {
    unsigned Dimension{2};
    struct Samples {
        unsigned Count{1};
    } SampleDesc;
    unsigned MipLevels{1}, DepthOrArraySize{1};
    UINT64 Width{640};
    unsigned Height{360};
};
struct Texture {
    Description desc;
    Description GetDesc() const {
        return desc;
    }
};
struct Resource {
    std::shared_ptr<Texture> handle = std::make_shared<Texture>();
};
using ResourcePtr = std::shared_ptr<Resource>;
namespace D3D12::Present {
void Diagnostic(const char*) {}
constexpr unsigned FrameSlots = D3D12::Present::MaxFrameSlots;
constexpr unsigned MaxPasses = 256, FxaaPass = 6, EasuPass = 7;
enum class Filter { Bilinear, Bicubic, Gaussian, ScaleForce, Area, Fsr };
enum class Blend { Opaque, Premultiplied, Coverage };
struct Options {
    Filter filter{Filter::Fsr};
    bool fxaa{};
    float sharpness{.5f};
};
struct Layer {
    ResourcePtr source = std::make_shared<Resource>();
    D3D12_SHADER_RESOURCE_VIEW_DESC view;
    Extent guest_extent{1280, 720};
    Crop crop_pixels{0, 0, 0, 0};
    unsigned transform{};
    Blend blend{Blend::Opaque};
};
struct Constants {
    Crop crop;
    float source_width, source_height, output_width, output_height;
    unsigned transform, opaque;
    float sharpness;
    unsigned unused;
};
D3D12_SHADER_RESOURCE_VIEW_DESC View(Extent) {
    return {};
}
struct Pipeline {
    int value;
    int Get() const {
        return value;
    }
};
struct TemporalUpscaler {
    template <class T>
    explicit TemporalUpscaler(T&) {}
    void Reset() {}
    ResourcePtr Upscale(const ResourcePtr& source, const ResourcePtr&, unsigned, unsigned) {
        return source;
    }
};
struct Impl {
    struct Context {
        u64 waited{};
        void WaitSubmitted(u64 tick, const char*) {
            waited = tick;
        }
    } context;
    bool begun{true};
    bool temporal_enabled{}, temporal_used{}, temporal_single_layer{true};
    std::unique_ptr<TemporalUpscaler> temporal;
    Crop temporal_crop{};
    unsigned temporal_transform{};
    unsigned cursor{}, frame_index{};
    std::array<std::array<Pipeline, 3>, 6> final{};
    struct Pass {
        unsigned pass;
        Extent extent;
        Constants constants;
    };
    std::vector<Pass> passes;
    Constants result{};
    int pipeline{};
    Impl() {
        for (unsigned i = 0; i < final.size(); ++i)
            for (unsigned j = 0; j < 3; ++j)
                final[i][j].value = static_cast<int>(i * 3 + j);
    }
    ResourcePtr Preprocess(unsigned pass, unsigned, const ResourcePtr& resource,
                           D3D12_SHADER_RESOURCE_VIEW_DESC, Extent extent, Constants constants) {
        passes.push_back({pass, extent, constants});
        ++cursor;
        return resource;
    }
    void Draw(const ResourcePtr&, D3D12_SHADER_RESOURCE_VIEW_DESC, D3D12_CPU_DESCRIPTOR_HANDLE,
              D3D12_VIEWPORT, D3D12_RECT, Constants constants, int selected) {
        ++cursor;
        result = constants;
        pipeline = selected;
    }
};
struct Presenter {
    std::unique_ptr<Impl> impl = std::make_unique<Impl>();
    void BeginFrame(unsigned, u64);
    void DrawLayer(const Layer&, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_VIEWPORT, D3D12_RECT, Options);
};
// BEGIN_FRAME
// DRAW_LAYER
} // namespace D3D12::Present
int main() {
    using namespace D3D12::Present;
    Presenter slots;
    slots.BeginFrame(1, 91);
    assert(slots.impl->frame_index == 1 && slots.impl->cursor == 256);
    assert(slots.impl->context.waited == 91);
    slots.BeginFrame(2, 93);
    assert(slots.impl->frame_index == 2);
    slots.BeginFrame(0, 92);
    assert(slots.impl->frame_index == 0 && slots.impl->cursor == 0);
    assert(slots.impl->context.waited == 92);
    try {
        slots.BeginFrame(3, 93);
        assert(false);
    } catch (const std::invalid_argument&) {
    }
    for (unsigned transform = 0; transform < 8; ++transform) {
        for (unsigned scale : {1u, 2u, 3u, 4u}) {
            Presenter presenter;
            Layer layer;
            layer.transform = transform;
            layer.blend = Blend::Coverage;
            layer.source->handle->desc.Width = scale * 320;
            layer.source->handle->desc.Height = scale * 180;
            const D3D12_VIEWPORT viewport{10, 20, 1280, 720, 0, 1};
            presenter.DrawLayer(layer, {}, viewport, {}, {});
            const bool upscale = scale < 4 || (transform & 4);
            assert(presenter.impl->passes.size() == (upscale ? 1u : 0u));
            assert(presenter.impl->cursor == (upscale ? 2u : 1u));
            assert(presenter.impl->pipeline == (upscale ? 17 : 2));
            const auto result = presenter.impl->result;
            assert(result.transform == transform && result.opaque == 0);
            assert(result.crop.left == 0 && result.crop.right == 1);
            if (upscale) {
                const auto easu = presenter.impl->passes.front();
                assert(easu.pass == EasuPass);
                assert((easu.extent == ((transform & 4) ? Extent{720, 1280} : Extent{1280, 720})));
                assert(easu.constants.transform == 0);
                assert(result.source_width == easu.extent.width);
            }
        }
    }
    Presenter bilinear;
    bilinear.DrawLayer({}, {}, {0, 0, 1280, 720, 0, 1}, {}, {Filter::Bilinear});
    assert(bilinear.impl->passes.empty());
    Presenter cropped;
    Layer layer;
    layer.crop_pixels = {160, 90, 1120, 630};
    cropped.DrawLayer(layer, {}, {0, 0, 1280, 720, 0, 1}, {}, {});
    assert(cropped.impl->passes.front().constants.crop.left == .125f);
    assert(cropped.impl->result.crop.left == 0);
    Presenter capacity;
    capacity.impl->frame_index = 1;
    capacity.impl->cursor = 511;
    try {
        capacity.DrawLayer({}, {}, {0, 0, 1280, 720, 0, 1}, {}, {});
        assert(false);
    } catch (const std::runtime_error&) {
    }
}
