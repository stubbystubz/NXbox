// SPDX-License-Identifier: GPL-3.0-or-later
// Host-only execution adapter. Production HLSL math is mechanically extracted by
// p59_metal_shaders.py; this is not a D3D12/Xbox performance measurement.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using Pixel = std::array<float, 4>;
static Pixel GroundTruth(float x, float y) {
    return {0.45f + 0.13f * std::sin(x * .14f) + 0.10f * std::cos(y * .19f),
            0.45f + 0.14f * std::cos(x * .11f + y * .12f),
            0.45f + 0.13f * std::sin(x * .08f - y * .13f), 1};
}
static id<MTLTexture> Texture(id<MTLDevice> device, unsigned w, unsigned h,
                              MTLPixelFormat format = MTLPixelFormatRGBA16Float) {
    auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                   width:w
                                                                  height:h
                                                               mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModeShared;
    auto texture = [device newTextureWithDescriptor:desc];
    if (!texture)
        std::abort();
    return texture;
}
static void Fill(id<MTLTexture> image, float shift, unsigned seed, float noise) {
    std::vector<Pixel> pixels(image.width * image.height);
    std::mt19937 random(seed);
    std::normal_distribution<float> distribution(0, noise);
    for (unsigned y = 0; y < image.height; ++y)
        for (unsigned x = 0; x < image.width; ++x) {
            auto color = GroundTruth(x - shift, y);
            for (unsigned c = 0; c < 3; ++c)
                color[c] += distribution(random);
            pixels[y * image.width + x] = color;
        }
    [image replaceRegion:MTLRegionMake2D(0, 0, image.width, image.height)
             mipmapLevel:0
               withBytes:pixels.data()
             bytesPerRow:image.width * sizeof(Pixel)];
}
static double Error(id<MTLTexture> image, unsigned source_w, unsigned source_h, float shift) {
    std::vector<std::array<_Float16, 4>> pixels(image.width * image.height);
    [image getBytes:pixels.data()
        bytesPerRow:image.width * sizeof(pixels[0])
         fromRegion:MTLRegionMake2D(0, 0, image.width, image.height)
        mipmapLevel:0];
    double error = 0;
    unsigned count = 0;
    // The moving image exposes new pixels at the left edge. Score those too;
    // only omit the two-pixel reconstruction boundary common to both methods.
    for (unsigned y = 4; y + 4 < image.height; ++y)
        for (unsigned x = 4; x + 4 < image.width; ++x) {
            const auto truth = GroundTruth((x + .5f) * source_w / image.width - .5f - shift,
                                           (y + .5f) * source_h / image.height - .5f);
            for (unsigned c = 0; c < 3; ++c) {
                const double delta = float(pixels[y * image.width + x][c]) - truth[c];
                error += delta * delta;
                ++count;
            }
        }
    return error / count;
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2)
            return 2;
        auto device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::fprintf(stderr, "Metal GPU unavailable\n");
            return 77;
        }
        NSError *error = nil;
        auto source = [NSString stringWithContentsOfFile:@(argv[1])
                                                encoding:NSUTF8StringEncoding
                                                   error:&error];
        auto library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) {
            std::fprintf(stderr, "%s\n", error.description.UTF8String);
            return 1;
        }
        auto queue = [device newCommandQueue];
        std::array<id<MTLComputePipelineState>, 5> pipelines;
        NSArray<NSString *> *names = @[ @"luma", @"motion", @"temporal", @"easu", @"rcas" ];
        for (unsigned i = 0; i < pipelines.size(); ++i) {
            pipelines[i] =
                [device newComputePipelineStateWithFunction:[library newFunctionWithName:names[i]]
                                                      error:&error];
            if (!pipelines[i]) {
                std::fprintf(stderr, "%s\n", error.description.UTF8String);
                return 1;
            }
        }
        const auto dispatch = [&](id<MTLCommandBuffer> commands, unsigned pipeline,
                                  NSArray<id<MTLTexture>> *textures, unsigned valid = 0) {
            auto encoder = [commands computeCommandEncoder];
            [encoder setComputePipelineState:pipelines[pipeline]];
            for (unsigned i = 0; i < textures.count; ++i)
                [encoder setTexture:textures[i] atIndex:i];
            [encoder setBytes:&valid length:sizeof(valid) atIndex:0];
            auto output = textures.lastObject;
            [encoder dispatchThreads:MTLSizeMake(output.width, output.height, 1)
                threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [encoder endEncoding];
        };
        std::printf("GPU: %s\n", device.name.UTF8String);
        for (const auto extent : {std::array<unsigned, 4>{160, 96, 320, 192},
                                  std::array<unsigned, 4>{1280, 720, 1920, 1080},
                                  std::array<unsigned, 4>{1920, 1080, 3840, 2160}}) {
            const auto [w, h, ow, oh] = extent;
            auto previous = Texture(device, w, h, MTLPixelFormatRGBA32Float);
            auto current = Texture(device, w, h, MTLPixelFormatRGBA32Float);
            Fill(previous, 0, 41, .035f);
            Fill(current, 8, 97, .035f);
            auto luma = Texture(device, (w + 7) / 8, (h + 7) / 8, MTLPixelFormatRG32Float);
            auto motion = Texture(device, (w + 7) / 8, (h + 7) / 8, MTLPixelFormatRGBA32Float);
            auto history = Texture(device, ow, oh);
            auto accumulated = Texture(device, ow, oh);
            auto sharpened = Texture(device, ow, oh);
            auto spatial = Texture(device, ow, oh);
            auto spatial_sharp = Texture(device, ow, oh);
            // Bootstrap uses the spatial seed, matching the invalid-history branch.
            auto bootstrap = [queue commandBuffer];
            dispatch(bootstrap, 3, @[ previous, spatial ]);
            dispatch(bootstrap, 2, @[ previous, history, motion, previous, spatial, history ], 0);
            [bootstrap commit];
            [bootstrap waitUntilCompleted];
            std::vector<double> milliseconds;
            for (unsigned iteration = 0; iteration < 24; ++iteration) {
                auto commands = [queue commandBuffer];
                dispatch(commands, 0, @[ previous, current, luma ]);
                dispatch(commands, 1, @[ luma, motion ]);
                dispatch(commands, 3, @[ current, spatial ]);
                dispatch(commands, 2, @[ current, history, motion, previous, spatial, accumulated ],
                         1);
                dispatch(commands, 4, @[ accumulated, sharpened ]);
                [commands commit];
                [commands waitUntilCompleted];
                if (commands.status == MTLCommandBufferStatusError) {
                    std::fprintf(stderr, "%s\n", commands.error.description.UTF8String);
                    return 1;
                }
                if (iteration >= 4)
                    milliseconds.push_back((commands.GPUEndTime - commands.GPUStartTime) * 1000);
            }
            std::sort(milliseconds.begin(), milliseconds.end());
            std::printf("%ux%u -> %ux%u flow+EASU+TAAU+RCAS median=%.3f ms p95=%.3f ms\n", w, h, ow,
                        oh, milliseconds[milliseconds.size() / 2],
                        milliseconds[milliseconds.size() * 95 / 100]);
            if (w == 160) {
                auto baseline = [queue commandBuffer];
                dispatch(baseline, 3, @[ current, spatial ]);
                dispatch(baseline, 4, @[ spatial, spatial_sharp ]);
                [baseline commit];
                [baseline waitUntilCompleted];
                const double temporal_error = Error(sharpened, w, h, 8);
                const double spatial_error = Error(spatial_sharp, w, h, 8);
                std::printf("two moving noisy frames RGB MSE: TAAU=%.8f FSR1=%.8f ratio=%.4f\n",
                            temporal_error, spatial_error, temporal_error / spatial_error);
                if (!(temporal_error < spatial_error))
                    return 1;
            }
        }
    }
}
