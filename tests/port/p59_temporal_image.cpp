// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled after the actual production HLSL math and the vendored AMD kernels.
float4 Truth(float x, float y) {
    return float4{.45f + .13f * std::sin(x * .14f) + .10f * std::cos(y * .19f),
                  .45f + .14f * std::cos(x * .11f + y * .12f),
                  .45f + .13f * std::sin(x * .08f - y * .13f), 1};
}
void Fill(Image<float4>& image, float shift, unsigned seed, float noise) {
    std::mt19937 random(seed);
    std::normal_distribution<float> distribution(0, noise);
    for (unsigned y = 0; y < image.height; ++y)
        for (unsigned x = 0; x < image.width; ++x) {
            auto color = Truth(x - shift, y);
            for (unsigned c = 0; c < 3; ++c)
                color[c] += distribution(random);
            image[uint2{x, y}] = color;
        }
}
void Sharpen(const Image<float4>& source, Image<float4>& output) {
    Fsr f{source};
    uint4 con{std::bit_cast<uint>(std::exp2(-.5f)), 0, 0, 0};
    for (unsigned y = 0; y < output.height; ++y)
        for (unsigned x = 0; x < output.width; ++x) {
            float r, g, b, a;
            f.FsrRcasF(r, g, b, a, uint2{x, y}, con);
            output[uint2{x, y}] = float4{r, g, b, a};
        }
}
double Error(const Image<float4>& image, unsigned w, unsigned h, float shift) {
    double error = 0;
    unsigned count = 0;
    for (unsigned y = 4; y + 4 < image.height; ++y)
        for (unsigned x = 4; x + 4 < image.width; ++x) {
            auto truth = Truth((x + .5f) * w / image.width - .5f - shift,
                               (y + .5f) * h / image.height - .5f);
            for (unsigned c = 0; c < 3; ++c) {
                double delta = image.pixels[y * image.width + x][c] - truth[c];
                assert(std::isfinite(delta));
                error += delta * delta;
                ++count;
            }
        }
    return error / count;
}
int main() {
    // Exercise the actual parameterized warp on a known constant pair. The
    // generated images must follow the schedule before the current real image.
    Image<float4> wa(8, 8), wb(8, 8), wm(1, 1), wo(8, 8);
    std::fill(wa.pixels.begin(), wa.pixels.end(), float4{.2f, .2f, .2f, 1});
    std::fill(wb.pixels.begin(), wb.pixels.end(), float4{.8f, .8f, .8f, 1});
    Warp warp{wa, wb, wm, wo, uint2{8, 8}, uint2{8, 8}, .5f};
    for (unsigned multiplier : {2u, 4u}) {
        float previous_value = .2f;
        for (unsigned i = 1; i < multiplier; ++i) {
            warp.interpolation = float(i) / multiplier;
            warp.WarpCS(uint3{4, 4, 0});
            const float value = wo[uint2{4, 4}].r;
            assert(std::abs(value - (.2f + .6f * i / multiplier)) < 1e-6);
            assert(value > previous_value && value < .8f);
            previous_value = value;
        }
    }

    constexpr unsigned w = 160, h = 96, ow = 320, oh = 192;
    Image<float4> previous(w, h), current(w, h), history(ow, oh), accumulated(ow, oh),
        spatial(ow, oh), temporal_sharp(ow, oh), spatial_sharp(ow, oh);
    Image<float4> previous_spatial(ow, oh);
    Image<float2> luma((w + 7) / 8, (h + 7) / 8);
    Image<float4> motion(luma.width, luma.height);
    Fill(previous, 0, 41, .035f);
    Fill(current, 8, 97, .035f);
    Flow flow{luma, motion, uint2{luma.width, luma.height}};
    for (unsigned y = 0; y < luma.height; ++y)
        for (unsigned x = 0; x < luma.width; ++x) {
            float2 value = 0;
            for (unsigned j = 0; j < 2; ++j)
                for (unsigned i = 0; i < 2; ++i) {
                    float2 uv =
                        (float2{float(x * 8 + i * 4), float(y * 8 + j * 4)} + 2) / float2{w, h};
                    value += float2{flow.Luma(previous.SampleLevel(0, uv, 0).rgb),
                                    flow.Luma(current.SampleLevel(0, uv, 0).rgb)} *
                             .25f;
                }
            luma[uint2{x, y}] = value;
        }
    for (unsigned y = 0; y < luma.height; ++y)
        for (unsigned x = 0; x < luma.width; ++x)
            flow.MotionCS(uint3{x, y, 0});
    unsigned correct = 0;
    for (const auto& v : motion.pixels)
        if (v.z == -1 && v.w == 0)
            ++correct;
    std::printf("Flow backward -1,0: %u/%zu\n", correct, motion.pixels.size());
    Temporal bootstrap{previous,         history,     motion,        previous, history,
                       previous_spatial, uint2{w, h}, uint2{ow, oh}, 0};
    Temporal temporal{current, history,     motion,        previous, accumulated,
                      spatial, uint2{w, h}, uint2{ow, oh}, 1};
    Fsr fsr{current};
    uint4 a, b, c, d;
    fsr.FsrEasuCon(a, b, c, d, w, h, w, h, ow, oh);
    Fsr previous_fsr{previous};
    for (unsigned y = 0; y < oh; ++y)
        for (unsigned x = 0; x < ow; ++x) {
            float3 color;
            previous_fsr.FsrEasuF(color, uint2{x, y}, a, b, c, d);
            previous_spatial[uint2{x, y}] = float4{color.x, color.y, color.z, 1};
            fsr.FsrEasuF(color, uint2{x, y}, a, b, c, d);
            spatial[uint2{x, y}] = float4{color.x, color.y, color.z, 1};
        }
    for (unsigned y = 0; y < oh; ++y)
        for (unsigned x = 0; x < ow; ++x)
            bootstrap.TemporalCS(uint3{x, y, 0});
    for (unsigned y = 0; y < oh; ++y)
        for (unsigned x = 0; x < ow; ++x)
            temporal.TemporalCS(uint3{x, y, 0});
    Sharpen(accumulated, temporal_sharp);
    Sharpen(spatial, spatial_sharp);
    double taau = Error(temporal_sharp, w, h, 8), fsr1 = Error(spatial_sharp, w, h, 8);
    std::printf("Moving noisy frames: TAAU MSE=%.8f FSR1 MSE=%.8f ratio=%.4f\n", taau, fsr1,
                taau / fsr1);
    assert(taau < fsr1);
    // Uniform cut: all history must be rejected locally, even before the
    // asynchronous global scene-difference readback reaches the CPU.
    std::fill(current.pixels.begin(), current.pixels.end(), float4{.9f, .9f, .9f, 1});
    temporal.TemporalCS(uint3{100, 80, 0});
    assert(std::abs(accumulated[uint2{100, 80}].r - .9f) < 1e-5);
}
