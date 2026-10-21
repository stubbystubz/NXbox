// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstring>
#include "d3d12_present_shader_source.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12::Present {
struct FrameGenerator::Impl {
    Context& context;
    FrameGenerationPolicy policy;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::array<ComPtr<ID3D12PipelineState>, 4> pipelines;
    ResourcePtr previous, current, real, output, luma, motion, stats, readback, zero;
    unsigned width{}, height{}, bx{}, by{};
    unsigned output_width{}, output_height{}, source_width{}, source_height{};
    bool history{};
    explicit Impl(Context& context_) : context{context_} {}
    void Initialize() {
        if (root)
            return;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 5};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = {2, ranges};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = {0, 0, 6};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC desc{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, errors;
        Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
              "Frame generation root serialization");
        Check(context.Device()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                    blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "Frame generation root");
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 10;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(context.Device()->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)),
              "Frame generation descriptors");
        ShaderCompiler compiler;
        const auto source = BuildFrameGenerationShaderSource();
        constexpr std::array entries{L"LumaCS", L"MotionCS", L"WarpCS", L"DownsampleCS"};
        for (unsigned i = 0; i < entries.size(); ++i) {
            const auto shader = compiler.Compile(source, entries[i], L"cs_6_0");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = root.Get();
            pso.CS = {shader.data(), shader.size()};
            Check(context.Device()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipelines[i])),
                  "Frame generation compute PSO");
        }
    }
    ResourcePtr Texture(unsigned w, unsigned h, DXGI_FORMAT format, bool uav = true) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        return context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    }
    void ReleaseResources() noexcept {
        previous.reset();
        current.reset();
        real.reset();
        output.reset();
        luma.reset();
        motion.reset();
        stats.reset();
        readback.reset();
        zero.reset();
    }
    void Allocate(unsigned w, unsigned h) {
        const auto extent = CheaperHistoryExtent(w, h, source_width, source_height);
        if (output_width == w && output_height == h && width == extent.width &&
            height == extent.height)
            return;
        history = false;
        previous.reset();
        current.reset();
        real.reset();
        output.reset();
        luma.reset();
        motion.reset();
        bx = (extent.width + 7) / 8;
        by = (extent.height + 7) / 8;
        previous = Texture(extent.width, extent.height, DXGI_FORMAT_R8G8B8A8_UNORM);
        current = Texture(extent.width, extent.height, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (extent.width != w || extent.height != h)
            real = Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
        output = Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
        luma = Texture(bx, by, DXGI_FORMAT_R32G32_FLOAT);
        motion = Texture(bx, by, DXGI_FORMAT_R32G32B32A32_FLOAT);
        if (!stats) {
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = 256;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            stats = context.CreateResource(buffer, D3D12_HEAP_TYPE_DEFAULT,
                                           D3D12_RESOURCE_STATE_COMMON);
            readback = context.CreateBuffer(256, D3D12_HEAP_TYPE_READBACK);
            zero = context.CreateBuffer(256, D3D12_HEAP_TYPE_UPLOAD);
            std::memset(zero->mapped.data(), 0, 256);
        }
        width = extent.width;
        height = extent.height;
        output_width = w;
        output_height = h;
    }
    void Views() {
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        const auto stride = context.Device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (const auto& resource : {previous, current, luma, motion, real ? real : current}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = resource->handle->GetDesc().Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            context.ValidateViewFormat(resource, view.Format, "Frame generation SRV");
            context.Device()->CreateShaderResourceView(resource->handle.Get(), &view, cpu);
            cpu.ptr += stride;
        }
        for (const auto& resource : {luma, motion, output, stats, current}) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            if (resource == stats) {
                view.Format = DXGI_FORMAT_R32_TYPELESS;
                view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                view.Buffer.NumElements = 64;
                view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            } else {
                view.Format = resource->handle->GetDesc().Format;
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            }
            context.Device()->CreateUnorderedAccessView(resource->handle.Get(), nullptr, &view,
                                                        cpu);
            cpu.ptr += stride;
        }
    }
    void Copy(const ResourcePtr& destination, const ResourcePtr& source) {
        context.Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
        context.Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST);
        context.Retain(source);
        context.Retain(destination);
        context.Commands()->CopyResource(destination->handle.Get(), source->handle.Get());
    }
    void Dispatch(unsigned pass, unsigned w, unsigned h) {
        context.InvalidateRecordedState();
        context.MarkWork("frame generation");
        auto* commands = context.Commands();
        ID3D12DescriptorHeap* heaps[]{heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(root.Get());
        commands->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        const std::array<unsigned, 6> constants{width, height, bx, by, output_width, output_height};
        commands->SetComputeRoot32BitConstants(1, 6, constants.data(), 0);
        commands->SetPipelineState(pipelines[pass].Get());
        if (auto* profile = context.PassProfile()) {
            constexpr std::array labels{"frame generation luma", "frame generation motion",
                                        "frame generation warp",
                                        "frame generation history downsample"};
            profile->Label(labels[pass]);
            profile->Target({w, h, static_cast<u32>(output->handle->GetDesc().Format), 0, 0});
        }
        commands->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
    ResourcePtr Generate(const ResourcePtr& source, double seconds) {
        if (seconds > 0 && !policy.CanGenerate(true, seconds, 0)) {
            history = false;
            return {}; // Fast games do not copy history or wait for GPU completion.
        }
        Initialize();
        // Single private descriptor set, reused only after all prior dispatches complete.
        context.Finish(FrameCount::ForcedSubmitPresent);
        const auto desc = source->handle->GetDesc();
        Allocate(static_cast<unsigned>(desc.Width), desc.Height);
        if (history)
            std::swap(previous, current);
        Views();
        if (real) {
            Copy(real, source);
            context.Transition(real, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.Transition(current, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Dispatch(3, width, height);
            context.GlobalUavBarrier();
        } else {
            Copy(current, source);
        }
        const bool had_history = history;
        history = true;
        if (!policy.CanGenerate(had_history, seconds, 0))
            return {};
        for (const auto& resource :
             {previous, current, output, luma, motion, stats, readback, zero})
            context.Retain(resource);
        context.Transition(previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.Transition(current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Copy(stats, zero);
        context.Transition(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.Transition(luma, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Dispatch(0, bx, by);
        context.GlobalUavBarrier();
        context.Transition(stats, D3D12_RESOURCE_STATE_COPY_SOURCE);
        context.Commands()->CopyBufferRegion(readback->handle.Get(), 0, stats->handle.Get(), 0, 4);
        context.Finish(FrameCount::ForcedSubmitPresent);
        std::uint32_t sum{};
        std::memcpy(&sum, readback->mapped.data(), sizeof(sum));
        if (!policy.CanGenerate(true, seconds, double(sum) / (4095.0 * bx * by)))
            return {};
        // The readback wait retired the luma batch. Retain both warp sources in
        // this new batch too, including if recording fails before the final copy.
        context.Retain(previous);
        context.Retain(current);
        context.Transition(luma, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.Transition(motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Dispatch(1, bx, by);
        context.GlobalUavBarrier();
        context.Transition(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Dispatch(2, output_width, output_height);
        context.GlobalUavBarrier();
        return output;
    }
};
FrameGenerator::FrameGenerator(Context& context) : impl{std::make_unique<Impl>(context)} {}
FrameGenerator::~FrameGenerator() = default;
ResourcePtr FrameGenerator::Generate(const ResourcePtr& source, double seconds) noexcept {
    if (impl->policy.failed)
        return {};
    try {
        return impl->Generate(source, seconds);
    } catch (const std::exception&) {
        if (impl->policy.Fail())
            Diagnostic("D3D12 frame generation disabled for session: shader/PSO/resource "
                       "preparation failed");
    } catch (...) {
        if (impl->policy.Fail())
            Diagnostic("D3D12 frame generation disabled for session: unknown failure");
    }
    // Context retains recorded uses until completion. Release unused/partial
    // allocations immediately so a failed effect cannot starve the real renderer.
    // Root, heap and PSOs stay alive until renderer teardown drains the queue.
    impl->ReleaseResources();
    return {};
}
void FrameGenerator::SetSourceExtent(unsigned width, unsigned height) {
    impl->source_width = width;
    impl->source_height = height;
}
ResourcePtr FrameGenerator::Current() const {
    return impl->real ? impl->real : impl->current;
}
} // namespace D3D12::Present
