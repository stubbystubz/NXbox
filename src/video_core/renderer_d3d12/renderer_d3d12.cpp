// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <dxgi1_4.h>
#include <windows.h>
#ifdef NXBOX_UWP
#include <winrt/Windows.System.Profile.h>
#endif
#include "common/fps_overlay.h"
#include "common/logging.h"
#include "common/maxwell_method_profile.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "eden_uwp/renderer_boundary.h"
#include "eden_uwp/thread_profile.h"
#include "video_core/capture.h"
#include "video_core/renderer_d3d12/d3d12_output_policy.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_pacing_policy.h"
#include "video_core/renderer_d3d12/d3d12_present/present.h"
#include "video_core/renderer_d3d12/d3d12_present/present_policy.h"
#include "video_core/renderer_d3d12/d3d12_resolution.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12 {
namespace {
constexpr std::string_view PresentShader = R"(
cbuffer Parameters : register(b0) { float4 crop; uint transform; uint opaque; float2 unused; };
Texture2DArray<float4> source : register(t0);
SamplerState linear_sampler : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    Vertex result;
    float2 uv = float2((id << 1) & 2, id & 2);
    result.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    if (transform & 4) uv = float2(uv.y, 1 - uv.x);
    if (transform & 1) uv.x = 1 - uv.x;
    if (transform & 2) uv.y = 1 - uv.y;
    result.uv = lerp(crop.xy, crop.zw, uv);
    return result;
}
float4 PS(Vertex input) : SV_Target {
    float4 color = source.Sample(linear_sampler, float3(input.uv, 0));
    if (opaque) color.a = 1;
    return color;
}
)";
struct PresentParameters {
    std::array<float, 4> crop;
    u32 transform;
    u32 opaque;
    std::array<float, 2> unused{};
};
static_assert(sizeof(PresentParameters) == 32);
} // namespace
struct RendererD3D12::Impl {
    // Reserve descriptor/backbuffer storage for either policy; capture uses the next slice.
    static constexpr UINT BufferCount = Present::MaxFrameSlots;
    const UINT active_buffers = Present::FrameSlotCount();
    static constexpr UINT MaxLayers = 64;
    D3D12::Context context;
    ComPtr<IDXGISwapChain3> swapchain;
    std::unique_ptr<Present::Presenter> upscaler;
    std::unique_ptr<Present::FpsCounter> fps_counter;
    std::unique_ptr<Present::FrameGenerator> frame_generator;
    bool frame_pacing_enabled{};
    Present::FramePacingPolicy frame_pacing;
    std::chrono::steady_clock::time_point previous_pacing_frame{};
    std::chrono::steady_clock::time_point previous_present_return{};
    DXGI_FRAME_STATISTICS previous_statistics{};
    bool have_statistics{};
    bool frame_generation_enabled{};
    unsigned frame_generation_mode{}, effective_frame_generation_mode{2};
    bool frame_generation_failed{};
    Present::FrameGenerationCadence frame_generation_cadence;
    std::chrono::steady_clock::time_point previous_game_frame{};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 3> pipelines;
    std::array<ComPtr<ID3D12PipelineState>, 3> screenshot_pipelines;
    std::array<ResourcePtr, BufferCount> buffers;
    std::array<u64, BufferCount> present_ticks{};
    u64 present_sequence{};
    UINT rtv_stride{};
    UINT srv_stride{};
    bool series_s{};
    std::string output_environment;
    OutputExtent OutputSize(u32 requested_width, u32 requested_height) const {
        const auto size =
            ChooseOutputExtent({requested_width, requested_height}, series_s, output_environment);
        if (size.width != width || size.height != height)
            Diagnostic("D3D12 output requested=" + std::to_string(requested_width) + "x" +
                       std::to_string(requested_height) + " chosen=" + std::to_string(size.width) +
                       "x" + std::to_string(size.height) + " series_s=" + std::to_string(series_s) +
                       " NXBOX_SERIES_S_1080P=" + (output_environment == "0" ? "0" : "1"));
        return size;
    }
    u32 width{};
    u32 height{};
    std::atomic<bool> failed{};

