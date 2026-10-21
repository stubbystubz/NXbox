// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/renderer_d3d12/d3d12_context.h"
namespace D3D12::Present {
// Jitter-free TAAU over normalized compositor color. History is never shared
// between layers, captures, viewports or transformed source regions.
class TemporalUpscaler {
public:
    explicit TemporalUpscaler(Context& context);
    ~TemporalUpscaler();
    void Reset();
    ResourcePtr Motion() const;
    ResourcePtr Upscale(const ResourcePtr& native, const ResourcePtr& spatial, unsigned width,
                        unsigned height) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12::Present
