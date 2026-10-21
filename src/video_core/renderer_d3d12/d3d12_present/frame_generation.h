// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/renderer_d3d12/d3d12_context.h"
namespace D3D12::Present {
class FrameGenerator {
public:
    explicit FrameGenerator(Context& context);
    ~FrameGenerator();
    // No swapchain writes. Returns null on warmup, cut, fast input or session failure.
    ResourcePtr Generate(const ResourcePtr& current, double game_seconds) noexcept;
    void SetSourceExtent(unsigned width, unsigned height);
    ResourcePtr Current() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12::Present
