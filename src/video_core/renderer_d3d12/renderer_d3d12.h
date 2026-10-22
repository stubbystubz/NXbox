// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include "video_core/renderer_base.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"

namespace D3D12 {
class RendererD3D12 final : public VideoCore::RendererBase {
public:
    explicit RendererD3D12(Core::Frontend::EmuWindow& window,
                           Tegra::MaxwellDeviceMemoryManager& memory, Tegra::GPU& gpu,
                           std::unique_ptr<Core::Frontend::GraphicsContext> context);
    ~RendererD3D12() override;

    bool ReportFailure(const std::exception& error) noexcept override;
    bool HasFailed() const override;
    std::chrono::steady_clock::time_point PresentationDeadline() const override;
    void PresentIdle() override;
    void Composite(std::span<const Tegra::FramebufferConfig> layers) override;
    std::vector<u8> GetAppletCaptureBuffer() override;
    VideoCore::RasterizerInterface* ReadRasterizer() override;
    std::string GetDeviceVendor() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    Tegra::GPU& gpu;
    RasterizerD3D12 rasterizer;
};
} // namespace D3D12
