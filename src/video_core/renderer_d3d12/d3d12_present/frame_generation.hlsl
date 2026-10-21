// SPDX-License-Identifier: GPL-3.0-or-later
// Final compositor images only; no guest motion vectors or game-specific data.
cbuffer Parameters : register(b0) {
    uint2 size;
    uint2 blocks;
    uint2 output_size;
};
Texture2D<float4> previous_frame : register(t0);
Texture2D<float4> current_frame : register(t1);
Texture2D<float2> luma : register(t2);
Texture2D<float4> motion : register(t3);
Texture2D<float4> real_frame : register(t4);
RWTexture2D<float2> luma_out : register(u0);
RWTexture2D<float4> motion_out : register(u1);
RWTexture2D<float4> generated : register(u2);
RWByteAddressBuffer difference : register(u3);
RWTexture2D<float4> current_out : register(u4);
SamplerState linear_sampler : register(s0);
float Luma(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}
groupshared uint luma_difference[64];
[numthreads(8, 8, 1)] void LumaCS(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
    float2 value = 0;
    bool valid = all(id.xy < blocks);
    if (valid) {
        // Four taps over each 8x8 cell, low-pass before block matching.
        for (uint y = 0; y < 2; ++y)
            for (uint x = 0; x < 2; ++x) {
                float2 uv = (float2(id.xy * 8 + uint2(x, y) * 4) + 2) / float2(size);
                value += float2(Luma(previous_frame.SampleLevel(linear_sampler, uv, 0).rgb),
                                Luma(current_frame.SampleLevel(linear_sampler, uv, 0).rgb)) *
                         0.25;
            }
        luma_out[id.xy] = value;
    }
    luma_difference[lane] = valid ? uint(abs(value.x - value.y) * 4095) : 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride >>= 1) {
        if (lane < stride)
            luma_difference[lane] += luma_difference[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
        difference.InterlockedAdd(0, luma_difference[0]);
} float2 ReadLuma(int2 p) {
    return luma.Load(int3(clamp(p, int2(0, 0), int2(blocks) - 1), 0));
}
float Cost(int2 p, int2 displacement, bool reverse, int stride) {
    float cost = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            int2 q = p + int2(x, y) * stride;
            float2 a = ReadLuma(q), b = ReadLuma(q + displacement);
            cost += abs(reverse ? a.y - b.x : a.x - b.y);
        }
    return cost + 0.001 * dot(float2(displacement), float2(displacement));
}
int2 Search(int2 p, bool reverse) {
    int2 best = 0;
    float score = Cost(p, best, reverse, 2);
    // Coarse 1/16-scale stencil followed by a 1/8-scale refinement.
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            int2 v = int2(x, y) * 2;
            float c = Cost(p, v, reverse, 2);
            if (c < score) {
                score = c;
                best = v;
            }
        }
    int2 center = best;
    score = Cost(p, best, reverse, 1);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            int2 v = center + int2(x, y);
            float c = Cost(p, v, reverse, 1);
            if (c < score) {
                score = c;
                best = v;
            }
        }
    return best;
}
[numthreads(8, 8, 1)] void MotionCS(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= blocks))
        return;
    motion_out[id.xy] = float4(Search(int2(id.xy), false), Search(int2(id.xy), true));
}[numthreads(8, 8, 1)] void WarpCS(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= output_size))
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(output_size);
    float4 v = motion.SampleLevel(linear_sampler, uv, 0);
    float2 forward_uv = uv - v.xy * 4 / float2(size);
    float2 backward_uv = uv - v.zw * 4 / float2(size);
    float2 f = motion.SampleLevel(linear_sampler, forward_uv, 0).xy;
    float2 b = motion.SampleLevel(linear_sampler, backward_uv, 0).zw;
    // Forward/backward consistency at corresponding endpoints rejects occlusions.
    float2 opposite = motion.SampleLevel(linear_sampler, uv + v.xy * 8 / float2(size), 0).zw;
    bool reliable = length(v.xy + opposite) <= 1.5 && length(f + b) <= 1.5 &&
                    all(forward_uv >= 0) && all(forward_uv <= 1) && all(backward_uv >= 0) &&
                    all(backward_uv <= 1);
    float2 a_uv = reliable ? uv - f * 4 / float2(size) : uv;
    float2 b_uv = reliable ? uv - b * 4 / float2(size) : uv;
    generated[id.xy] = 0.5 * (previous_frame.SampleLevel(linear_sampler, a_uv, 0) +
                              current_frame.SampleLevel(linear_sampler, b_uv, 0));
}

// Only needed when the guest image is cheaper to retain than the output image.
[numthreads(8, 8, 1)] void DownsampleCS(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size))
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(size);
    current_out[id.xy] = real_frame.SampleLevel(linear_sampler, uv, 0);
}
