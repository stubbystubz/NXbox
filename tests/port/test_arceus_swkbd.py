# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check shared applet buffers against the production block-linear address calculation."""

import tempfile
import unittest
from pathlib import Path

from test_d3d12_capture import compile_run

ROOT = Path(__file__).resolve().parents[2]


def extract_function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class ArceusSoftwareKeyboardTests(unittest.TestCase):
    def test_shared_buffer_stride_addresses_pixels_within_its_pool_slot(self):
        source = (ROOT / "src/core/hle/service/vi/shared_buffer_manager.cpp").read_text()
        constants = source.split("constexpr auto SharedBufferBlockLinearFormat", 1)[1].split(
            "constexpr SharedMemoryPoolLayout", 1
        )[0]
        make_buffer = extract_function(source, "void MakeGraphicBuffer(")
        decoders = (ROOT / "src/video_core/textures/decoders.cpp").read_text()
        gob_offset = extract_function(decoders, "u64 GetGOBOffset(")
        driver = r"""
#include <cassert>
#include <cstdint>
#include <memory>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
#define ASSERT assert
namespace android {
enum class PixelFormat { Rgba8888 };
enum class Status { NoError };
struct NvGraphicBuffer {
    u32 width{}, height{}, stride{}, buffer_id{}, offset{};
    PixelFormat format{}, external_format{};
};
struct BufferQueueProducer {
    std::shared_ptr<NvGraphicBuffer> buffers[2];
    Status SetPreallocatedBuffer(u32 slot, std::shared_ptr<NvGraphicBuffer> buffer) {
        buffers[slot] = buffer;
        return Status::NoError;
    }
};
}
constexpr u32 GOB_SIZE_X = 64, GOB_SIZE_Y = 8, GOB_SIZE = 512;
"""
        driver += "constexpr auto SharedBufferBlockLinearFormat" + constants
        driver += make_buffer + "\n" + gob_offset
        driver += r"""
int main() {
    assert(!SharedBufferAsync);
    assert(SharedBufferSize == SharedBufferSlotSize * 7u);
    android::BufferQueueProducer producer;
    for (u32 slot = 0; slot < 2; ++slot) {
        MakeGraphicBuffer(producer, slot, slot + 3, 42);
        const auto& buffer = *producer.buffers[slot];
        assert(buffer.width == 1280 && buffer.height == 720);
        assert(buffer.offset == (slot + 3) * 3932160u);
        assert(buffer.buffer_id == 42);
        // Presenters take stride in pixels and apply RGBA's four bytes themselves.
        // Every GOB in the visible image must address the same slot the guest writes.
        for (u32 y = 0; y < buffer.height; ++y) {
            for (u32 x : {0u, 1279u}) {
                const auto guest_offset = GetGOBOffset(1280, 720, x, y, 4, 4);
                const auto display_offset = GetGOBOffset(buffer.stride, buffer.height, x, y, 4, 4);
                assert(display_offset == guest_offset);
                assert(display_offset + GOB_SIZE <= SharedBufferSlotSize);
            }
        }
        assert(buffer.stride == buffer.width);
        // Regression signature: the old byte stride maps display row 128 to guest
        // row 512, hence the observed keyboard band at output y=192 on a 1080p TV.
        assert(GetGOBOffset(5120, 720, 0, 128, 4, 4) ==
               GetGOBOffset(1280, 720, 0, 512, 4, 4));
    }
}
"""
        with tempfile.TemporaryDirectory() as directory:
            compile_run(
                driver,
                Path(directory),
                extra_flags=[
                    "-Wall",
                    "-Wextra",
                    "-Wno-unused-parameter",
                    "-Wconversion",
                    "-Wshadow",
                    "-Werror",
                ],
            )


if __name__ == "__main__":
    unittest.main()
