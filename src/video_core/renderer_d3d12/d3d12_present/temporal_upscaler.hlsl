// SPDX-License-Identifier: GPL-3.0-or-later
// Jitter-free TAAU: image-derived flow is heuristic, never geometry motion.
cbuffer Parameters : register(b0) {
    uint2 source_size;
    uint2 output_size;
    uint history_valid;
};
Texture2D<float4> current_frame : register(t0);
Texture2D<float4> history_frame : register(t1);
Texture2D<float4> motion : register(t2);
Texture2D<float4> previous_frame : register(t3);
Texture2D<float4> spatial_frame : register(t4);
RWTexture2D<float4> accumulated : register(u0);
SamplerState linear_sampler : register(s0);
float3 ToYCoCg(float3 c) {
    return float3(dot(c, float3(.25, .5, .25)), (c.r - c.b) * .5, (-c.r + 2 * c.g - c.b) * .25);
}
float3 FromYCoCg(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}
float4 Cubic(float x) {
    float x2 = x * x, x3 = x2 * x;
    return float4(-x + 2 * x2 - x3, 2 - 5 * x2 + 3 * x3, x + 4 * x2 - 3 * x3, -x2 + x3) * .5;
}
float4 Reconstruct(Texture2D<float4> image, float2 uv, uint2 extent) {
    float2 p = uv * float2(extent) - .5;
    float4 wx = Cubic(frac(p.x)), wy = Cubic(frac(p.y));
    float4 color = 0;
    [unroll] for (int y = 0; y < 4; ++y)[unroll] for (int x = 0; x < 4; ++x) color +=
        image.Load(
            int3(clamp(int2(floor(p)) + int2(x - 1, y - 1), int2(0, 0), int2(extent) - 1), 0)) *
        wx[x] * wy[y];
    return color;
}
[numthreads(8, 8, 1)] void TemporalCS(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= output_size))
        return;
    float2 uv = (float2(id.xy) + .5) / float2(output_size);
    float4 current = spatial_frame.Load(int3(id.xy, 0));
    float3 low = 1e10, high = -1e10;
    int2 center = int2(uv * float2(source_size));
    [unroll] for (int y = -1; y <= 1; ++y)[unroll] for (int x = -1; x <= 1; ++x) {
        float3 value = ToYCoCg(
            current_frame
                .Load(int3(clamp(center + int2(x, y), int2(0, 0), int2(source_size) - 1), 0))
                .rgb);
        low = min(low, value);
        high = max(high, value);
    }
    current.rgb = FromYCoCg(clamp(ToYCoCg(current.rgb), low, high));
    if (!history_valid) {
        accumulated[id.xy] = current;
        return;
    }
    // Bilinear flow upsampling plus a 3x3 photometrically guided dilation.
    // Reject incompatible motion near an edge. With no depth, this is a silhouette heuristic, not
    // nearest geometry.
    float4 flow = motion.SampleLevel(linear_sampler, uv, 0);
    float2 backward = flow.zw;
    float new_luma = ToYCoCg(current_frame.SampleLevel(linear_sampler, uv, 0).rgb).x;
    float best_cost = 1e10;
    uint mw, mh;
    motion.GetDimensions(mw, mh);
    int2 cell = int2(uv * float2(mw, mh));
    [unroll] for (int y = -1; y <= 1; ++y)[unroll] for (int x = -1; x <= 1; ++x) {
        float2 candidate =
            motion.Load(int3(clamp(cell + int2(x, y), int2(0, 0), int2(mw, mh) - 1), 0)).zw;
        float2 endpoint = uv + candidate * 8 / float2(source_size);
        float2 opposite = motion.SampleLevel(linear_sampler, endpoint, 0).xy;
        float candidate_luma =
            ToYCoCg(previous_frame.SampleLevel(linear_sampler, endpoint, 0).rgb).x;
        float cost = abs(new_luma - candidate_luma) + .002 * dot(candidate, candidate) +
                     .01 * float(x * x + y * y);
        // Dilate only a photometrically plausible, bidirectionally consistent
        // edge. Magnitude alone spreads unrelated background flow into it.
        if (length(candidate + opposite) <= .5 && cost < best_cost) {
            backward = candidate;
            best_cost = cost;
        }
    }
    float2 history_uv = uv + backward * 8 / float2(source_size);
    float2 forward = motion.SampleLevel(linear_sampler, history_uv, 0).xy;
    float4 history = Reconstruct(history_frame, history_uv, output_size);
    float old_luma = ToYCoCg(previous_frame.SampleLevel(linear_sampler, history_uv, 0).rgb).x;
    float reactive = saturate(abs(new_luma - old_luma) * 8);
    float disocclusion = saturate((length(backward + forward) - .5) * 2);
    float weight = best_cost < 1 ? .8 * (1 - reactive) * (1 - disocclusion) : 0;
    if (any(history_uv < 0) || any(history_uv > 1))
        weight = 0;
    float3 clamped = clamp(ToYCoCg(history.rgb), low, high);
    accumulated[id.xy] = float4(lerp(current.rgb, FromYCoCg(clamped), weight), current.a);
}
