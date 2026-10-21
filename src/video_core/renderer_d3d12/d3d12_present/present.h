// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Composition integration (renderer_d3d12.cpp):
// Own one Presenter alongside Impl::context; construction compiles all DXIL/PSOs.
// Resolve ALL FramebufferTexture uploads before BeginFrame(), which waits for its slot
// before recycling descriptors. Clear the backbuffer, then DrawLayer() for each
// selected layer with its ResourcePtr/SRV, guest extent, guest-pixel crop,
// transform_flags and blend mode. Use the real SRV mip extent, never multiply an
// already scaled cache extent by Settings::resolution_info a second time.
// Map PresentFilters scaling/AA settings to Filter/Options; sharpness is Eden's
// fsr_sharpening_slider / 100.f. DrawLayer owns viewport/scissor/root/heaps/PSO.
// Transition the backbuffer to PRESENT, Context::Submit(), then
// Check(presenter.Present(swapchain, vsync, interval), "Present"). It uses DXGI
// refresh pacing (0 when disabled, 1..4 when enabled), without a CPU sleep.
// Drain Context before ResizeBuffers. Presenter teardown also drains Context.
// Targets must be single-sample RGBA8 RTVs, matching the existing swapchain.
// Empty frames may skip DrawLayer; the caller still owns the backbuffer clear.
#include <memory>
#include <dxgi1_4.h>
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_present/present_math.h"
namespace D3D12::Present {
enum class Filter { Bilinear, Bicubic, Gaussian, ScaleForce, Area, Fsr };
enum class Blend { Opaque, Premultiplied, Coverage };
struct Options {
    Filter filter{Filter::Bilinear};
    bool fxaa{};
    float sharpness{.5f};
};
struct Layer {
    ResourcePtr source;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    Extent guest_extent;
    Crop crop_pixels{0, 0, 0, 0}; // Empty selects the full guest extent.
    std::uint32_t transform{};
    Blend blend{Blend::Opaque};
};
// Created lazily on the first enabled swapchain presentation. No texture uploads or SRVs.
class FpsCounter {
public:
    explicit FpsCounter(Context& context);
    ~FpsCounter();
    void Draw(D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
class Presenter {
public:
    explicit Presenter(Context& context);
    ~Presenter();
    // Two swapchain slots; previous_tick must cover every use of this slot.
    void BeginFrame(unsigned frame_index, u64 previous_tick);
    void SetCompositeLayerCount(unsigned count);
    ResourcePtr Motion() const;
    void DrawLayer(const Layer& layer, D3D12_CPU_DESCRIPTOR_HANDLE target, D3D12_VIEWPORT viewport,
                   D3D12_RECT scissor, Options options);
    HRESULT Present(IDXGISwapChain3* swapchain, bool vsync, unsigned interval = 1);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12::Present
