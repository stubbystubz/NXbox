// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include "d3d12_present_shader_source.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation.h"
#include "video_core/renderer_d3d12/d3d12_present/temporal_upscaler.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"
namespace D3D12::Present {
struct TemporalUpscaler::Impl {
    Context& context;
    FrameGenerator flow;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    struct Slot {
        ComPtr<ID3D12DescriptorHeap> heap;
        u64 tick{};
    };
    std::array<Slot, 3> slots;
    std::array<ResourcePtr, 2> history;
    unsigned width{}, height{}, source_width{}, source_height{}, write_index{};
    bool valid{}, failed{}, flow_valid{};
    explicit Impl(Context& context_) : context{context_}, flow{context_} {}
    void Initialize() {
        if (root)
            return;
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0},
                                         {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 5}};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = {2, ranges};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = {0, 0, 5};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC desc{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, errors;
        Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
              "Temporal root serialization");
        Check(context.Device()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                    blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "Temporal root");
        ShaderCompiler compiler;
        const auto shader =
            compiler.Compile(BuildTemporalUpscalerShaderSource(), L"TemporalCS", L"cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root.Get();
        pso.CS = {shader.data(), shader.size()};
        Check(context.Device()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)),
              "Temporal pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 6;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        for (auto& slot : slots)
            Check(context.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot.heap)),
                  "Temporal descriptors");
    }
    ResourcePtr Upscale(const ResourcePtr& source, const ResourcePtr& spatial, unsigned w,
                        unsigned h) {
        Initialize();
        flow_valid = false;
        const auto source_desc = source->handle->GetDesc();
        Slot* available = nullptr;
        for (auto& slot : slots)
            if (!slot.tick || context.IsFree(slot.tick)) {
                available = &slot;
                break;
            }
        if (!available) {
            valid = false;
            return {}; // No CPU/GPU fence wait for an optional effect.
        }
        auto& slot = *available;
        if (width != w || height != h || source_width != source_desc.Width ||
            source_height != source_desc.Height) {
            valid = false;
            width = w;
            height = h;
            source_width = static_cast<unsigned>(source_desc.Width);
            source_height = source_desc.Height;
            D3D12_RESOURCE_DESC desc = source_desc;
            desc.Width = w;
            desc.Height = h;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            for (auto& image : history) {
                image.reset();
                image = context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_STATE_COMMON);
            }
        }
        const bool motion_valid = flow.Analyze(source);
        flow_valid = motion_valid;
        if (!flow.Motion() || !flow.Previous()) {
            valid = false;
            failed = true;
            history = {};
            Diagnostic("D3D12 temporal upscaler disabled for session: motion analysis failed");
            return {};
        }
        const auto destination = history[write_index];
        const auto previous = history[write_index ^ 1];
        auto cpu = slot.heap->GetCPUDescriptorHandleForHeapStart();
        const auto stride = context.Device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (const auto& image : {source, previous, flow.Motion(), flow.Previous(), spatial}) {
            context.Transition(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.Retain(image);
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = image->handle->GetDesc().Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            context.ValidateViewFormat(image, view.Format, "Temporal SRV");
            context.Device()->CreateShaderResourceView(image->handle.Get(), &view, cpu);
            cpu.ptr += stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = destination->handle->GetDesc().Format;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        context.Device()->CreateUnorderedAccessView(destination->handle.Get(), nullptr, &view, cpu);
        context.Transition(destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.Retain(destination);
        context.InvalidateRecordedState();
        context.MarkWork("temporal upscale");
        auto* commands = context.Commands();
        ID3D12DescriptorHeap* heaps[]{slot.heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(root.Get());
        commands->SetComputeRootDescriptorTable(0, slot.heap->GetGPUDescriptorHandleForHeapStart());
        const std::array<unsigned, 5> constants{source_width, source_height, w, h,
                                                unsigned(valid && motion_valid)};
        commands->SetComputeRoot32BitConstants(1, 5, constants.data(), 0);
        commands->SetPipelineState(pipeline.Get());
        if (auto* profile = context.PassProfile()) {
            profile->Label("temporal accumulate");
            profile->Target({w, h, static_cast<u32>(view.Format), 0, 0});
        }
        commands->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        slot.tick = context.CurrentTick();
        context.GlobalUavBarrier();
        valid = true;
        write_index ^= 1;
        return destination;
    }
};
TemporalUpscaler::TemporalUpscaler(Context& context) : impl{std::make_unique<Impl>(context)} {}
TemporalUpscaler::~TemporalUpscaler() = default;
void TemporalUpscaler::Reset() {
    impl->valid = impl->flow_valid = false;
    impl->flow.Reset();
}
ResourcePtr TemporalUpscaler::Motion() const {
    return impl->flow_valid && !impl->failed ? impl->flow.Motion() : ResourcePtr{};
}
ResourcePtr TemporalUpscaler::Upscale(const ResourcePtr& source, const ResourcePtr& spatial,
                                      unsigned w, unsigned h) noexcept {
    if (impl->failed)
        return {};
    try {
        return impl->Upscale(source, spatial, w, h);
    } catch (...) {
        impl->failed = true;
        impl->history = {};
        Diagnostic("D3D12 temporal upscaler disabled for session: shader/PSO/resource failure");
        return {};
    }
}
} // namespace D3D12::Present
