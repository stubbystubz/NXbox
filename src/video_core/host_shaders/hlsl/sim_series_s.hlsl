// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
ByteAddressBuffer timestamps : register(t0);
RWByteAddressBuffer output : register(u0);
cbuffer Parameters : register(b0) {
    uint segment_count;
    float extra_factor;
    float iterations_per_tick;
    float maximum_ticks;
};

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    float ticks = 0.0;
    for (uint segment = 0; segment < segment_count; ++segment) {
        uint2 start = timestamps.Load2(segment * 16);
        uint2 end = timestamps.Load2(segment * 16 + 8);
        // Subtract as integers before conversion: absolute timestamps lose float precision.
        uint low = end.x - start.x;
        uint high = end.y - start.y - uint(end.x < start.x);
        ticks += float(low) + float(high) * 4294967296.0;
    }
    float target_ticks = min(ticks * extra_factor, maximum_ticks);
    float requested_iterations = target_ticks * iterations_per_tick;
    // Bootstrap calibration without trusting GPU clock rates or host CPU timings.
    uint iterations =
        iterations_per_tick > 0.0 ? uint(clamp(requested_iterations, 1.0, 4194304.0)) : 4096;
    uint value = id.x + 1;
    [loop] for (uint i = 0; i < iterations; ++i) {
        value = (value ^ (value >> 13)) * 1664525u + 1013904223u;
        value ^= value << 7;
    }
    // Observable, distinct writes prevent DXC from deleting the dependent ALU loop.
    output.Store(8 + id.x * 4, value);
    if (id.x == 0)
        output.Store2(0, uint2(iterations, uint(ticks * extra_factor >= maximum_ticks ||
                                                requested_iterations >= 4194304.0)));
}
