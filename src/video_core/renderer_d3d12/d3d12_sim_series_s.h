// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include "video_core/renderer_d3d12/d3d12_context.h"

namespace D3D12 {
// Opt-in, graphics-queue-only load. The encoder owns all state; no completion callbacks
// capture this object, and Context drains the queue before destroying it.
class SimSeriesS {
public:
    explicit SimSeriesS(Context& context);
    void PrepareFrame();
    void BeginSegment(CommandFacade& commands);
    void EndSegment(CommandFacade& commands);
    void EndFrame();

private:
    static constexpr u32 SlotCount = 8;
    static constexpr u32 MaxSegments = 1024;
    static constexpr u32 QueriesPerSlot = MaxSegments * 2 + 2;
    struct Slot {
        ResourcePtr timestamps;
        ResourcePtr output;
        ResourcePtr readback;
        u64 tick{};
        bool pending{};
    };
    void Poll();
    void ReadSample(Slot& slot);
    Context& context;
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<Slot, SlotCount> slots;
    u32 slot_index{};
    u32 segments{};
    bool segment_open{};
    bool frame_open{};
    u64 frequency{};
    double iterations_per_tick{};
    double ballast_ms_sum{};
    u64 samples{};
    u64 capped_frames{};
    std::chrono::steady_clock::time_point last_log{std::chrono::steady_clock::now()};
};
} // namespace D3D12
