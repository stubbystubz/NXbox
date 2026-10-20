// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_blit_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <fmt/format.h>

#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12 {
namespace {
// Embedded HLSL avoids filesystem access in the packaged UWP application. All
// variants are compiled by the packaged DXC during helper construction.
constexpr std::string_view VertexShader = R"hlsl(
float4 main(uint vertex : SV_VertexID) : SV_Position {
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)hlsl";
constexpr std::string_view PixelShader = R"hlsl(
cbuffer BlitConstants : register(b0, space1) {
    float2 dstOrigin; float2 dstExtent;
    float2 srcOrigin; float2 srcExtent;
    uint2 srcSize; uint srcSamples; uint mode;
    uint conversion; uint linearFilter; uint gridX; uint stencilTest;
};
#if SOURCE_MSAA
Texture2DMSArray<VALUE4> source : register(t0);
Texture2DMSArray<uint4> stencilSource : register(t1);
#else
Texture2DArray<VALUE4> source : register(t0);
Texture2DArray<uint4> stencilSource : register(t1);
#endif
SamplerState pointSampler : register(s0);
SamplerState linearSampler : register(s1);

void Address(float4 position, uint sampleIndex, out int2 coord, out uint sampleId) {
    float2 relative = (position.xy - dstOrigin) / dstExtent;
    coord = int2(floor(srcOrigin + relative * srcExtent));
    sampleId = min(sampleIndex, srcSamples - 1);
    if (mode == 2) { // MSAA -> expanded single-sample grid
        int2 expanded = int2(position.xy - dstOrigin + srcOrigin);
        coord = expanded / int2(gridX, srcSamples / gridX);
        int2 cell = expanded % int2(gridX, srcSamples / gridX);
        sampleId = cell.x + cell.y * gridX;
    } else if (mode == 3) { // expanded grid -> MSAA
        coord = int2(position.xy - dstOrigin) * int2(gridX, srcSamples / gridX)
              + int2(sampleIndex % gridX, sampleIndex / gridX) + int2(srcOrigin);
        sampleId = 0;
    }
    coord = clamp(coord, int2(0, 0), int2(srcSize) - 1);
}
VALUE4 ReadColor(int2 coord, uint sampleId) {
#if SOURCE_MSAA
    return source.Load(int3(coord, 0), sampleId);
#else
    return source.Load(int4(coord, 0, 0));
#endif
}
uint ReadStencil(int2 coord, uint sampleId) {
#if SOURCE_MSAA
    return stencilSource.Load(int3(coord, 0), sampleId).g;
#else
    return stencilSource.Load(int4(coord, 0, 0)).g;
#endif
}
float4 ReadFloat(float4 position, uint sampleIndex, out uint stencil) {
    int2 coord; uint sampleId;
    Address(position, sampleIndex, coord, sampleId);
    float4 value = float4(ReadColor(coord, sampleId));
    stencil = 0;
#if FLOAT_VALUE
#if SOURCE_MSAA
    if (mode == 1) {
        value = 0;
        for (uint i = 0; i < srcSamples; ++i) value += float4(ReadColor(coord, i));
        value /= srcSamples;
    }
#else
    if (linearFilter != 0) {
        float2 uv = (srcOrigin + (position.xy - dstOrigin) / dstExtent * srcExtent) / srcSize;
        value = source.SampleLevel(linearSampler, float3(uv, 0), 0);
    }
#endif
#endif
    if (conversion == 1 || conversion == 6) {
        uint packed = (uint(round(saturate(value.r) * 16777215.0)) & 0xffffff)
                    | (ReadStencil(coord, sampleId) << 24);
        if (conversion == 6) packed = (packed << 8) | (packed >> 24);
        value = float4(uint4(packed, packed >> 8, packed >> 16, packed >> 24) & 255) / 255.0;
    } else if (conversion == 2 || conversion == 7) {
        uint4 bytes = uint4(round(saturate(value) * 255.0));
        uint packed = bytes.r | (bytes.g << 8) | (bytes.b << 16) | (bytes.a << 24);
        if (conversion == 7) packed = (packed >> 8) | (packed << 24);
        value.r = float(packed & 0xffffff) / 16777215.0;
        stencil = packed >> 24;
    } else if (conversion == 3) {
        uint packed = asuint(value.r);
        value = float4(uint4(packed, packed >> 8, packed >> 16, packed >> 24) & 255) / 255.0;
    } else if (conversion == 4) {
        uint4 bytes = uint4(round(saturate(value) * 255.0));
        value.r = asfloat(bytes.r | (bytes.g << 8) | (bytes.b << 16) | (bytes.a << 24));
    } else if (conversion == 5) {
        value = value.bgra;
    }
    if (conversion == 0) stencil = ReadStencil(coord, sampleId);
    return value;
}
VALUE4 Color(float4 position : SV_Position, uint sampleIndex : SV_SampleIndex) : SV_Target {
#if FLOAT_VALUE
    uint stencil; return ReadFloat(position, sampleIndex, stencil);
#else
    int2 coord; uint sampleId; Address(position, sampleIndex, coord, sampleId);
    return ReadColor(coord, sampleId);
#endif
}
float Depth(float4 position : SV_Position, uint sampleIndex : SV_SampleIndex) : SV_Depth {
    uint stencil; return ReadFloat(position, sampleIndex, stencil).r;
}
struct DepthStencilOutput { float depth : SV_Depth; uint stencil : SV_StencilRef; };
DepthStencilOutput DepthStencil(float4 position : SV_Position, uint sampleIndex : SV_SampleIndex) {
    DepthStencilOutput result;
    result.depth = ReadFloat(position, sampleIndex, result.stencil).r;
    return result;
}
void Stencil(float4 position : SV_Position, uint sampleIndex : SV_SampleIndex) {
    uint stencil; ReadFloat(position, sampleIndex, stencil);
    uint mask = stencilTest & 255;
    uint expected = stencilTest >> 8;
    if ((stencil & mask) != expected) discard;
}
)hlsl";