    ~Impl() {
        // Drain before destroying swapchain buffers, descriptors or PSOs.
        try {
            context.Finish(FrameCount::ForcedSubmitLifecycle);
        } catch (const std::exception& error) {
            context.ReportFailure(error);
        } catch (...) {
            Diagnostic("D3D12 renderer teardown: non-standard exception");
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index) const {
        auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * rtv_stride;
        return handle;
    }
    void AcquireBuffers() {
        for (UINT i = 0; i < active_buffers; ++i) {
            buffers[i] = std::make_shared<Resource>();
            buffers[i]->heap = D3D12_HEAP_TYPE_DEFAULT;
            buffers[i]->state = D3D12_RESOURCE_STATE_PRESENT;
            Check(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]->handle)), "GetBuffer",
                  context.Device());
            context.Device()->CreateRenderTargetView(buffers[i]->handle.Get(), nullptr, Rtv(i));
        }
    }
    void InitializePipeline() {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = {1, &range};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = {0, 0, 8};
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
              "SerializeRootSignature", context.Device());
        Check(context.Device()->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                    serialized->GetBufferSize(),
                                                    IID_PPV_ARGS(&root)),
              "CreateRootSignature", context.Device());
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
        Check(context.Device()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model,
                                                    sizeof(model)),
              "shader model support", context.Device());
        if (model.HighestShaderModel < D3D_SHADER_MODEL_6_0)
            throw std::runtime_error("D3D12 presentation requires shader model 6.0");
        ShaderCompiler compiler;
        const auto vertex = compiler.Compile(PresentShader, L"VS", L"vs_6_0");
        const auto pixel = compiler.Compile(PresentShader, L"PS", L"ps_6_0");
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
        desc.SampleDesc.Count = 1;
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        for (UINT format_index = 0; format_index < 2; ++format_index) {
            desc.RTVFormats[0] =
                format_index == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            auto& target_pipelines = format_index == 0 ? pipelines : screenshot_pipelines;
            for (UINT i = 0; i < target_pipelines.size(); ++i) {
                blend.BlendEnable = i != 0;
                blend.SrcBlend = i == 2 ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
                CheckPipeline(context.Device()->CreateGraphicsPipelineState(
                                  &desc, IID_PPV_ARGS(&target_pipelines[i])),
                              "Create present PSO", context.Device(),
                              Pso::HashBytes(desc.PS.pShaderBytecode, desc.PS.BytecodeLength));
            }
        }
    }
    void Initialize(Core::Frontend::EmuWindow& window) {
        char show_fps[16]{};
        GetEnvironmentVariableA("NXBOX_SHOW_FPS", show_fps, sizeof(show_fps));
        if (Common::FpsOverlay::EnvironmentForces(show_fps))
            Common::FpsOverlay::Initialize(true, show_fps);
        char frame_gen[16]{}, frame_gen_setting[16]{};
        if (GetEnvironmentVariableA("NXBOX_FRAME_GEN", frame_gen, sizeof(frame_gen)) != 1)
            frame_gen[0] = 0;
        if (GetEnvironmentVariableA("NXBOX_FRAME_GEN_SETTING", frame_gen_setting,
                                    sizeof(frame_gen_setting)) != 1)
            frame_gen_setting[0] = 0;
        frame_generation_mode = Present::FrameGenerationMode(
            Present::FrameGenerationMode(0, frame_gen_setting), frame_gen);
        frame_generation_enabled = frame_generation_mode != 0;
        Common::FpsOverlay::frame_generation.store(frame_generation_enabled);
        char pacing[16]{}, pacing_setting[16]{};
        if (GetEnvironmentVariableA("NXBOX_FRAME_PACING", pacing, sizeof(pacing)) != 1)
            pacing[0] = 0;
        if (GetEnvironmentVariableA("NXBOX_FRAME_PACING_SETTING", pacing_setting,
                                    sizeof(pacing_setting)) != 1)
            pacing_setting[0] = 0;
        frame_pacing_enabled =
            Present::FramePacingEnabled(std::string_view(pacing_setting) == "1", pacing);
        const auto& info = window.GetWindowInfo();
        if (info.type != Core::Frontend::WindowSystemType::Windows || !info.render_surface) {
#ifdef NXBOX_UWP
            throw std::runtime_error("D3D12 requires a UWP CoreWindow ABI surface");
#else
            throw std::runtime_error("D3D12 requires a Windows HWND surface");
#endif
        }
        ComPtr<IDXGIFactory2> factory;
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2",
              context.Device());
