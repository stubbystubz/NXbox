// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
#include "common/nxbox_seam_diagnostics.h"
#include "shader_recompiler/backend/hlsl/hlsl_draw_texture.h"
#include "video_core/renderer_d3d12/d3d12_index_converter.h"

using namespace D3D12::IndexConversion;
namespace {
struct Point {
    std::int64_t x, y;
};
std::int64_t Edge(Point a, Point b, Point p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}
bool Covered(const std::array<Point, 4>& corners, const std::array<std::uint32_t, 6>& list,
             std::size_t first, Point sample) {
    for (std::size_t edge = 0; edge < 3; ++edge) {
        const auto a = corners[list[first + edge]];
        const auto b = corners[list[first + (edge + 1) % 3]];
        const auto distance = Edge(a, b, sample);
        // A consistent half-open edge convention. Both directions of a shared
        // edge cannot own a sample; the choice of screen Y orientation is immaterial.
        const bool inclusive = b.y < a.y || (b.y == a.y && b.x > a.x);
        if (distance < 0 || (distance == 0 && !inclusive))
            return false;
    }
    return true;
}
void Check(Topology topology, Format format, Provoking provoking) {
    constexpr std::int64_t width = 32 * 256;
    constexpr std::int64_t height = 18 * 256;
    const bool strip = topology == Topology::QuadStrip || topology == Topology::TriangleStrip;
    const std::array<Point, 4> corners =
        strip ? std::array<Point, 4>{{{0, 0}, {0, height}, {width, 0}, {width, height}}}
              : std::array<Point, 4>{{{0, 0}, {width, 0}, {width, height}, {0, height}}};
    const auto bytes_per_index = std::size_t{1} << static_cast<unsigned>(format);
    std::vector<std::byte> input(4 * bytes_per_index);
    for (std::size_t index = 0; index < 4; ++index)
        input[index * bytes_per_index] = static_cast<std::byte>(index);
    const auto layout = Analyze(topology, format, input);
    assert(layout.count == 6);
    std::vector<std::byte> output(layout.bytes);
    Convert(topology, format, input, output, {}, provoking);
    const auto output_width = layout.bytes / layout.count;
    std::array<std::uint32_t, 6> list{};
    for (std::size_t index = 0; index < list.size(); ++index)
        for (std::size_t byte = 0; byte < output_width; ++byte)
            list[index] |=
                std::uint32_t(std::to_integer<unsigned char>(output[index * output_width + byte]))
                << (8 * byte);
    // Strip input above has negative orientation for the ordinary triangle strip.
    // Normalize winding for the independent coverage reference.
    for (const std::size_t first : {std::size_t{0}, std::size_t{3}}) {
        const auto area =
            Edge(corners[list[first]], corners[list[first + 1]], corners[list[first + 2]]);
        assert(area != 0);
        if (area < 0)
            std::swap(list[first + 1], list[first + 2]);
    }
    unsigned shared_edges{};
    for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t b = 0; b < 3; ++b)
            shared_edges += list[a] == list[3 + (b + 1) % 3] && list[(a + 1) % 3] == list[3 + b];
    assert(shared_edges == 1);
    // Test centers, fractional sample positions, and samples EXACTLY on the seam.
    // The latter are where checking winding/count alone misses double ownership.
    const auto check_sample = [&](Point sample) {
        assert(unsigned(Covered(corners, list, 0, sample)) +
                   unsigned(Covered(corners, list, 3, sample)) ==
               1);
    };
    for (std::int64_t y = 0; y < 18; ++y)
        for (std::int64_t x = 0; x < 32; ++x)
            for (const std::int64_t offset : {64, 128, 192})
                check_sample({x * 256 + offset, y * 256 + offset});
    for (std::int64_t step = 1; step < 256; ++step)
        check_sample({width * step / 256, height * step / 256});
    const auto generated_layout = AnalyzeGenerated(topology, 4, 17);
    std::vector<std::byte> generated(generated_layout.bytes);
    Generate(topology, 4, generated, 17, provoking);
    // The generated path must preserve the same endpoints with a nonzero first vertex.
    for (std::size_t index = 0; index < 6; ++index) {
        const auto original = std::to_integer<unsigned char>(output[index * output_width]);
        assert(std::to_integer<unsigned char>(generated[index * 2]) == original + 17);
        assert(generated[index * 2 + 1] == std::byte{0});
    }
}
} // namespace
int main(int argc, char** argv) {
    assert(argc == 2);
    assert(Nxbox::SeamDiagnostics::Mode() == static_cast<std::uint32_t>(argv[1][0] - '0'));
    using Shader::Backend::HLSL::DrawTextureFillsViewport;
    assert(DrawTextureFillsViewport(0, 0, 1920, 1080, 1920, 1080));
    assert(!DrawTextureFillsViewport(0.25f, 0, 1920, 1080, 1920, 1080));
    assert(!DrawTextureFillsViewport(0, 0, 1919.75f, 1080, 1920, 1080));
    assert(!DrawTextureFillsViewport(1920, 0, 0, 1080, 1920, 1080));
    assert(!DrawTextureFillsViewport(0, 0, 0, 0, 0, 0));
    for (const auto topology : {Topology::Quads, Topology::QuadStrip, Topology::TriangleFan,
                                Topology::TriangleStrip, Topology::Polygon})
        for (const auto format : {Format::Uint8, Format::Uint16, Format::Uint32})
            for (const auto provoking : {Provoking::Preserve, Provoking::First, Provoking::Last})
                Check(topology, format, provoking);
}
