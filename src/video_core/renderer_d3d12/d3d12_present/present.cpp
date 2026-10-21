// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include "common/fps_overlay.h"
#include "common/logging.h"
#include "d3d12_present_shader_source.h"
#include "video_core/renderer_d3d12/d3d12_present/fps_bitmap.h"
#include "video_core/renderer_d3d12/d3d12_present/present.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12::Present {
namespace {
constexpr unsigned MaxPasses = 256; // Up to four passes for each of the renderer's 64 layers.
constexpr unsigned FrameSlots = 2;
constexpr unsigned PassCount = 9;
constexpr unsigned FxaaPass = 6, EasuPass = 7;
constexpr DXGI_FORMAT IntermediateFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
struct Constants {
    Crop crop;
    float source_width, source_height, output_width, output_height;
    unsigned transform{}, opaque{};
    float sharpness{};
    unsigned unused{};
};
static_assert(sizeof(Constants) == 48);
D3D12_SHADER_RESOURCE_VIEW_DESC View(Extent) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = IntermediateFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = 1;
    return view;
}
} // namespace
struct Presenter::Impl {
    Context& context;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> srv_heap, rtv_heap;
    // Final passes use RGBA8 with each layer blend; preprocessing uses float RGBA.
    std::array<std::array<ComPtr<ID3D12PipelineState>, 3>, 6> final;
    std::array<ComPtr<ID3D12PipelineState>, PassCount> intermediate;
    std::array<std::array<ResourcePtr, 2>, FrameSlots> scratch;
    std::array<std::array<Extent, 2>, FrameSlots> scratch_extent{};
    unsigned frame_index{};
    unsigned srv_stride{}, rtv_stride{}, cursor{};
    bool begun{};