#ifdef NXBOX_UWP
        try {
            const auto form =
                winrt::to_string(winrt::Windows::System::Profile::AnalyticsInfo::DeviceForm());
            series_s = IsSeriesS(form);
            Diagnostic("D3D12 output device_form=" + form);
        } catch (const winrt::hresult_error&) {
            Diagnostic("D3D12 output device_form unavailable; output cap not applied");
        }
#endif
        char output_cap[16]{};
        if (GetEnvironmentVariableA("NXBOX_SERIES_S_1080P", output_cap, sizeof(output_cap)) != 1)
            output_cap[0] = 0;
        output_environment = output_cap;
        const auto size =
            OutputSize(window.GetFramebufferLayout().width, window.GetFramebufferLayout().height);
        width = size.width;
        height = size.height;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = active_buffers;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> chain;
#ifdef NXBOX_UWP
        Check(factory->CreateSwapChainForCoreWindow(context.Queue(),
                                                    static_cast<IUnknown*>(info.render_surface),
                                                    &desc, nullptr, &chain),
              "CreateSwapChainForCoreWindow", context.Device());
#else
        Check(factory->CreateSwapChainForHwnd(context.Queue(),
                                              static_cast<HWND>(info.render_surface), &desc,
                                              nullptr, nullptr, &chain),
              "CreateSwapChainForHwnd", context.Device());
        Check(factory->MakeWindowAssociation(static_cast<HWND>(info.render_surface),
                                             DXGI_MWA_NO_ALT_ENTER),
              "MakeWindowAssociation", context.Device());
