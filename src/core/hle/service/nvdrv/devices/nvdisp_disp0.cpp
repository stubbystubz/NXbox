// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include <boost/container/small_vector.hpp>

#include "common/adpf.h"
#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"

namespace Service::Nvidia::Devices {

namespace {

Tegra::BlendMode ConvertBlending(Service::Nvnflinger::LayerBlending blending) {
    switch (blending) {
    case Service::Nvnflinger::LayerBlending::None:
    default:
        return Tegra::BlendMode::Opaque;
    case Service::Nvnflinger::LayerBlending::Premultiplied:
        return Tegra::BlendMode::Premultiplied;
    case Service::Nvnflinger::LayerBlending::Coverage:
        return Tegra::BlendMode::Coverage;
    }
}

} // namespace

nvdisp_disp0::nvdisp_disp0(Core::System& system_, NvCore::Container& core)
    : nvdevice{system_}, container{core}, nvmap{core.GetNvMapFile()} {}
nvdisp_disp0::~nvdisp_disp0() = default;

NvResult nvdisp_disp0::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                              std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvdisp_disp0::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                              std::span<const u8> inline_input, std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvdisp_disp0::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                              std::span<u8> output, std::span<u8> inline_output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvdisp_disp0::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {}
void nvdisp_disp0::OnClose(DeviceFD fd) {}

void nvdisp_disp0::Composite(std::span<const Nvnflinger::HwcLayer> sorted_layers) {
    std::vector<Tegra::FramebufferConfig> output_layers;
    std::vector<Service::Nvidia::NvFence> output_fences;
    output_layers.reserve(sorted_layers.size());
    output_fences.reserve(sorted_layers.size());

    static const bool layer_diag = [] {
        const char* value = std::getenv("NXBOX_LAYER_DIAG");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    // Composite calls are serialized by SurfaceFlinger. Count only when tracing is enabled.
    static u64 diagnostic_frame = 0;
    if (layer_diag) {
        ++diagnostic_frame;
        LOG_INFO(Service_VI, "NXBOX_LAYER frame={} layers={}", diagnostic_frame,
                 sorted_layers.size());
    }

    for (auto& layer : sorted_layers) {
        output_layers.emplace_back(Tegra::FramebufferConfig{
            .address = nvmap.GetHandleAddress(layer.buffer_handle),
            .offset = layer.offset,
            .width = layer.width,
            .height = layer.height,
            .stride = layer.stride,
            .pixel_format = layer.format,
            .transform_flags = layer.transform,
            .crop_rect = layer.crop_rect,
            .blending = ConvertBlending(layer.blending),
            .layer_stack_mask = layer.layer_stack_mask,
        });

        if (layer_diag) {
            const auto& framebuffer = output_layers.back();
            // VI position/size setters are unimplemented; all backends draw to layout.screen.
            LOG_INFO(Service_VI,
                     "NXBOX_LAYER frame={} index={} handle={} address={:#x} offset={:#x} "
                     "buffer={:#x} size={}x{} stride_pixels={} format={:#x} crop={},{},{},{} "
                     "position=0,0 destination=full_screen z={} blending={:#x} transform={:#x} "
                     "stack_mask={:#x}",
                     diagnostic_frame, output_layers.size() - 1, layer.buffer_handle,
                     framebuffer.address, framebuffer.offset,
                     framebuffer.address + framebuffer.offset, framebuffer.width,
                     framebuffer.height, framebuffer.stride,
                     static_cast<u32>(framebuffer.pixel_format), framebuffer.crop_rect.left,
                     framebuffer.crop_rect.top, framebuffer.crop_rect.right,
                     framebuffer.crop_rect.bottom, layer.z_index, static_cast<u32>(layer.blending),
                     static_cast<u32>(framebuffer.transform_flags), framebuffer.layer_stack_mask);
        }

        for (size_t i = 0; i < layer.acquire_fence.num_fences; i++) {
            if (layer.acquire_fence.fences[i].id >= 0) {
                output_fences.push_back(layer.acquire_fence.fences[i]);
            }
        }
    }

    system.GPU().RequestComposite(std::move(output_layers), std::move(output_fences));
    Common::ADPF::ReportFrameInterval();
    system.SpeedLimiter().DoSpeedLimiting(system.CoreTiming().GetGlobalTimeUs());
    system.GetPerfStats().EndSystemFrame();
    system.GetPerfStats().BeginSystemFrame();
}

void nvdisp_disp0::WaitForComposite() {
    system.GPU().WaitForComposite();
}

Kernel::KEvent* nvdisp_disp0::QueryEvent(u32 event_id) {
    LOG_CRITICAL(Service_NVDRV, "Unknown DISP Event {}", event_id);
    return nullptr;
}

} // namespace Service::Nvidia::Devices
