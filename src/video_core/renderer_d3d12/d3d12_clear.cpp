// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include "video_core/renderer_d3d12/d3d12_clear.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"

namespace D3D12::ClearOperations {
Rect Intersect(Rect area, Rect clip) {
    return {std::max(area.left, clip.left), std::max(area.top, clip.top),
            std::min(area.right, clip.right), std::min(area.bottom, clip.bottom)};
}
Rect ResolveScissor(Rect render_area, std::uint32_t clear_control, bool scissor_enabled,
                    Rect scissor) {
    return (clear_control & (1u << 8)) && scissor_enabled ? Intersect(render_area, scissor)
                                                          : render_area;
}
Selection Decode(std::uint32_t surface, std::uint32_t control, std::uint32_t write_mask) {
    const auto stencil_mask = static_cast<std::uint8_t>(control & 1 ? write_mask : 255);
    return {static_cast<std::uint8_t>((surface >> 2) & 15),
            (surface & 1) != 0,
            (surface & 2) != 0 && stencil_mask != 0,
            stencil_mask,
            (surface >> 6) & 15,
            (surface >> 10) & 65535};
}
} // namespace D3D12::ClearOperations

#ifdef _WIN32
#include <bit>
#include <climits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12 {
namespace {
constexpr std::string_view ClearShader = R"(
cbuffer Parameters : register(b0) { uint4 color_bits; float clear_depth; };
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
float4 PSFloat() : SV_Target0 { return asfloat(color_bits); }
uint4 PSUint() : SV_Target0 { return color_bits; }
int4 PSSint() : SV_Target0 { return asint(color_bits); }
float PSDepth() : SV_Depth { return clear_depth; }
)";
D3D12_RECT Rectangle(const ClearAttachment& target, const ClearOperations::Rect* clip) {
    if (!target.width || !target.height || target.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        target.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::invalid_argument("Invalid clear attachment extent");
    ClearOperations::Rect area{0, 0, static_cast<std::int32_t>(target.width),
                               static_cast<std::int32_t>(target.height)};
    if (clip)
        area = ClearOperations::Intersect(area, *clip);
    return {area.left, area.top, area.right, area.bottom};
}
bool Empty(D3D12_RECT rect) {
    return rect.left >= rect.right || rect.top >= rect.bottom;
}
} // namespace
struct ClearRenderer::Impl {
    Context& context;
    ComPtr<ID3D12RootSignature> root;
    std::vector<u8> vertex;
    std::array<std::vector<u8>, 4> pixels;
    // format, sample count/quality, pixel type, color mask, depth enable, stencil mask.
    using Key = std::tuple<DXGI_FORMAT, UINT, UINT, unsigned, unsigned, bool, unsigned>;
    std::map<Key, ComPtr<ID3D12PipelineState>> pipelines;