u32 Width(const BlitSurface& surface) {
    return static_cast<u32>(
        std::max<u64>(1, surface.resource->handle->GetDesc().Width >> surface.level));
}
u32 Height(const BlitSurface& surface) {
    return std::max(1u, surface.resource->handle->GetDesc().Height >> surface.level);
}
u32 Samples(const BlitSurface& surface) {
    return surface.resource->handle->GetDesc().SampleDesc.Count;
}
u32 Subresource(const BlitSurface& surface, u32 plane = 0) {
    const auto desc = surface.resource->handle->GetDesc();
    const u32 layers =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    return surface.level + surface.layer * desc.MipLevels + plane * desc.MipLevels * layers;
}
void Validate(const BlitSurface& surface) {
    if (!surface.resource || !surface.resource->handle || !surface.format.Supported())
        throw std::invalid_argument("D3D12 blit missing resource/format");
    const auto desc = surface.resource->handle->GetDesc();
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || surface.level >= desc.MipLevels ||
        surface.layer >= (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                              ? 1u
                              : static_cast<u32>(desc.DepthOrArraySize)))
        throw std::out_of_range("D3D12 blit subresource");
}
void ValidateRegion(const BlitSurface& surface, const VideoCommon::Region2D& region) {
    Validate(surface);
    if (surface.resource->handle->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        region.start.x == region.end.x || region.start.y == region.end.y ||
        std::min(region.start.x, region.end.x) < 0 || std::min(region.start.y, region.end.y) < 0 ||
        static_cast<u32>(std::max(region.start.x, region.end.x)) > Width(surface) ||
        static_cast<u32>(std::max(region.start.y, region.end.y)) > Height(surface))
        throw std::out_of_range("D3D12 shader blit requires a valid 2D region");
}
DXGI_FORMAT DepthFormat(const FormatInfo& format) {
    switch (format.resource_format) {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
        throw std::invalid_argument("D3D12 unsupported blit depth format");
    }
}
DXGI_FORMAT StencilFormat(const FormatInfo& format) {
    return format.resource_format == DXGI_FORMAT_R24G8_TYPELESS
               ? DXGI_FORMAT_X24_TYPELESS_G8_UINT
               : DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
}
BlitSurface RawSurface(BlitSurface surface) {
    // Reinterpretation operates on bytes, without sRGB transfer functions.
    if (surface.format.view_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        surface.format.view_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (surface.format.view_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        surface.format.view_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    return surface;
}
u32 ValueClass(const FormatInfo& format) {
    if (format.name.ends_with("_UINT"))
        return format.depth ? 0 : 1;
    if (format.name.ends_with("_SINT"))
        return 2;
    return 0;
}
struct Constants {
    std::array<float, 2> dst_origin, dst_extent, src_origin, src_extent;
    std::array<u32, 2> src_size;
    u32 src_samples, mode, conversion, linear_filter, grid_x, stencil_test;
};
static_assert(sizeof(Constants) == 16 * sizeof(u32));
using PipelineKey = std::tuple<DXGI_FORMAT, u32, u32, u32, u32,
                               u32>; // format, samples, quality, MSAA, value, stencil mask
struct SharedState {
    ComPtr<ID3D12RootSignature> root;
    std::vector<u8> vertex;
    std::array<std::array<std::vector<u8>, 3>, 2> color;
    std::array<std::vector<u8>, 2> depth, depth_stencil, stencil;
    std::map<PipelineKey, ComPtr<ID3D12PipelineState>> pipelines;
    bool stencil_export{};
};
struct Descriptors {
    ComPtr<ID3D12DescriptorHeap> srv, target;
    std::shared_ptr<SharedState> state;
};
} // namespace

struct BlitImageHelper::Impl {
    Context& context;
    std::shared_ptr<SharedState> state = std::make_shared<SharedState>();
    u32 pending_descriptors{};

    explicit Impl(Context& context_) : context{context_} {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        Check(context.Device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                    sizeof(options)),
              "D3D12 blit options");
        state->stencil_export = options.PSSpecifiedStencilRefSupported;
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
        std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 1, 16};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {1, &range};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
        for (u32 i = 0; i < samplers.size(); ++i) {
            auto& sampler = samplers[i];
            sampler.Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = sampler.AddressV = sampler.AddressW =
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxAnisotropy = 1;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderRegister = i;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = static_cast<UINT>(parameters.size());
        root.pParameters = parameters.data();
        root.NumStaticSamplers = static_cast<UINT>(samplers.size());
        root.pStaticSamplers = samplers.data();
        ComPtr<ID3DBlob> blob, errors;
        Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
              "Serialize D3D12 blit root signature");
        Check(context.Device()->CreateRootSignature(
                  0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&state->root)),
              "Create D3D12 blit root signature");
        ShaderCompiler compiler;
        state->vertex = compiler.Compile(VertexShader, L"main", L"vs_6_0");
        for (u32 msaa = 0; msaa < 2; ++msaa) {
            for (u32 type = 0; type < 3; ++type) {
                const std::string source = "#define SOURCE_MSAA " + std::to_string(msaa) +
                                           "\n#define VALUE4 " +
                                           (type == 0   ? "float4"
                                            : type == 1 ? "uint4"
                                                        : "int4") +
                                           "\n#define FLOAT_VALUE " + (type == 0 ? "1\n" : "0\n") +
                                           std::string{PixelShader};
                state->color[msaa][type] = compiler.Compile(source, L"Color", L"ps_6_0");
                if (type == 0) {
                    state->depth[msaa] = compiler.Compile(source, L"Depth", L"ps_6_0");
                    state->stencil[msaa] = compiler.Compile(source, L"Stencil", L"ps_6_0");
                    if (state->stencil_export)
                        state->depth_stencil[msaa] =
                            compiler.Compile(source, L"DepthStencil", L"ps_6_0");
                }
            }
        }
    }

    BlitSurface Snapshot(const BlitSurface& src, const BlitSurface& dst) {
        if (src.resource->handle.Get() != dst.resource->handle.Get() &&
            !(src.resource->handle->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            return src;
        auto desc = src.resource->handle->GetDesc();
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(desc.Flags &
                                                       ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        auto scratch =
            context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        context.Transition(src.resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
        context.Retain(scratch);
        context.InvalidateRecordedState();
        context.Commands()->CopyResource(scratch->handle.Get(), src.resource->handle.Get());
        return {scratch, src.format, src.level, src.layer};
    }

    ComPtr<ID3D12PipelineState> Pipeline(const BlitSurface& dst, const BlitSurface& src,
                                         u32 stencil_mask) {
        const auto desc = dst.resource->handle->GetDesc();
        const auto format = dst.format.depth ? DepthFormat(dst.format)
                                             : static_cast<DXGI_FORMAT>(dst.format.view_format);
        const u32 msaa = Samples(src) > 1;
        const u32 type = ValueClass(src.format);
        const PipelineKey key{format, Samples(dst), desc.SampleDesc.Quality,
                              msaa,   type,         stencil_mask};
        if (const auto it = state->pipelines.find(key); it != state->pipelines.end())
            return it->second;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = state->root.Get();
        pso.VS = {state->vertex.data(), state->vertex.size()};
        const auto& pixel = !dst.format.depth ? state->color[msaa][type]
                            : stencil_mask    ? state->stencil[msaa]
                            : dst.format.planes > 1 && state->stencil_export
                                ? state->depth_stencil[msaa]
                                : state->depth[msaa];
        pso.PS = {pixel.data(), pixel.size()};
        pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
        pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = ~0u;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.RasterizerState.MultisampleEnable = Samples(dst) > 1;
        pso.DepthStencilState.DepthEnable = dst.format.depth && !stencil_mask;
        pso.DepthStencilState.DepthWriteMask =
            stencil_mask ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.StencilEnable =
            dst.format.depth && dst.format.planes > 1 && (state->stencil_export || stencil_mask);
        pso.DepthStencilState.StencilReadMask = 255;
        pso.DepthStencilState.StencilWriteMask =
            stencil_mask ? static_cast<UINT8>(stencil_mask) : 255;
        const D3D12_DEPTH_STENCILOP_DESC stencil{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                 D3D12_STENCIL_OP_REPLACE,
                                                 D3D12_COMPARISON_FUNC_ALWAYS};
        pso.DepthStencilState.FrontFace = pso.DepthStencilState.BackFace = stencil;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.SampleDesc = desc.SampleDesc;
        if (dst.format.depth)
            pso.DSVFormat = format;
        else {
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = format;
        }
        ComPtr<ID3D12PipelineState> pipeline;
        Check(context.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)),
              "Create D3D12 blit pipeline");
        state->pipelines.emplace(key, pipeline);
        return pipeline;
    }

    void Draw(const BlitSurface& dst, const BlitSurface& original_src,
              const VideoCommon::Region2D& dst_region, const VideoCommon::Region2D& src_region,
              bool linear, BlitConversion conversion, u32 mode = 0, u32 grid_samples = 1) {
        ValidateRegion(dst, dst_region);
        if (mode == 2 || mode == 3) {
            Validate(original_src);
            if (original_src.resource->handle->GetDesc().Dimension !=
                D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                throw std::invalid_argument("D3D12 MSAA shader copy requires 2D textures");
        } else
            ValidateRegion(original_src, src_region);
        if (dst.format.block_width != 1 || original_src.format.block_width != 1 ||
            ValueClass(dst.format) != ValueClass(original_src.format) ||
            (ValueClass(dst.format) && (linear || conversion != BlitConversion::Identity)))
            throw std::invalid_argument("D3D12 unsupported compressed/integer blit conversion");
        if (dst.format.depth && !original_src.format.depth &&
            conversion != BlitConversion::RGBA8ToD24S8 &&
            conversion != BlitConversion::RGBA8ToS8D24 &&
            conversion != BlitConversion::RGBA8ToD32F && dst.format.planes > 1)
            throw std::invalid_argument("D3D12 depth/stencil conversion requires stencil input");
        const auto dst_desc = dst.resource->handle->GetDesc();
        const auto required = dst.format.depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                               : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!(dst_desc.Flags & required)) {
            // P2 cache textures have no attachment flags. Preserve every mip,
            // layer and plane in an attachment-capable scratch resource, draw
            // only the requested region, then copy the whole resource back.
            auto scratch_desc = dst_desc;
            scratch_desc.Flags = required;
            const auto scratch = context.CreateResource(scratch_desc, D3D12_HEAP_TYPE_DEFAULT,
                                                        D3D12_RESOURCE_STATE_COPY_DEST);
            context.Transition(dst.resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.Retain(scratch);
            context.InvalidateRecordedState();
            context.Commands()->CopyResource(scratch->handle.Get(), dst.resource->handle.Get());
            const BlitSurface target{scratch, dst.format, dst.level, dst.layer};
            Draw(target, original_src, dst_region, src_region, linear, conversion, mode,
                 grid_samples);
            context.Transition(scratch, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.Transition(dst.resource, D3D12_RESOURCE_STATE_COPY_DEST);
            context.InvalidateRecordedState();
            context.Commands()->CopyResource(dst.resource->handle.Get(), scratch->handle.Get());
            return;
        }
        if (++pending_descriptors > 128) {
            context.Finish(FrameCount::ForcedSubmitTextureDownload);
            pending_descriptors = 1;
        }
        const auto src = Snapshot(original_src, dst);
        auto descriptors = std::make_shared<Descriptors>();
        descriptors->state = state;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 2;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(context.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors->srv)),
              "Create blit SRV heap");
        heap.Type =
            dst.format.depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check(context.Device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors->target)),
              "Create blit attachment heap");
        auto cpu = descriptors->srv->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = static_cast<DXGI_FORMAT>(src.format.view_format);
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (Samples(src) > 1) {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            srv.Texture2DMSArray = {src.layer, 1};
        } else {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MostDetailedMip = src.level;
            srv.Texture2DArray.MipLevels = 1;
            srv.Texture2DArray.FirstArraySlice = src.layer;
            srv.Texture2DArray.ArraySize = 1;
        }
        context.ValidateViewFormat(src.resource, srv.Format, "CreateShaderResourceView");
        context.Device()->CreateShaderResourceView(src.resource->handle.Get(), &srv, cpu);
        TraceApiCall(context.Device(), "CreateShaderResourceView(blit)", src.resource->handle.Get(),
                     &srv, sizeof(srv));
        cpu.ptr += context.Device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srv.Format = src.format.planes > 1 ? StencilFormat(src.format) : DXGI_FORMAT_R8G8B8A8_UINT;
        if (Samples(src) == 1)
            srv.Texture2DArray.PlaneSlice = src.format.planes > 1 ? 1 : 0;
        if (src.format.planes == 1) {
            if (Samples(src) > 1) {
                srv.Texture2DMSArray.FirstArraySlice = 0;
            } else {
                srv.Texture2DArray.MostDetailedMip = 0;
                srv.Texture2DArray.FirstArraySlice = 0;
            }
        }
        context.ValidateViewFormat(src.format.planes > 1 ? src.resource : nullptr, srv.Format,
                                   "CreateShaderResourceView(stencil)");
        context.Device()->CreateShaderResourceView(
            src.format.planes > 1 ? src.resource->handle.Get() : nullptr, &srv, cpu);
        TraceApiCall(context.Device(), "CreateShaderResourceView(blit stencil)",
                     src.format.planes > 1 ? src.resource->handle.Get() : nullptr, &srv, sizeof(srv));
        const auto target = descriptors->target->GetCPUDescriptorHandleForHeapStart();
        if (dst.format.depth) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = DepthFormat(dst.format);
            if (Samples(dst) > 1) {
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                dsv.Texture2DMSArray = {dst.layer, 1};
            } else {
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsv.Texture2DArray = {dst.level, dst.layer, 1};
            }
            context.ValidateViewFormat(dst.resource, dsv.Format, "CreateDepthStencilView");
            context.Device()->CreateDepthStencilView(dst.resource->handle.Get(), &dsv, target);
            TraceApiCall(context.Device(), "CreateDepthStencilView(blit)", dst.resource->handle.Get(),
                         &dsv, sizeof(dsv));
        } else {
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = static_cast<DXGI_FORMAT>(dst.format.view_format);
            if (Samples(dst) > 1) {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                rtv.Texture2DMSArray = {dst.layer, 1};
            } else {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray = {dst.level, dst.layer, 1, 0};
            }
            context.ValidateViewFormat(dst.resource, rtv.Format, "CreateRenderTargetView");
            context.Device()->CreateRenderTargetView(dst.resource->handle.Get(), &rtv, target);
            TraceApiCall(context.Device(), "CreateRenderTargetView(blit)", dst.resource->handle.Get(),
                         &rtv, sizeof(rtv));
        }
        Constants constants{};
        constants.dst_origin = {static_cast<float>(dst_region.start.x),
                                static_cast<float>(dst_region.start.y)};
        constants.dst_extent = {static_cast<float>(dst_region.end.x - dst_region.start.x),
                                static_cast<float>(dst_region.end.y - dst_region.start.y)};
        constants.src_origin = {static_cast<float>(src_region.start.x),
                                static_cast<float>(src_region.start.y)};
        constants.src_extent = {static_cast<float>(src_region.end.x - src_region.start.x),
                                static_cast<float>(src_region.end.y - src_region.start.y)};
        constants.src_size = {Width(src), Height(src)};
        constants.src_samples = mode == 3 ? grid_samples : Samples(src);
        constants.mode = mode;
        constants.conversion = static_cast<u32>(conversion);
        constants.linear_filter = linear;
        constants.grid_x = grid_samples >= 8 ? 4 : grid_samples >= 2 ? 2 : 1;
        D3D12_VIEWPORT viewport{static_cast<float>(std::min(dst_region.start.x, dst_region.end.x)),
                                static_cast<float>(std::min(dst_region.start.y, dst_region.end.y)),
                                std::abs(constants.dst_extent[0]),
                                std::abs(constants.dst_extent[1]),
                                0,
                                1};
        D3D12_RECT scissor{std::min(dst_region.start.x, dst_region.end.x),
                           std::min(dst_region.start.y, dst_region.end.y),
                           std::max(dst_region.start.x, dst_region.end.x),
                           std::max(dst_region.start.y, dst_region.end.y)};
        // Retain before recording: exceptions must not release descriptors used by
        // a partially recorded batch. SharedState also retains root signature/PSOs.
        context.AfterCompletion([descriptors] {});
        context.Transition(src.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context.Transition(dst.resource, dst.format.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                          : D3D12_RESOURCE_STATE_RENDER_TARGET);
        context.InvalidateRecordedState();
        auto* commands = context.Commands();
        ID3D12DescriptorHeap* heaps[]{descriptors->srv.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootSignature(state->root.Get());
        commands->SetGraphicsRootDescriptorTable(
            1, descriptors->srv->GetGPUDescriptorHandleForHeapStart());
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (dst.format.depth)
            commands->OMSetRenderTargets(0, nullptr, FALSE, &target);
        else
            commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
        commands->SetPipelineState(Pipeline(dst, src, 0).Get());
        commands->SetGraphicsRoot32BitConstants(0, 16, &constants, 0);
        commands->DrawInstanced(3, 1, 0, 0);
        if (dst.format.depth && dst.format.planes > 1 && !state->stencil_export) {
            // Portable stencil export: write both zero and one for each bit,
            // preserving depth and all other stencil bits (no full-surface clear).
            for (u32 bit = 1; bit <= 128; bit <<= 1) {
                commands->SetPipelineState(Pipeline(dst, src, bit).Get());
                for (u32 value : {0u, bit}) {
                    constants.stencil_test = bit | (value << 8);
                    commands->OMSetStencilRef(value);
                    commands->SetGraphicsRoot32BitConstants(0, 16, &constants, 0);
                    commands->DrawInstanced(3, 1, 0, 0);
                }
            }
        }
    }
};