    explicit Impl(Context& context_) : context{context_} {
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
        Check(context.Device()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model,
                                                    sizeof(model)),
              "Presentation shader model support");
        if (model.HighestShaderModel < D3D_SHADER_MODEL_6_0)
            throw std::runtime_error("Presentation requires shader model 6.0");
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = {1, &range};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = {0, 0, sizeof(Constants) / 4};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root_desc{2, parameters, 1, &sampler,
                                            D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> serialized, errors;
        Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                          &errors),
              "Serialize presentation root");
        Check(context.Device()->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                    serialized->GetBufferSize(),
                                                    IID_PPV_ARGS(&root)),
              "Create presentation root");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = MaxPasses * FrameSlots;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(context.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap)),
              "Create presentation SRVs");
        srv_stride = context.Device()->GetDescriptorHandleIncrementSize(heap.Type);
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = MaxPasses * FrameSlots;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check(context.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap)),
              "Create presentation RTVs");
        rtv_stride = context.Device()->GetDescriptorHandleIncrementSize(heap.Type);
        ShaderCompiler compiler;
        const std::string source = BuildShaderSource();
        const auto vertex = compiler.Compile(source, L"VS", L"vs_6_0");
        constexpr std::array entries{L"Bilinear", L"Bicubic", L"Gaussian", L"ScaleForce", L"Area",
                                     L"Rcas",     L"Fxaa",    L"Easu",     L"Rcas"};
        for (unsigned pass = 0; pass < PassCount; ++pass) {
            const auto pixel = compiler.Compile(source, entries[pass], L"ps_6_0");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = root.Get();
            desc.VS = {vertex.data(), vertex.size()};
            desc.PS = {pixel.data(), pixel.size()};
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable = TRUE;
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            desc.DepthStencilState.StencilReadMask = desc.DepthStencilState.StencilWriteMask = 0xff;
            desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
                D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                D3D12_COMPARISON_FUNC_ALWAYS};
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = IntermediateFormat;
            desc.SampleDesc.Count = 1;
            auto& blend = desc.BlendState.RenderTarget[0];
            blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            Check(context.Device()->CreateGraphicsPipelineState(&desc,
                                                                IID_PPV_ARGS(&intermediate[pass])),
                  "Create presentation intermediate PSO");
            if (pass >= final.size())
                continue;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            for (unsigned mode = 0; mode < 3; ++mode) {
                blend.BlendEnable = mode != 0;
                blend.SrcBlend = mode == 2 ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
                Check(context.Device()->CreateGraphicsPipelineState(
                          &desc, IID_PPV_ARGS(&final[pass][mode])),
                      "Create presentation final PSO");
            }
        }
    }
    ResourcePtr Scratch(unsigned index, Extent size) {
        if (!scratch[frame_index][index] || scratch_extent[frame_index][index] != size) {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = size.width;
            desc.Height = size.height;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = IntermediateFormat;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            // Context accounts committed allocations against the shared 512 MiB cap
            // and retains replaced images until their recorded uses complete.
            scratch[frame_index][index].reset();
            scratch[frame_index][index] = context.CreateResource(
                desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            scratch_extent[frame_index][index] = size;
        }
        return scratch[frame_index][index];
    }
    void Draw(const ResourcePtr& source, const D3D12_SHADER_RESOURCE_VIEW_DESC& view,
              D3D12_CPU_DESCRIPTOR_HANDLE target, D3D12_VIEWPORT viewport, D3D12_RECT scissor,
              Constants constants, ID3D12PipelineState* pipeline) {
        if (cursor == (frame_index + 1) * MaxPasses)
            throw std::runtime_error("Presentation descriptor capacity exceeded");
        context.Transition(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context.Retain(source);
        auto cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        auto gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(cursor) * srv_stride;
        gpu.ptr += static_cast<UINT64>(cursor++) * srv_stride;
        context.ValidateViewFormat(source, view.Format, "CreateShaderResourceView");
        context.Device()->CreateShaderResourceView(source->handle.Get(), &view, cpu);
        context.InvalidateRecordedState();
        auto* commands = context.Commands();
        ID3D12DescriptorHeap* heaps[]{srv_heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootSignature(root.Get());
        commands->SetPipelineState(pipeline);
        commands->SetGraphicsRootDescriptorTable(0, gpu);
        commands->SetGraphicsRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
        commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (auto* profile = context.PassProfile()) {
            const bool preprocessing =
                std::any_of(intermediate.begin(), intermediate.end(),
                            [pipeline](const auto& pso) { return pso.Get() == pipeline; });
            profile->Label("present blit");
            profile->Target(
                {static_cast<u32>(viewport.Width), static_cast<u32>(viewport.Height),
                 static_cast<u32>(preprocessing ? IntermediateFormat : DXGI_FORMAT_R8G8B8A8_UNORM),
                 0, 0});
        }
        commands->DrawInstanced(3, 1, 0, 0);
    }
    ResourcePtr Preprocess(unsigned pass, unsigned scratch_index, const ResourcePtr& source,
                           const D3D12_SHADER_RESOURCE_VIEW_DESC& view, Extent output,
                           Constants constants) {
        auto destination = Scratch(scratch_index, output);
        context.Transition(destination, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto target = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        target.ptr += static_cast<SIZE_T>(cursor) * rtv_stride;
        context.Device()->CreateRenderTargetView(destination->handle.Get(), nullptr, target);
        constants.output_width = static_cast<float>(output.width);
        constants.output_height = static_cast<float>(output.height);
        Draw(source, view, target, {0, 0, constants.output_width, constants.output_height, 0, 1},
             {0, 0, static_cast<LONG>(output.width), static_cast<LONG>(output.height)}, constants,
             intermediate[pass].Get());
        return destination;
    }
};
Presenter::Presenter(Context& context) : impl{std::make_unique<Impl>(context)} {}
Presenter::~Presenter() {
    try {
        impl->context.Finish(FrameCount::ForcedSubmitLifecycle);
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12 presentation teardown: {}", error.what());
    }
}
void Presenter::BeginFrame(unsigned frame_index, u64 previous_tick) {
    if (frame_index >= FrameSlots)
        throw std::invalid_argument("Invalid presentation frame slot");
    impl->context.WaitSubmitted(previous_tick, "present upscale");
    impl->frame_index = frame_index;
    impl->cursor = frame_index * MaxPasses;
    impl->begun = true;
}
void Presenter::DrawLayer(const Layer& layer, D3D12_CPU_DESCRIPTOR_HANDLE target,
                          D3D12_VIEWPORT viewport, D3D12_RECT scissor, Options options) {
    if (!impl->begun)
        throw std::logic_error("BeginFrame must precede presentation draws");
    if (!layer.source || !layer.source->handle ||
        layer.view.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY ||
        layer.view.Texture2DArray.ArraySize != 1 || layer.view.Texture2DArray.PlaneSlice != 0)
        throw std::invalid_argument("Presentation requires one color Texture2DArray slice");
    if (!std::isfinite(viewport.Width) || !std::isfinite(viewport.Height) || viewport.Width < 1 ||
        viewport.Height < 1 || viewport.Width > 3840 || viewport.Height > 2160)
        throw std::invalid_argument("Invalid presentation viewport");
    const Extent output{static_cast<unsigned>(viewport.Width),
                        static_cast<unsigned>(viewport.Height)};
    ValidateExtent(output, true);
    const auto pass = static_cast<unsigned>(options.filter);
    const auto blend = static_cast<unsigned>(layer.blend);
    if (pass >= 6 || blend >= 3 || layer.transform > 7)
        throw std::invalid_argument("Invalid presentation options");
    RcasStrength(options.sharpness); // Validate even when FSR is not selected.
    auto source = layer.source;
    auto view = layer.view;
    view.Texture2DArray.MipLevels = 1;
    const auto desc = source->handle->GetDesc();
    const unsigned mip = view.Texture2DArray.MostDetailedMip;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        mip >= desc.MipLevels || view.Texture2DArray.FirstArraySlice >= desc.DepthOrArraySize)
        throw std::invalid_argument("Invalid presentation source subresource");
    const Extent extent{static_cast<unsigned>(std::max(UINT64{1}, desc.Width >> mip)),
                        std::max(1u, desc.Height >> mip)};
    const auto crop = NormalizeCrop(
        layer.crop_pixels.left == 0 && layer.crop_pixels.top == 0 && layer.crop_pixels.right == 0 &&
                layer.crop_pixels.bottom == 0
            ? Crop{0, 0, float(layer.guest_extent.width), float(layer.guest_extent.height)}
            : layer.crop_pixels,
        layer.guest_extent);
    const bool upscale =
        options.filter == Filter::Fsr && NeedsUpscale(extent, output, crop, layer.transform);
    const unsigned needed = 1 + unsigned(options.fxaa) + (upscale ? 1 : 0);
    if (needed > (impl->frame_index + 1) * MaxPasses - impl->cursor)
        throw std::runtime_error("Presentation descriptor capacity exceeded");
    Constants constants{{},
                        float(extent.width),
                        float(extent.height),
                        float(output.width),
                        float(output.height),
                        0,
                        0,
                        std::clamp(options.sharpness, 0.f, 1.f),
                        0};
    if (options.fxaa) {
        source = impl->Preprocess(FxaaPass, 0, source, view, extent, constants);
        view = View(extent);
    }
    constants.crop = crop;
    if (upscale) {
        // Rotation is applied only in the final composition; upscale in source axes.
        const Extent fsr_extent =
            (layer.transform & 4) ? Extent{output.height, output.width} : output;
        source = impl->Preprocess(EasuPass, 1, source, view, fsr_extent, constants);
        view = View(fsr_extent);
        constants.crop = {};
        constants.source_width = float(fsr_extent.width);
        constants.source_height = float(fsr_extent.height);
        // RCAS is the final blended pass; avoid a second full-size image and copy.
    }
    constants.output_width = float(output.width);
    constants.output_height = float(output.height);
    constants.transform = layer.transform;
    constants.opaque = layer.blend == Blend::Opaque;
    const unsigned final_pass = options.filter == Filter::Fsr && !upscale ? 0 : pass;
    impl->Draw(source, view, target, viewport, scissor, constants,
               impl->final[final_pass][blend].Get());
}
HRESULT Presenter::Present(IDXGISwapChain3* swapchain, bool vsync, unsigned interval) {
    if (!swapchain)
        throw std::invalid_argument("Missing presentation swapchain");
    impl->begun = false;
    return swapchain->Present(SyncInterval(vsync, interval), 0);
}
namespace {
constexpr std::string_view FpsShader = R"(
cbuffer Parameters : register(b0) {
    uint4 glyphs[8];
    float2 size;
    uint glyph_count;
    float scale;
};
struct Vertex { float4 position : SV_Position; };
Vertex VS(uint id : SV_VertexID) {
    Vertex result;
    float2 uv = float2((id << 1) & 2, id & 2);
    result.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    return result;
}
float4 PS(Vertex input) : SV_Target {
    // Viewport origin is eight bitmap pixels from the display's top-left corner.
    float2 p = input.position.xy / scale - 8;
    float2 extent = size / scale;
    float radius = extent.y * .5;
    float2 q = abs(p - extent * .5) - float2(extent.x * .5 - radius, 0);
    float distance = length(max(q, 0)) + min(max(q.x, q.y), 0) - radius;
    if (distance > 0) discard;
    int2 cell = int2(floor(p - float2(8, 4)));
    if (cell.x >= 0 && cell.x < int(glyph_count * 5) && cell.y >= 0 && cell.y < 7) {
        uint index = uint(cell.x) / 5;
        uint column = uint(cell.x) % 5;
        uint bitmap = glyphs[index / 4][index % 4];
        if (column < 4 && ((bitmap >> (uint(cell.y) * 4 + column)) & 1))
            return float4(1, 1, 1, 1);
    }
    return float4(0, 0, 0, .72);
}
)";
struct FpsConstants {
    std::array<std::uint32_t, 32> glyphs{};
    float width{}, height{};
    unsigned length{};
    float scale{};
};
static_assert(sizeof(FpsConstants) == 36 * 4);
} // namespace
struct FpsCounter::Impl {
    Context& context;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    Common::FpsOverlay::Sample sample;

    explicit Impl(Context& context_) : context{context_} {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0, 0, sizeof(FpsConstants) / 4};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root_desc{1, &parameter, 0, nullptr,
                                            D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> serialized, errors;
        Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                          &errors),
              "Serialize FPS root");
        Check(context.Device()->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                    serialized->GetBufferSize(),
                                                    IID_PPV_ARGS(&root)),
              "Create FPS root");
        ShaderCompiler compiler;
        const auto vertex = compiler.Compile(FpsShader, L"VS", L"vs_6_0");
        const auto pixel = compiler.Compile(FpsShader, L"PS", L"ps_6_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.VS = {vertex.data(), vertex.size()};
        desc.PS = {pixel.data(), pixel.size()};
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.DepthStencilState.StencilReadMask = desc.DepthStencilState.StencilWriteMask = 0xff;
        desc.DepthStencilState.FrontFace =
            desc.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                               D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        Check(context.Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)),
              "Create FPS pipeline");
    }
    void Draw(D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height) {
        using namespace Common::FpsOverlay;
        sample.Update(Sample::Clock::now(), guest_presents.load(std::memory_order_relaxed),
                      generation.load(std::memory_order_relaxed));
        // Integer formatting keeps the decimal point stable regardless of the user's locale.
        const auto fps = static_cast<unsigned>(std::clamp(std::round(sample.fps), 0.0, 9999.0));
        const auto tenths =
            static_cast<unsigned>(std::clamp(std::round(sample.milliseconds * 10), 0.0, 999999.0));
        std::array<char, 32> text{};
        std::snprintf(text.data(), text.size(), "%u FPS  %u.%u ms", fps, tenths / 10, tenths % 10);
        FpsConstants constants;
        constants.length = static_cast<unsigned>(std::char_traits<char>::length(text.data()));
        for (unsigned i = 0; i < constants.length; ++i)
            constants.glyphs[i] = FpsGlyph(text[i]);
        constants.scale = std::max(1.f, std::min(width / 1920.f, height / 1080.f) * 3.f);
        constants.width = (16 + constants.length * 5) * constants.scale;
        constants.height = 15 * constants.scale;
        const float margin = 8 * constants.scale;
        const D3D12_VIEWPORT viewport{margin, margin, constants.width, constants.height, 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        context.InvalidateRecordedState();
        auto* commands = context.Commands();
        commands->SetGraphicsRootSignature(root.Get());
        commands->SetPipelineState(pipeline.Get());
        commands->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
        commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
    }
};
FpsCounter::FpsCounter(Context& context) : impl{std::make_unique<Impl>(context)} {}
FpsCounter::~FpsCounter() {
    try {
        impl->context.Finish(FrameCount::ForcedSubmitLifecycle);
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12 FPS teardown: {}", error.what());
    }
}
void FpsCounter::Draw(D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height) {
    impl->Draw(target, width, height);
}

} // namespace D3D12::Present
