// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_sim_series_s.h"

#include <cstring>
#include <fmt/format.h>
#include "common/nxbox_sim_series_s.h"
#include "video_core/host_shaders/hlsl/sim_series_s_hlsl.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12 {
SimSeriesS::SimSeriesS(Context& context_) : context{context_} {
    Check(context.Queue()->GetTimestampFrequency(&frequency), "simulation timestamp frequency",
          context.Device());
    if (!frequency)
        throw std::runtime_error("D3D12 simulation timestamp frequency is zero");
    D3D12_QUERY_HEAP_DESC query_desc{};
    query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_desc.Count = SlotCount * QueriesPerSlot;
    Check(context.Device()->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&heap)),
          "simulation timestamp heap", context.Device());

    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 4};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor = {0, 0};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor = {0, 0};
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = static_cast<UINT>(parameters.size());
    root_desc.pParameters = parameters.data();
    ComPtr<ID3DBlob> blob, error;
    Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error),
          "simulation root signature serialization", context.Device());
    Check(context.Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                IID_PPV_ARGS(&root)),
          "simulation root signature", context.Device());
    ShaderCompiler compiler;
    const auto dxil = compiler.Compile(HostShaders::SIM_SERIES_S_HLSL, L"main", L"cs_6_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {dxil.data(), dxil.size()};
    Check(context.Device()->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)),
          "simulation compute pipeline", context.Device());

    for (auto& slot : slots) {
        slot.timestamps = context.CreateBuffer(MaxSegments * 16, D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC output_desc{};
        output_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        output_desc.Width = 8 + 128 * 64 * 4;
        output_desc.Height = 1;
        output_desc.DepthOrArraySize = 1;
        output_desc.MipLevels = 1;
        output_desc.SampleDesc.Count = 1;
        output_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        slot.output = context.CreateResource(output_desc, D3D12_HEAP_TYPE_DEFAULT,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot.readback = context.CreateBuffer(24, D3D12_HEAP_TYPE_READBACK);
    }
}

void SimSeriesS::ReadSample(Slot& slot) {
    if (!slot.pending)
        return;
    u64 stamps[2]{};
    u32 info[2]{};
    std::memcpy(stamps, slot.readback->mapped.data(), sizeof(stamps));
    std::memcpy(info, slot.readback->mapped.data() + 16, sizeof(info));
    const u32 iterations = info[0];
    if (stamps[1] > stamps[0] && iterations) {
        const double elapsed = static_cast<double>(stamps[1] - stamps[0]);
        const double measured = static_cast<double>(iterations) / elapsed;
        // Smooth changes in clocks/occupancy. This measures the entire fixed-size dispatch.
        iterations_per_tick =
            iterations_per_tick > 0.0 ? iterations_per_tick * 0.75 + measured * 0.25 : measured;
        ballast_ms_sum += elapsed * 1000.0 / static_cast<double>(frequency);
        ++samples;
        if (info[1])
            ++capped_frames;
    }
    slot.pending = false;
}

void SimSeriesS::Poll() {
    const auto completed = context.CompletedTick();
    for (auto& slot : slots)
        if (slot.pending && slot.tick <= completed)
            ReadSample(slot);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds{5}) {
        const auto& config = NxboxSimSeriesS::Settings();
        Diagnostic(fmt::format(
            "NXBOX SIM_SERIES_S gpu_factor={:.2f} cpu_factor={:.2f} ballast_ms_avg={:.3f} "
            "samples={} capped_frames={}",
            config.gpu_factor, config.cpu_factor,
            samples ? ballast_ms_sum / static_cast<double>(samples) : 0.0, samples, capped_frames));
        ballast_ms_sum = 0.0;
        samples = capped_frames = 0;
        last_log = now;
    }
}

void SimSeriesS::PrepareFrame() {
    if (!frame_open) {
        // Completion callbacks may start recording. Mark the frame first and wait before
        // Context resets a list, just like its allocator-ring reuse path.
        frame_open = true;
        auto& slot = slots[slot_index];
        // Query indices and mapped readback bytes cannot be overwritten while in flight.
        // Wait only on ring reuse, never to obtain the current frame's duration.
        if (slot.tick)
            context.WaitSubmitted(slot.tick, "simulation slot reuse");
        ReadSample(slot);
    }
}

void SimSeriesS::BeginSegment(CommandFacade& commands) {
    if (segments < MaxSegments) {
        commands.EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          slot_index * QueriesPerSlot + segments * 2);
        segment_open = true;
    }
}

void SimSeriesS::EndSegment(CommandFacade& commands) {
    if (!segment_open)
        return;
    commands.EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                      slot_index * QueriesPerSlot + segments * 2 + 1);
    ++segments;
    segment_open = false;
}

void SimSeriesS::EndFrame() {
    Poll();
    auto* commands = context.Commands();
    EndSegment(*commands);
    auto& slot = slots[slot_index];
    const u32 first = slot_index * QueriesPerSlot;
    const double factor = NxboxSimSeriesS::Settings().gpu_factor;
    if (factor > 1.0 && segments) {
        context.MarkWork("Series S simulation");
        context.Transition(slot.timestamps, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, segments * 2,
                                   slot.timestamps->handle.Get(), 0);
        context.Transition(slot.timestamps, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.Transition(slot.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->SetPipelineState(pipeline.Get());
        commands->SetComputeRootSignature(root.Get());
        struct Constants {
            u32 segments;
            float extra_factor;
            float iterations_per_tick;
            float maximum_ticks;
        } constants{segments, static_cast<float>(factor - 1.0),
                    static_cast<float>(iterations_per_tick), static_cast<float>(frequency * 0.1)};
        commands->SetComputeRoot32BitConstants(0, 4, &constants, 0);
        commands->SetComputeRootShaderResourceView(1,
                                                   slot.timestamps->handle->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(2, slot.output->handle->GetGPUVirtualAddress());
        commands->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + MaxSegments * 2);
        commands->Dispatch(128, 1, 1);
        commands->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + MaxSegments * 2 + 1);
        commands->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + MaxSegments * 2,
                                   2, slot.readback->handle.Get(), 0);
        context.Transition(slot.output, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(slot.readback->handle.Get(), 16, slot.output->handle.Get(), 0,
                                   8);
        context.Retain(slot.timestamps);
        context.Retain(slot.output);
        context.Retain(slot.readback);
        slot.pending = true;
        context.InvalidateRecordedState();
    }
    if (segments == MaxSegments)
        Diagnostic(
            "NXBOX SIM_SERIES_S timestamp segment limit reached; frame may be underweighted");
    slot.tick = context.CurrentTick();
    slot_index = (slot_index + 1) % SlotCount;
    segments = 0;
    frame_open = false;
}
} // namespace D3D12
