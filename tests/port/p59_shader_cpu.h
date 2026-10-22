// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal HLSL execution adapter for the production float32 image kernels.
#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>
using uint = unsigned;
using float2 = float __attribute__((ext_vector_type(2)));
using float3 = float __attribute__((ext_vector_type(3)));
using float4 = float __attribute__((ext_vector_type(4)));
using int2 = int __attribute__((ext_vector_type(2)));
using int3 = int __attribute__((ext_vector_type(3)));
using uint2 = uint __attribute__((ext_vector_type(2)));
using uint3 = uint __attribute__((ext_vector_type(3)));
using uint4 = uint __attribute__((ext_vector_type(4)));
template <class V, class S>
V Make(S s) {
    if constexpr (std::is_arithmetic_v<S>)
        return (V)s;
    else
        return __builtin_convertvector(s, V);
}
template <class V>
V Make(float s) {
    return (V)s;
}
template <class V>
V Make(int s) {
    return (V)s;
}
template <class V>
V Make(uint s) {
    return (V)s;
}
template <class V, class A, class B>
V Make(A a, B b) {
    if constexpr (std::is_same_v<A, float3>)
        return V{a.x, a.y, a.z, b};
    else if constexpr (std::is_same_v<A, float2> && std::is_same_v<B, float2>)
        return V{a.x, a.y, b.x, b.y};
    else if constexpr (std::is_same_v<A, int2> && std::is_same_v<B, int2>)
        return V{float(a.x), float(a.y), float(b.x), float(b.y)};
    else if constexpr (std::is_same_v<A, int2> || std::is_same_v<A, uint2>)
        return V{int(a.x), int(a.y), b};
    else
        return V{a, b};
}
template <class V, class A, class B, class C>
V Make(A a, B b, C c) {
    return V{a, b, c};
}
template <class V, class A, class B, class C, class D>
V Make(A a, B b, C c, D d) {
    return V{a, b, c, d};
}
template <class V>
auto min(V a, V b) {
    return __builtin_elementwise_min(a, b);
}
template <class V>
auto max(V a, V b) {
    return __builtin_elementwise_max(a, b);
}
template <class V>
V clamp(V a, V low, V high) {
    return min(max(a, low), high);
}
template <class V>
V abs(V a) {
    return __builtin_elementwise_abs(a);
}
template <class V>
V floor(V a) {
    return __builtin_elementwise_floor(a);
}
template <class V>
V frac(V a) {
    return a - floor(a);
}
template <class V>
V saturate(V a) {
    return clamp(a, (V)0, (V)1);
}
template <class V>
V lerp(V a, V b, float t) {
    return a + (b - a) * t;
}
inline float dot(float2 a, float2 b) {
    return a.x * b.x + a.y * b.y;
}
inline float dot(float3 a, float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float length(float2 a) {
    return std::sqrt(dot(a, a));
}
inline bool any(int2 a) {
    return a.x || a.y;
}
inline bool all(int2 a) {
    return a.x && a.y;
}
template <class T>
struct Image {
    unsigned width, height;
    std::vector<T> pixels;
    Image(unsigned w, unsigned h) : width(w), height(h), pixels(w * h) {}
    T Load(int3 p) const {
        return pixels[std::clamp(p.y, 0, int(height) - 1) * width +
                      std::clamp(p.x, 0, int(width) - 1)];
    }
    T& operator[](uint2 p) {
        return pixels[p.y * width + p.x];
    }
    T SampleLevel(int, float2 uv, int) const {
        float2 p = uv * float2{float(width), float(height)} - .5f;
        auto x = int(std::floor(p.x)), y = int(std::floor(p.y));
        float2 f = frac(p);
        return lerp(lerp(Load(int3{x, y, 0}), Load(int3{x + 1, y, 0}), f.x),
                    lerp(Load(int3{x, y + 1, 0}), Load(int3{x + 1, y + 1, 0}), f.x), f.y);
    }
    void GetDimensions(uint& w, uint& h) const {
        w = width;
        h = height;
    }
    float4 Gather(float2 uv, unsigned channel) const {
        const int x = int(std::floor(uv.x * width - .5f)), y = int(std::floor(uv.y * height - .5f));
        return float4{Load(int3{x, y + 1, 0})[channel], Load(int3{x + 1, y + 1, 0})[channel],
                      Load(int3{x + 1, y, 0})[channel], Load(int3{x, y, 0})[channel]};
    }
};