    explicit Impl(Context& context) : context{context} {
        // Reuse presentation's LoadPackagedLibrary("dxcompiler.dll")/IDxcCompiler3 path.
        ShaderCompiler compiler;
        vertex = compiler.Compile(ClearShader, L"VS", L"vs_6_0");
        pixels[0] = compiler.Compile(ClearShader, L"PSFloat", L"ps_6_0");
        pixels[1] = compiler.Compile(ClearShader, L"PSUint", L"ps_6_0");
        pixels[2] = compiler.Compile(ClearShader, L"PSSint", L"ps_6_0");
        pixels[3] = compiler.Compile(ClearShader, L"PSDepth", L"ps_6_0");
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0, 0, 5};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC description{1, &parameter, 0, nullptr,
                                              D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> serialized, errors;
        Check(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                          &errors),
              "Serialize clear root signature", context.Device());
        Check(context.Device()->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                    serialized->GetBufferSize(),
                                                    IID_PPV_ARGS(&root)),
              "Create clear root signature", context.Device());
    }
    ComPtr<ID3D12PipelineState> Pipeline(const ClearAttachment& target, unsigned pixel,
                                         unsigned mask, bool depth, unsigned stencil_mask) {
        const Key key{target.format, target.samples.Count, target.samples.Quality, pixel, mask,
                      depth,         stencil_mask};
        if (const auto found = pipelines.find(key); found != pipelines.end())
            return found->second;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.VS = {vertex.data(), vertex.size()};
        desc.PS = {pixels.at(pixel).data(), pixels.at(pixel).size()};
        desc.SampleMask = UINT_MAX;
        desc.SampleDesc = target.samples;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.RasterizerState.MultisampleEnable = target.samples.Count > 1;
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = static_cast<UINT8>(mask);
        auto& ds = desc.DepthStencilState;
        ds.DepthEnable = depth;
        ds.DepthWriteMask = depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        ds.StencilEnable = stencil_mask != 0;
        ds.StencilReadMask = 255;
        ds.StencilWriteMask = static_cast<UINT8>(stencil_mask);
        ds.FrontFace = ds.BackFace = {D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE,
                                      D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
        if (pixel == 3) {
            desc.DSVFormat = target.format;
        } else {
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = target.format;
        }
        ComPtr<ID3D12PipelineState> pipeline;
        CheckPipeline(context.Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)),
                      "Create masked clear pipeline", context.Device(),
                      Pso::HashBytes(desc.PS.pShaderBytecode, desc.PS.BytecodeLength));
        pipelines.emplace(key, pipeline);
        return pipeline;
    }
    void Draw(const ClearAttachment& target, D3D12_RECT rect, const ClearValues& values,
              unsigned pixel, unsigned mask, bool depth, unsigned stencil_mask) {
        auto pipeline = Pipeline(target, pixel, mask, depth, stencil_mask);
        context.InvalidateRecordedState();
        auto* commands = context.Commands();
        commands->SetPipelineState(pipeline.Get());
        commands->SetGraphicsRootSignature(root.Get());
        std::array<std::uint32_t, 5> constants{};
        for (std::size_t i = 0; i < 4; ++i)
            constants[i] = pixel == 0 ? std::bit_cast<std::uint32_t>(values.color[i])
                                      : values.integer_color[i];
        constants[4] = std::bit_cast<std::uint32_t>(std::clamp(values.depth, 0.0f, 1.0f));
        commands->SetGraphicsRoot32BitConstants(0, 5, constants.data(), 0);
        if (pixel == 3) {
            commands->OMSetRenderTargets(0, nullptr, FALSE, &target.descriptor);
            commands->OMSetStencilRef(values.stencil);
        } else {
            commands->OMSetRenderTargets(1, &target.descriptor, FALSE, nullptr);
        }
        const D3D12_VIEWPORT viewport{
            0, 0, static_cast<float>(target.width), static_cast<float>(target.height), 0, 1};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &rect);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        // Allow owner destruction before submission without releasing live objects.
        context.AfterCompletion([pipeline, signature = root] {});
    }
};
ClearRenderer::ClearRenderer(Context& context) : impl{std::make_unique<Impl>(context)} {}
ClearRenderer::~ClearRenderer() = default;
bool ClearRenderer::Execute(const ClearOperations::Selection& selection, const ClearValues& values,
                            const ClearAttachment* color, const ClearAttachment* depth_stencil,
                            const ClearOperations::Rect* clip) {
    if (selection.color_mask > 15)
        throw std::invalid_argument("Invalid clear color mask");
    if (values.color_type != ClearColorType::Float && values.color_type != ClearColorType::Uint &&
        values.color_type != ClearColorType::Sint)
        throw std::invalid_argument("Invalid clear color type");
    impl->context.MarkWork("guest clears");
    if (auto* profile = impl->context.PassProfile()) {
        const auto* target = color ? color : depth_stencil;
        profile->Boundary();
        if (target)
            profile->Target({target->width, target->height,
                             color ? static_cast<u32>(color->format) : 0,
                             depth_stencil ? static_cast<u32>(depth_stencil->format) : 0, 0});
    }
    bool drawn{};
    if (color && color->resource && selection.color_mask) {
        const auto rect = Rectangle(*color, clip);
        if (!Empty(rect)) {
            impl->context.Transition(color->resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (selection.color_mask == 15 && values.color_type == ClearColorType::Float) {
                impl->context.Commands()->ClearRenderTargetView(color->descriptor,
                                                                values.color.data(), 1, &rect);
            } else {
                impl->Draw(*color, rect, values, static_cast<unsigned>(values.color_type),
                           selection.color_mask, false, 0);
                drawn = true;
            }
        }
    }
    if (depth_stencil && depth_stencil->resource) {
        const bool depth = selection.depth && depth_stencil->has_depth;
        const bool stencil =
            selection.stencil && selection.stencil_mask && depth_stencil->has_stencil;
        const auto rect = Rectangle(*depth_stencil, clip);
        if (!Empty(rect) && (depth || stencil)) {
            impl->context.Transition(depth_stencil->resource, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (stencil && selection.stencil_mask != 255) {
                impl->Draw(*depth_stencil, rect, values, 3, 0, depth, selection.stencil_mask);
                drawn = true;
            } else {
                const auto flags =
                    static_cast<D3D12_CLEAR_FLAGS>((depth ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                   (stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
                impl->context.Commands()->ClearDepthStencilView(
                    depth_stencil->descriptor, flags, values.depth, values.stencil, 1, &rect);
            }
        }
    }
    return drawn;
}
} // namespace D3D12
#endif