#endif
        Check(chain.As(&swapchain), "Query IDXGISwapChain3", context.Device());
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = BufferCount + 1;
        Check(context.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap)),
              "Create RTV heap", context.Device());
        rtv_stride = context.Device()->GetDescriptorHandleIncrementSize(heap.Type);
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = MaxLayers * (BufferCount + 1);
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(context.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap)),
              "Create SRV heap", context.Device());
        srv_stride = context.Device()->GetDescriptorHandleIncrementSize(heap.Type);
        Diagnostic("D3D12 swapchain and descriptor heaps created; NXBOX_D3D12_PRESENT_FAST=" +
                   std::to_string(Present::FastEnabled()) +
                   " buffers=" + std::to_string(active_buffers));
        AcquireBuffers();
        InitializePipeline();
        context.LogInfoQueue();
        Diagnostic("D3D12 present PSOs ready");
        LOG_INFO(Render, "D3D12 initialized: native caches/framebuffer presentation; guest "
                         "pipeline path=Maxwell/HLSL/DXIL");
    }
    void Draw(const Layout::FramebufferLayout& layout,
              std::span<const Tegra::FramebufferConfig> layers, RasterizerD3D12& rasterizer,
              const ResourcePtr& render_target, D3D12_CPU_DESCRIPTOR_HANDLE target, UINT index,
              const std::array<ComPtr<ID3D12PipelineState>, 3>& target_pipelines) {
        const Common::MaxwellMethodProfile::NamedScope draw_profile{
            Common::MaxwellMethodProfile::PresentDraw};
        if (layers.size() > MaxLayers)
            throw std::runtime_error("D3D12 presentation layer limit exceeded");
        // Cache uploads/downloads may submit work. Resolve every source before binding
        // compositor state so allocator resets cannot erase an in-progress layer draw.
        std::vector<RasterizerD3D12::DisplayTexture> textures;
        textures.reserve(layers.size());
        for (const auto& layer : layers)
            textures.push_back(rasterizer.FramebufferTexture(layer));
        // Capture uses a BGRA target and keeps its existing presentation PSO.
        const bool use_upscaler =
            !layers.empty() && index < BufferCount && BisectEnabled("NXBOX_D3D12_FSR");
        if (use_upscaler) {
            if (!upscaler) {
                upscaler = std::make_unique<Present::Presenter>(context);
                Diagnostic("D3D12 FSR 1 EASU + RCAS ready");
            }
            upscaler->SetCompositeLayerCount(static_cast<unsigned>(layers.size()));
            upscaler->BeginFrame(index, present_ticks[index]);
        } else if (index < BufferCount) {
            context.WaitSubmitted(present_ticks[index], "present");
        }
        context.MarkWork(layers.empty() ? "init present clear" : "frame present clear/compositor");
        auto* commands = context.Commands();
        if (auto* profile = context.PassProfile()) {
            const auto desc = render_target->handle->GetDesc();
            profile->Target(
                {static_cast<u32>(desc.Width), desc.Height, static_cast<u32>(desc.Format), 0, 0});
        }
        context.Transition(render_target, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
        constexpr float Black[]{0, 0, 0, 1};
        commands->ClearRenderTargetView(target, Black, 0, nullptr);
        commands->SetGraphicsRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]{srv_heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto& screen = layout.screen;
        D3D12_VIEWPORT viewport{static_cast<float>(screen.left),
                                static_cast<float>(screen.top),
                                static_cast<float>(screen.GetWidth()),
                                static_cast<float>(screen.GetHeight()),
                                0,
                                1};
        D3D12_RECT scissor{static_cast<LONG>(screen.left), static_cast<LONG>(screen.top),
                           static_cast<LONG>(screen.right), static_cast<LONG>(screen.bottom)};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        UINT slot = index * MaxLayers;
        for (const auto& layer : layers) {
            const auto& texture = textures[slot - index * MaxLayers];
            if (!texture.resource) {
                ++slot;
                continue;
            }
            context.Transition(texture.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            auto cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
            auto gpu_handle = srv_heap->GetGPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(slot) * srv_stride;
            gpu_handle.ptr += static_cast<UINT64>(slot++) * srv_stride;
            context.ValidateViewFormat(texture.resource, texture.view.Format,
                                       "CreateShaderResourceView");
            context.Device()->CreateShaderResourceView(texture.resource->handle.Get(),
                                                       &texture.view, cpu);
            TraceApiCall(context.Device(), "CreateShaderResourceView(present)",
                         texture.resource->handle.Get(), &texture.view, sizeof(texture.view));
            const auto resource_desc = texture.resource->handle->GetDesc();
            const auto crop = layer.crop_rect.IsEmpty()
                                  ? Common::Rectangle<int>{0, 0, static_cast<int>(layer.width),
                                                           static_cast<int>(layer.height)}
                                  : layer.crop_rect;
            const auto mip = texture.view.Texture2DArray.MostDetailedMip;
            const float texture_width =
                static_cast<float>(std::max(UINT64{1}, resource_desc.Width >> mip));
            const float texture_height =
                static_cast<float>(std::max(1u, resource_desc.Height >> mip));
            const auto crop_x = [&](int value) {
                return texture.scaled
                           ? ResolutionScale::PresentCoordinate(static_cast<float>(value),
                                                                texture.guest_width,
                                                                static_cast<u32>(texture_width)) /
                                 texture_width
                           : static_cast<float>(value) / texture_width;
            };
            const auto crop_y = [&](int value) {
                return texture.scaled
                           ? ResolutionScale::PresentCoordinate(static_cast<float>(value),
                                                                texture.guest_height,
                                                                static_cast<u32>(texture_height)) /
                                 texture_height
                           : static_cast<float>(value) / texture_height;
            };
            const PresentParameters constants{
                {crop_x(crop.left), crop_y(crop.top), crop_x(crop.right), crop_y(crop.bottom)},
                static_cast<u32>(layer.transform_flags),
                layer.blending == Tegra::BlendMode::Opaque ? 1u : 0u};
            if (use_upscaler) {
                Present::Layer upscale_layer;
                upscale_layer.source = texture.resource;
                upscale_layer.view = texture.view;
                upscale_layer.guest_extent = {
                    texture.scaled ? texture.guest_width : static_cast<u32>(texture_width),
                    texture.scaled ? texture.guest_height : static_cast<u32>(texture_height)};
                upscale_layer.crop_pixels = {
                    static_cast<float>(crop.left), static_cast<float>(crop.top),
                    static_cast<float>(crop.right), static_cast<float>(crop.bottom)};
                upscale_layer.transform = static_cast<u32>(layer.transform_flags);
                upscale_layer.blend = layer.blending == Tegra::BlendMode::Opaque
                                          ? Present::Blend::Opaque
                                      : layer.blending == Tegra::BlendMode::Premultiplied
                                          ? Present::Blend::Premultiplied
                                          : Present::Blend::Coverage;
                upscaler->DrawLayer(upscale_layer, target, viewport, scissor,
                                    {Present::Filter::Fsr, false, .5f});
                continue;
            }
            commands->SetPipelineState(
                target_pipelines[layer.blending == Tegra::BlendMode::Opaque          ? 0
                                 : layer.blending == Tegra::BlendMode::Premultiplied ? 1
                                                                                     : 2]
                    .Get());
            commands->SetGraphicsRootDescriptorTable(0, gpu_handle);
            commands->SetGraphicsRoot32BitConstants(1, 8, &constants, 0);
            commands->DrawInstanced(3, 1, 0, 0);
        }
    }
    void Capture(const Layout::FramebufferLayout& layout,
                 std::span<const Tegra::FramebufferConfig> layers, RasterizerD3D12& rasterizer,
                 void* bits) {
        const Common::MaxwellMethodProfile::NamedScope capture_profile{
            Common::MaxwellMethodProfile::PresentCapture};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = layout.width;
        desc.Height = layout.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const auto target = context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 total_bytes{};
        context.Device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                                &total_bytes);
        const auto readback = context.CreateBuffer(total_bytes, D3D12_HEAP_TYPE_READBACK);
        context.Device()->CreateRenderTargetView(target->handle.Get(), nullptr, Rtv(BufferCount));
        Draw(layout, layers, rasterizer, target, Rtv(BufferCount), BufferCount,
             screenshot_pipelines);
        context.Transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = target->handle.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback->handle.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        context.Retain(readback);
        context.BeforeCopyWrite(readback, footprint.Offset, total_bytes);
        context.Commands()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        context.Finish(FrameCount::ForcedSubmitPresent);
        const Common::MaxwellMethodProfile::NamedScope copy_profile{
            Common::MaxwellMethodProfile::PresentCopy};
        const size_t row_bytes = static_cast<size_t>(layout.width) * 4;
        for (u32 row = 0; row < layout.height; ++row) {
            std::memcpy(static_cast<u8*>(bits) + row * row_bytes,
                        readback->mapped.data() + footprint.Offset +
                            static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                        row_bytes);
        }
    }
    void Present(const Layout::FramebufferLayout& layout,
                 std::span<const Tegra::FramebufferConfig> layers, RasterizerD3D12& rasterizer) {
        unsigned pacing_interval = 1;
        if (frame_pacing_enabled) {
            const auto now = std::chrono::steady_clock::now();
            const double seconds =
                previous_pacing_frame.time_since_epoch().count() == 0
                    ? 0.0
                    : std::chrono::duration<double>(now - previous_pacing_frame).count();
            previous_pacing_frame = now;
            pacing_interval =
                frame_pacing.Observe(seconds, frame_generation_enabled || layers.empty());
        }
        // Each swapchain buffer owns a descriptor slice; wait only when reusing it.
        context.Collect();
        const auto size = OutputSize(layout.width, layout.height);
        const auto new_width = size.width;
        const auto new_height = size.height;
        if (width != new_width || height != new_height) {
            context.Finish(FrameCount::ForcedSubmitLifecycle);
            present_ticks.fill(0);
            for (auto& buffer : buffers)
                buffer.reset();
            Check(swapchain->ResizeBuffers(active_buffers, new_width, new_height,
                                           DXGI_FORMAT_R8G8B8A8_UNORM, 0),
                  "ResizeBuffers", context.Device());
            width = new_width;
            height = new_height;
            AcquireBuffers();
        }
        UINT index = swapchain->GetCurrentBackBufferIndex();
        auto output_layout = layout;
        const auto scale_x = [&](auto x) {
            return static_cast<decltype(x)>(std::uint64_t{x} * width / std::max(1u, layout.width));
        };
        const auto scale_y = [&](auto y) {
            return static_cast<decltype(y)>(std::uint64_t{y} * height /
                                            std::max(1u, layout.height));
        };
        output_layout.screen = {scale_x(layout.screen.left), scale_y(layout.screen.top),
                                scale_x(layout.screen.right), scale_y(layout.screen.bottom)};
        output_layout.width = width;
        output_layout.height = height;
        Draw(output_layout, layers, rasterizer, buffers[index], Rtv(index), index, pipelines);
        if (frame_generation_enabled && !frame_generation_failed && !layers.empty()) {
            const auto now = std::chrono::steady_clock::now();
            const double seconds =
                previous_game_frame.time_since_epoch().count() == 0
                    ? 0.0
                    : std::chrono::duration<double>(now - previous_game_frame).count();
            previous_game_frame = now;
            ResourcePtr generated;
            try {
                if (!frame_generator)
                    frame_generator = std::make_unique<Present::FrameGenerator>(context);
                unsigned source_width = 0, source_height = 0;
                for (const auto& layer : layers) {
                    const bool rotated = (static_cast<unsigned>(layer.transform_flags) & 4) != 0;
                    source_width = std::max(source_width, rotated ? layer.height : layer.width);
                    source_height = std::max(source_height, rotated ? layer.width : layer.height);
                }
                effective_frame_generation_mode =
                    frame_generator->Configure(swapchain.Get(), frame_generation_mode);
                frame_generator->UseMotion(layers.size() == 1 && upscaler ? upscaler->Motion()
                                                                          : ResourcePtr{});
                frame_generator->SetSourceExtent(source_width, source_height);
                generated = frame_generator->Generate(buffers[index],
                                                      frame_generation_cadence.Observe(seconds));
            } catch (const std::exception&) {
                frame_generation_failed = true;
                Diagnostic("D3D12 frame generation disabled for session: initialization failed");
            } catch (...) {
                frame_generation_failed = true;
                Diagnostic(
                    "D3D12 frame generation disabled for session: unknown initialization failure");
            }
            frame_generation_cadence.Completed(bool(generated));
            if (generated) {
                const Present::GeneratedFrameSchedule schedule{effective_frame_generation_mode};
                for (unsigned i = 0; i < schedule.Count(); ++i) {
                    const auto frame = i == 0 ? generated :
                        frame_generator->Interpolate(schedule.Fraction(i));
                    if (!frame)
                        break; // The last generated present already restored the real image.
                    PresentGenerated(frame, rasterizer, index);
                }
            }
            context.Transition(buffers[index], D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        // Only the display target receives host diagnostics; Capture() never reaches this path.
        if (!layers.empty()) {
            Common::FpsOverlay::DrawIfEnabled([&] {
                if (!fps_counter)
                    fps_counter = std::make_unique<Present::FpsCounter>(context);
                fps_counter->Draw(Rtv(index), width, height);
            });
        }
        context.Transition(buffers[index], D3D12_RESOURCE_STATE_PRESENT);
        context.SimulateSeriesSFrame();
        context.GpuPassFrame();
        {
            const Common::MaxwellMethodProfile::NamedScope submit_profile{
                Common::MaxwellMethodProfile::PresentSubmit};
            present_ticks[index] = context.Submit(FrameCount::ForcedSubmitPresent);
        }
        rasterizer.NotifyFencesSubmitted();
        context.Acknowledge(present_ticks[index]);
        const bool log_present = ++present_sequence <= 8 || present_sequence % 120 == 0;
        if (log_present)
            Diagnostic("D3D12 Present begin");
        HRESULT status;
        {
            const EdenXbox::ThreadProfile::Scope present_scope{
                EdenXbox::ThreadProfile::Wait::Present};
            status = Common::MaxwellMethodProfile::Measure(
                Common::MaxwellMethodProfile::PresentSwapchain, [&] {
                    if (frame_pacing_enabled)
                        return PresentPaced(pacing_interval);
                    return swapchain->Present(1, 0);
                });
        }
        context.LogInfoQueue();
        Check(status, "Present", context.Device());
        RecordPresentation();
        if (frame_generation_enabled)
            Common::FpsOverlay::Presented();
        Check(context.Device()->GetDeviceRemovedReason(), "after Present", context.Device());
        if (log_present)
            Diagnostic("D3D12 Present complete");
    }
    HRESULT PresentPaced(unsigned interval) {
        bool accepted_pacing = false;
        const auto result = Present::SubmitPaced(
            interval,
            [&](unsigned sync, bool nonblocking) {
                const auto status =
                    swapchain->Present(sync, nonblocking ? DXGI_PRESENT_DO_NOT_WAIT : 0);
                accepted_pacing = nonblocking && status == S_OK;
                return status;
            },
            DXGI_ERROR_WAS_STILL_DRAWING, DXGI_ERROR_INVALID_CALL);
        frame_pacing.Completed(accepted_pacing);
        return result;
    }
    void RecordPresentation() {
        auto& profile = context.Profile();
        if (!profile.Enabled())
            return;
        // CPU return times are deliberately separate from observed display times.
        const auto now = std::chrono::steady_clock::now();
        if (previous_present_return.time_since_epoch().count())
            profile.present_call_gaps.Add(
                std::chrono::duration<double>(now - previous_present_return).count());
        previous_present_return = now;
        DXGI_FRAME_STATISTICS stats{};
        if (swapchain->GetFrameStatistics(&stats) != S_OK) {
            have_statistics = false; // Unsupported/occluded/disjoint: no invented display sample.
            return;
        }
        if (have_statistics && stats.PresentCount == previous_statistics.PresentCount)
            return;
        if (have_statistics && stats.PresentCount == previous_statistics.PresentCount + 1 &&
            stats.SyncRefreshCount > previous_statistics.SyncRefreshCount &&
            stats.SyncQPCTime.QuadPart > previous_statistics.SyncQPCTime.QuadPart &&
            stats.PresentRefreshCount > previous_statistics.PresentRefreshCount) {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            const double refresh_seconds =
                double(stats.SyncQPCTime.QuadPart - previous_statistics.SyncQPCTime.QuadPart) /
                (double(frequency.QuadPart) *
                 (stats.SyncRefreshCount - previous_statistics.SyncRefreshCount));
            profile.present_gaps.Add(refresh_seconds * (stats.PresentRefreshCount -
                                                        previous_statistics.PresentRefreshCount));
        }
        previous_statistics = stats;
        have_statistics = true;
    }
    void PresentGenerated(const ResourcePtr& generated, RasterizerD3D12& rasterizer, UINT& index) {
        // The complete real image is safe in Current(), before touching the swapchain.
        const auto copy = [&](const ResourcePtr& destination, const ResourcePtr& source) {
            context.Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST);
            context.Retain(source);
            context.Retain(destination);
            context.Commands()->CopyResource(destination->handle.Get(), source->handle.Get());
        };
        copy(buffers[index], generated);
        context.Transition(buffers[index], D3D12_RESOURCE_STATE_PRESENT);
        present_ticks[index] = context.Submit(FrameCount::ForcedSubmitPresent);
        rasterizer.NotifyFencesSubmitted();
        context.Acknowledge(present_ticks[index]);
        Check(swapchain->Present(1, 0), "Present generated frame", context.Device());
        Common::FpsOverlay::Presented();
        RecordPresentation();
        index = swapchain->GetCurrentBackBufferIndex();
        context.WaitSubmitted(present_ticks[index], "frame generation real frame");
        copy(buffers[index], frame_generator->Current());
    }
};
RendererD3D12::RendererD3D12(Core::Frontend::EmuWindow& window,
                             Tegra::MaxwellDeviceMemoryManager& memory, Tegra::GPU& gpu_,
                             std::unique_ptr<Core::Frontend::GraphicsContext> context_)
    : RendererBase(window, std::move(context_)), impl(std::make_unique<Impl>()), gpu(gpu_),
      rasterizer(gpu_, memory, impl->context) {
    try {
        impl->Initialize(window);
        impl->Present(window.GetFramebufferLayout(), {}, rasterizer);
        impl->context.Finish(FrameCount::ForcedSubmitLifecycle);
        SetEnvironmentVariableA("NXBOX_NATIVE_D3D12_ACTIVE", "1");
    } catch (const std::exception& error) {
        ReportFailure(error);
        throw;
    } catch (...) {
        const EdenXbox::UnknownRendererException error{
            "D3D12 presentation initialization: non-standard exception"};
        ReportFailure(error);
        // The renderer factory already handles runtime_error on initialization failure.
        throw std::runtime_error(error.what());
    }
}
RendererD3D12::~RendererD3D12() {
    SetEnvironmentVariableA("NXBOX_NATIVE_D3D12_ACTIVE", nullptr);
}
bool RendererD3D12::ReportFailure(const std::exception& error) noexcept {
    impl->failed = FAILED(impl->context.Device()->GetDeviceRemovedReason());
    impl->context.ReportFailure(error);
    return true;
}
bool RendererD3D12::HasFailed() const {
    return impl->failed;
}
void RendererD3D12::Composite(std::span<const Tegra::FramebufferConfig> layers) {
    if (rasterizer.OnGpuPipeline())
        return rasterizer.RunGpuPipeline([&] { Composite(layers); });
    const Common::MaxwellMethodProfile::NamedScope composite_profile{
        Common::MaxwellMethodProfile::CompositeRest};
    impl->context.Profile().EndFrame();
    try {
        if (IsScreenshotPending()) {
            std::vector<Tegra::FramebufferConfig> screenshot_layers;
            const auto selected = Tegra::FilterLayerStack(
                layers, renderer_settings.screenshot_layer_stack, screenshot_layers);
            Common::MaxwellMethodProfile::Measure(Common::MaxwellMethodProfile::PresentPrepare,
                                                  [&] { rasterizer.PreparePresentation(); });
            impl->Capture(renderer_settings.screenshot_framebuffer_layout, selected, rasterizer,
                          renderer_settings.screenshot_bits);
            renderer_settings.screenshot_requested = false;
            renderer_settings.screenshot_complete_callback(false);
        }
        if (!impl->failed && render_window.IsShown()) {
            try {
                std::vector<Tegra::FramebufferConfig> display_layers;
                const auto selected = Tegra::FilterLayerStack(
                    layers, Service::Nvnflinger::LayerStackId::Default, display_layers);
                Common::MaxwellMethodProfile::Measure(Common::MaxwellMethodProfile::PresentPrepare,
                                                      [&] { rasterizer.PreparePresentation(); });
                impl->Present(render_window.GetFramebufferLayout(), selected, rasterizer);
                ++m_current_frame;
                render_window.OnFrameDisplayed();
            } catch (const std::exception& error) {
                ReportFailure(error);
            }
        }
    } catch (const std::exception& error) {
        ReportFailure(error);
    } catch (...) {
        const EdenXbox::UnknownRendererException error{
            "D3D12 compositor callback: non-standard exception"};
        ReportFailure(error);
    }
    try {
        const Common::MaxwellMethodProfile::NamedScope end_profile{
            Common::MaxwellMethodProfile::PresentFrameEnd};
        gpu.RendererFrameEndNotify();
        // Advance the caches' frame clock like the Vulkan backend: texture garbage
        // collection, buffer downloads, fence and descriptor-cache aging all key off it.
        rasterizer.TickFrame();
    } catch (const std::exception& error) {
        ReportFailure(error);
    } catch (...) {
        const EdenXbox::UnknownRendererException error{
            "D3D12 frame notification: non-standard exception"};
        ReportFailure(error);
    }
}
std::vector<u8> RendererD3D12::GetAppletCaptureBuffer() {
    try {
        return std::vector<u8>(VideoCore::Capture::TiledSize);
    } catch (const std::exception& error) {
        ReportFailure(error);
    } catch (...) {
        const EdenXbox::UnknownRendererException error{
            "D3D12 applet capture: non-standard exception"};
        ReportFailure(error);
    }
    return {};
}
VideoCore::RasterizerInterface* RendererD3D12::ReadRasterizer() {
    return &rasterizer;
}
std::string RendererD3D12::GetDeviceVendor() const {
    return "D3D12";
}
} // namespace D3D12
