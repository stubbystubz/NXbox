// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dxgi1_4.h>
#include "video_core/renderer_d3d12/d3d12_context.h"
namespace D3D12::Present {
class FrameGenerator {
public:
    explicit FrameGenerator(Context& context);
    ~FrameGenerator();
    // No swapchain writes. Returns null on warmup, cut, fast input or session failure.
    ResourcePtr Generate(const ResourcePtr& current, double game_seconds) noexcept;
    // Configure once per real presentation; unknown refresh conservatively selects 2x.
    unsigned Configure(IDXGISwapChain3* swapchain, unsigned requested);
    ResourcePtr Interpolate(float fraction) noexcept;
    // Native-resolution analysis shared by temporal upscaling; no warp dispatch.
    bool Analyze(const ResourcePtr& source) noexcept;
    ResourcePtr Motion() const;
    void UseMotion(const ResourcePtr& motion);
    ResourcePtr Previous() const;
    void Reset();
    void SetSourceExtent(unsigned width, unsigned height);
    ResourcePtr Current() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12::Present
