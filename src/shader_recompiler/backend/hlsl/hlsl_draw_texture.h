// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>

namespace Shader::Backend::HLSL {
constexpr bool DrawTextureFillsViewport(float x0, float y0, float x1, float y1, float width,
                                        float height) {
    return width > 0.0f && height > 0.0f && x0 == 0.0f && y0 == 0.0f && x1 == width && y1 == height;
}
inline constexpr std::string_view DRAW_TEXTURE_VERTEX = R"hlsl(
cbuffer BlitRect : register(b0, space1) {
    float4 dst_rect; float4 src_rect; float2 dst_size; float2 src_size;
};
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    float2 corner = float2(id & 1u, id >> 1u);
    Output result;
    result.position = float4(lerp(dst_rect.xy, dst_rect.zw, corner) / dst_size *
                             float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0, 1);
    result.uv = lerp(src_rect.xy, src_rect.zw, corner) / src_size;
    return result;
}
)hlsl";
// Only for a destination that exactly fills the viewport. Scissoring partial
// rectangles would change fractional-edge/MSAA coverage, so keep their strip.
inline constexpr std::string_view DRAW_TEXTURE_TRIANGLE_VERTEX = R"hlsl(
cbuffer BlitRect : register(b0, space1) {
    float4 dst_rect; float4 src_rect; float2 dst_size; float2 src_size;
};
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    float2 corner = float2((id << 1u) & 2u, id & 2u);
    Output result;
    result.position = float4(lerp(dst_rect.xy, dst_rect.zw, corner) / dst_size *
                             float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0, 1);
    result.uv = lerp(src_rect.xy, src_rect.zw, corner) / src_size;
    return result;
}
)hlsl";
inline constexpr std::string_view DRAW_TEXTURE_FRAGMENT = R"hlsl(
Texture2DArray<float4> source_texture : register(t0, space0);
SamplerState source_sampler : register(s0, space0);
struct Input { float4 position : SV_Position; float2 uv : TEXCOORD0; };
float4 main(Input input) : SV_Target0 {
    return source_texture.Sample(source_sampler, float3(input.uv, 0));
}
)hlsl";
} // namespace Shader::Backend::HLSL