BlitImageHelper::BlitImageHelper(Context& context) : impl{std::make_unique<Impl>(context)} {}
BlitImageHelper::~BlitImageHelper() = default;

void BlitImageHelper::BlitColor(const BlitSurface& dst, const BlitSurface& src,
                                const VideoCommon::Region2D& dst_region,
                                const VideoCommon::Region2D& src_region,
                                Tegra::Engines::Fermi2D::Filter filter,
                                Tegra::Engines::Fermi2D::Operation operation) {
    if (operation != Tegra::Engines::Fermi2D::Operation::SrcCopy || dst.format.depth ||
        src.format.depth)
        throw std::invalid_argument("D3D12 color blit requires SrcCopy/color surfaces");
    impl->Draw(dst, src, dst_region, src_region,
               filter == Tegra::Engines::Fermi2D::Filter::Bilinear, BlitConversion::Identity);
}
void BlitImageHelper::BlitDepthStencil(const BlitSurface& dst, const BlitSurface& src,
                                       const VideoCommon::Region2D& dst_region,
                                       const VideoCommon::Region2D& src_region) {
    if (!dst.format.depth || !src.format.depth || dst.format.planes != src.format.planes)
        throw std::invalid_argument("D3D12 depth blit requires matching aspects");
    impl->Draw(dst, src, dst_region, src_region, false, BlitConversion::Identity);
}
void BlitImageHelper::Resolve(const BlitSurface& dst, const BlitSurface& src,
                              const VideoCommon::Region2D& dst_region,
                              const VideoCommon::Region2D& src_region) {
    ValidateRegion(dst, dst_region);
    ValidateRegion(src, src_region);
    if (Samples(src) == 1 || Samples(dst) != 1 ||
        dst.format.resource_format != src.format.resource_format ||
        dst.format.depth != src.format.depth)
        throw std::invalid_argument("D3D12 resolve requires compatible MSAA->single surfaces");
    const bool full = dst_region.start == VideoCommon::Offset2D{0, 0} &&
                      src_region.start == VideoCommon::Offset2D{0, 0} &&
                      dst_region.end == VideoCommon::Offset2D{static_cast<s32>(Width(dst)),
                                                              static_cast<s32>(Height(dst))} &&
                      dst_region.end == src_region.end && Width(src) == Width(dst) &&
                      Height(src) == Height(dst);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = static_cast<DXGI_FORMAT>(dst.format.view_format);
    Check(impl->context.Device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                      sizeof(support)),
          "D3D12 resolve format support");
    if (full && !dst.format.depth && !ValueClass(dst.format) &&
        (support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE)) {
        impl->context.Transition(src.resource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        impl->context.Transition(dst.resource, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        impl->context.InvalidateRecordedState();
        impl->context.Commands()->ResolveSubresource(
            dst.resource->handle.Get(), Subresource(dst), src.resource->handle.Get(),
            Subresource(src), static_cast<DXGI_FORMAT>(dst.format.view_format));
    } else {
        impl->Draw(dst, src, dst_region, src_region, false, BlitConversion::Identity,
                   dst.format.depth || ValueClass(dst.format) ? 4 : 1);
    }
}
void BlitImageHelper::ConvertImage(const BlitSurface& dst, const BlitSurface& src,
                                   const VideoCommon::Region2D& dst_region,
                                   const VideoCommon::Region2D& src_region,
                                   BlitConversion conversion) {
    Validate(dst);
    Validate(src);
    const auto source_format = src.format.resource_format;
    const auto target_format = dst.format.resource_format;
    const bool source_rgba = source_format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool target_rgba = target_format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    bool valid = false;
    switch (conversion) {
    case BlitConversion::Identity:
        valid = source_format == target_format;
        break;
    case BlitConversion::S8D24ToRGBA8:
    case BlitConversion::D24S8ToRGBA8:
        valid = source_format == DXGI_FORMAT_R24G8_TYPELESS && target_rgba;
        break;
    case BlitConversion::RGBA8ToS8D24:
    case BlitConversion::RGBA8ToD24S8:
        valid = source_rgba && target_format == DXGI_FORMAT_R24G8_TYPELESS;
        break;
    case BlitConversion::D32FToRGBA8:
        valid = source_format == DXGI_FORMAT_R32_TYPELESS && target_rgba;
        break;
    case BlitConversion::RGBA8ToD32F:
        valid = source_rgba && target_format == DXGI_FORMAT_R32_TYPELESS;
        break;
    case BlitConversion::SwapRedBlue:
        valid = (source_rgba && target_format == DXGI_FORMAT_B8G8R8A8_TYPELESS) ||
                (target_rgba && source_format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
        break;
    }
    if (!valid)
        throw std::invalid_argument("D3D12 conversion does not match surface formats");
    impl->Draw(RawSurface(dst), RawSurface(src), dst_region, src_region, false, conversion);
}
bool BlitImageHelper::DrawCompatible(const BlitSurface& dst, const BlitSurface& src,
                                     bool linear) {
    // Mirrors the checks Draw throws on for an Identity color blit.
    return dst.resource && src.resource && dst.resource->handle && src.resource->handle &&
           dst.format.Supported() && src.format.Supported() && !dst.format.depth &&
           !src.format.depth && dst.format.block_width == 1 && src.format.block_width == 1 &&
           dst.format.block_height == 1 && src.format.block_height == 1 &&
           ValueClass(dst.format) == ValueClass(src.format) &&
           !(ValueClass(dst.format) && linear);
}
bool BlitImageHelper::CopyCompatible(const BlitSurface& dst, const BlitSurface& src) {
    if (!dst.resource || !src.resource || !dst.resource->handle || !src.resource->handle ||
        !dst.format.Supported() || !src.format.Supported())
        return false;
    const auto d = dst.resource->handle->GetDesc(), s = src.resource->handle->GetDesc();
    // Conservative typeless-family equivalence; equal byte size alone is insufficient.
    return d.Format == s.Format && dst.format.resource_format == src.format.resource_format &&
           dst.format.planes == src.format.planes && d.Dimension == s.Dimension &&
           d.SampleDesc.Count == s.SampleDesc.Count && d.SampleDesc.Quality == s.SampleDesc.Quality;
}
void BlitImageHelper::CopyImage(const BlitSurface& dst, const BlitSurface& original_src,
                                std::span<const VideoCommon::ImageCopy> copies) {
    Validate(dst);
    Validate(original_src);
    const auto src = impl->Snapshot(original_src, dst);
    for (const auto& copy : copies) {
        if (copy.src_subresource.num_layers <= 0 ||
            copy.src_subresource.num_layers != copy.dst_subresource.num_layers ||
            !copy.extent.width || !copy.extent.height || !copy.extent.depth ||
            copy.src_subresource.base_level < 0 || copy.dst_subresource.base_level < 0 ||
            copy.src_subresource.base_layer < 0 || copy.dst_subresource.base_layer < 0)
            throw std::invalid_argument("D3D12 copy invalid layers/extent");
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            BlitSurface s{src.resource, src.format,
                          static_cast<u32>(copy.src_subresource.base_level),
                          static_cast<u32>(copy.src_subresource.base_layer + layer)};
            BlitSurface d{dst.resource, dst.format,
                          static_cast<u32>(copy.dst_subresource.base_level),
                          static_cast<u32>(copy.dst_subresource.base_layer + layer)};
            Validate(s);
            Validate(d);
            const auto check_region = [&](const BlitSurface& surface,
                                          const VideoCommon::Offset3D& offset) {
                const auto desc = surface.resource->handle->GetDesc();
                const u32 depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                      ? std::max(1u, u32{desc.DepthOrArraySize} >> surface.level)
                                      : 1;
                if (offset.x < 0 || offset.y < 0 || offset.z < 0 ||
                    u64{static_cast<u32>(offset.x)} + copy.extent.width > Width(surface) ||
                    u64{static_cast<u32>(offset.y)} + copy.extent.height > Height(surface) ||
                    u64{static_cast<u32>(offset.z)} + copy.extent.depth > depth)
                    throw std::out_of_range("D3D12 image copy bounds");
                return offset.x == 0 && offset.y == 0 && offset.z == 0 &&
                       copy.extent.width == Width(surface) &&
                       copy.extent.height == Height(surface) && copy.extent.depth == depth;
            };
            const bool full_src = check_region(s, copy.src_offset),
                       full_dst = check_region(d, copy.dst_offset);
            static int trace_budget = BisectEnabled("NXBOX_D3D12_COPY_TRACE") ? 400 : 0;
            if (trace_budget > 0 && (s.level || d.level)) {
                --trace_budget;
                Diagnostic(fmt::format(
                    "D3D12_COPY_TRACE src={}x{} L{} dst={}x{} L{} src_off={},{} dst_off={},{} "
                    "extent={}x{} compatible={} full={}{} src_sub={} dst_sub={} dst_levels={}",
                    Width(s), Height(s), s.level, Width(d), Height(d), d.level, copy.src_offset.x,
                    copy.src_offset.y, copy.dst_offset.x, copy.dst_offset.y, copy.extent.width,
                    copy.extent.height, CopyCompatible(d, s), full_src, full_dst, Subresource(s),
                    Subresource(d), d.resource->handle->GetDesc().MipLevels));
            }
            if (CopyCompatible(d, s) &&
                (!(s.format.depth || d.format.depth || Samples(s) > 1) || (full_src && full_dst))) {
                const auto& f = s.format;
                if (f.block_width > 1 && (!full_src || !full_dst) &&
                    (copy.src_offset.x % f.block_width || copy.dst_offset.x % f.block_width ||
                     copy.src_offset.y % f.block_height || copy.dst_offset.y % f.block_height ||
                     copy.extent.width % f.block_width || copy.extent.height % f.block_height))
                    throw std::invalid_argument("D3D12 BC copy alignment");
                impl->context.Transition(s.resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
                impl->context.Transition(d.resource, D3D12_RESOURCE_STATE_COPY_DEST);
                for (u32 plane = 0; plane < s.format.planes; ++plane) {
                    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
                    source.pResource = s.resource->handle.Get();
                    target.pResource = d.resource->handle.Get();
                    source.Type = target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    source.SubresourceIndex = Subresource(s, plane);
                    target.SubresourceIndex = Subresource(d, plane);
                    D3D12_BOX box{static_cast<UINT>(copy.src_offset.x),
                                  static_cast<UINT>(copy.src_offset.y),
                                  static_cast<UINT>(copy.src_offset.z),
                                  static_cast<UINT>(copy.src_offset.x) + copy.extent.width,
                                  static_cast<UINT>(copy.src_offset.y) + copy.extent.height,
                                  static_cast<UINT>(copy.src_offset.z) + copy.extent.depth};
                    impl->context.InvalidateRecordedState();
                    impl->context.BeforeCopyWrite(d.resource, target.SubresourceIndex, 1);
                    impl->context.Commands()->CopyTextureRegion(
                        &target, copy.dst_offset.x, copy.dst_offset.y, copy.dst_offset.z, &source,
                        full_src && full_dst ? nullptr : &box);
                }
            } else {
                if (copy.extent.depth != 1 || copy.src_offset.z || copy.dst_offset.z)
                    throw std::invalid_argument("D3D12 shader copies require 2D slices");
                BlitConversion conversion = BlitConversion::Identity;
                if (s.format.resource_format == DXGI_FORMAT_R24G8_TYPELESS &&
                    d.format.resource_format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
                    conversion = s.format.name == "S8_UINT_D24_UNORM"
                                     ? BlitConversion::S8D24ToRGBA8
                                     : BlitConversion::D24S8ToRGBA8;
                else if (d.format.resource_format == DXGI_FORMAT_R24G8_TYPELESS &&
                         s.format.resource_format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
                    conversion = d.format.name == "S8_UINT_D24_UNORM"
                                     ? BlitConversion::RGBA8ToS8D24
                                     : BlitConversion::RGBA8ToD24S8;
                else if (s.format.resource_format == DXGI_FORMAT_R32_TYPELESS &&
                         d.format.resource_format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
                    conversion = BlitConversion::D32FToRGBA8;
                else if (d.format.resource_format == DXGI_FORMAT_R32_TYPELESS &&
                         s.format.resource_format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
                    conversion = BlitConversion::RGBA8ToD32F;
                else if (s.format.resource_format != d.format.resource_format &&
                         !((s.format.resource_format == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
                            d.format.resource_format == DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
                           (d.format.resource_format == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
                            s.format.resource_format == DXGI_FORMAT_R8G8B8A8_TYPELESS)))
                    throw std::invalid_argument("D3D12 unsupported format reinterpretation");
                // Cancel the typed BGRA swizzle to preserve the guest byte layout.
                if (s.format.resource_format != d.format.resource_format &&
                    (s.format.resource_format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                     d.format.resource_format == DXGI_FORMAT_B8G8R8A8_TYPELESS))
                    conversion = BlitConversion::SwapRedBlue;
                const VideoCommon::Region2D sr{
                    {copy.src_offset.x, copy.src_offset.y},
                    {copy.src_offset.x + static_cast<s32>(copy.extent.width),
                     copy.src_offset.y + static_cast<s32>(copy.extent.height)}};
                const VideoCommon::Region2D dr{
                    {copy.dst_offset.x, copy.dst_offset.y},
                    {copy.dst_offset.x + static_cast<s32>(copy.extent.width),
                     copy.dst_offset.y + static_cast<s32>(copy.extent.height)}};
                impl->Draw(RawSurface(d), RawSurface(s), dr, sr, false, conversion);
            }
        }
    }
}
void BlitImageHelper::CopyImageMSAA(const BlitSurface& dst, const BlitSurface& src,
                                    std::span<const VideoCommon::ImageCopy> copies) {
    Validate(dst);
    Validate(src);
    const u32 source_samples = Samples(src), target_samples = Samples(dst);
    if (source_samples == target_samples) {
        CopyImage(dst, src, copies);
        return;
    }
    if (source_samples > 1 && target_samples > 1)
        throw std::invalid_argument("D3D12 different MSAA sample counts");
    const u32 samples = std::max(source_samples, target_samples);
    if (samples != 2 && samples != 4 && samples != 8 && samples != 16)
        throw std::invalid_argument("D3D12 unsupported MSAA grid");
    const u32 grid_x = samples >= 8 ? 4 : 2, grid_y = samples / grid_x;
    for (const auto& copy : copies) {
        if (copy.src_subresource.num_layers <= 0 ||
            copy.src_subresource.num_layers != copy.dst_subresource.num_layers ||
            copy.extent.depth != 1 || copy.src_offset.z || copy.dst_offset.z ||
            !copy.extent.width || !copy.extent.height || copy.src_subresource.base_level < 0 ||
            copy.dst_subresource.base_level < 0 || copy.src_subresource.base_layer < 0 ||
            copy.dst_subresource.base_layer < 0)
            throw std::invalid_argument("D3D12 MSAA copy invalid layers/extent");
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            const BlitSurface s{src.resource, src.format,
                                static_cast<u32>(copy.src_subresource.base_level),
                                static_cast<u32>(copy.src_subresource.base_layer + layer)};
            const BlitSurface d{dst.resource, dst.format,
                                static_cast<u32>(copy.dst_subresource.base_level),
                                static_cast<u32>(copy.dst_subresource.base_layer + layer)};
            Validate(s);
            Validate(d);
            const u64 source_width = source_samples > 1 ? u64{Width(s)} * grid_x : Width(s);
            const u64 source_height = source_samples > 1 ? u64{Height(s)} * grid_y : Height(s);
            const u64 read_width = u64{copy.extent.width} * (target_samples > 1 ? grid_x : 1);
            const u64 read_height = u64{copy.extent.height} * (target_samples > 1 ? grid_y : 1);
            if (copy.src_offset.x < 0 || copy.src_offset.y < 0 ||
                u64{static_cast<u32>(copy.src_offset.x)} + read_width > source_width ||
                u64{static_cast<u32>(copy.src_offset.y)} + read_height > source_height)
                throw std::out_of_range("D3D12 MSAA copy source grid bounds");
            const VideoCommon::Region2D sr{
                {copy.src_offset.x, copy.src_offset.y},
                {copy.src_offset.x + static_cast<s32>(copy.extent.width),
                 copy.src_offset.y + static_cast<s32>(copy.extent.height)}};
            const VideoCommon::Region2D dr{
                {copy.dst_offset.x, copy.dst_offset.y},
                {copy.dst_offset.x + static_cast<s32>(copy.extent.width),
                 copy.dst_offset.y + static_cast<s32>(copy.extent.height)}};
            impl->Draw(d, s, dr, sr, false, BlitConversion::Identity, source_samples > 1 ? 2 : 3,
                       samples);
        }
    }
}
} // namespace D3D12
