// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/renderer_d3d12/d3d12_context.h"
namespace D3D12::Present {
class Recorder {
public:
    explicit Recorder(Context& context);
    ~Recorder();
    // Called after all display overlays, before transition to PRESENT. Never waits for a slot.
    void Capture(const ResourcePtr& source, D3D12_CPU_DESCRIPTOR_HANDLE rtv, bool game_frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12::Present
