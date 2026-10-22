# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production ASTC storage selection and conversion with host API doubles."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class AstcBcTests(unittest.TestCase):
    def run_cpp(self, source, environments=None):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cpp"
            exe = Path(directory) / "test"
            cpp.write_text(source)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-Wconversion",
                    "-Wno-sign-conversion",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(exe),
                ],
                check=True,
                text=True,
            )
            for overrides in environments or [{}]:
                environment = dict(os.environ, NXBOX_ASTC_BC_SKIP="")
                environment.update(overrides)
                subprocess.run([str(exe)], check=True, text=True, env=environment)

    def test_production_storage_selection(self):
        source = (D3D12 / "d3d12_texture_cache.cpp").read_text()
        start = source.index("    if (host_format.converted_astc)")
        selection = source[start : source.index("    resource = MakeTexture", start)]
        policy = D3D12 / "d3d12_astc_bc.h"
        include = (
            '#include "video_core/renderer_d3d12/d3d12_astc_bc.h"\n' if policy.exists() else ""
        )
        self.run_cpp(
            r"""
#include <cassert>
#include <cstdlib>
#include <string>
#include "video_core/renderer_d3d12/d3d12_formats.h"
"""
            + include
            + r"""
using u32=uint32_t; using u64=uint64_t;
namespace Settings {
enum class AstcRecompression { Uncompressed, Bc1, Bc3 };
struct Setting { AstcRecompression value=AstcRecompression::Bc3;
 AstcRecompression GetValue() const { return value; } };
struct { Setting astc_recompression; } values;
}
namespace VideoCommon {
enum class ImageType { e2D, e3D, e1D };
namespace ImageFlagBits { constexpr int Converted=1, CostlyLoad=2; }
struct ImageInfo {
 int format=1; ImageType type=ImageType::e2D;
 struct { u32 width=16, height=16, depth=1; } size;
 struct { int levels=1, layers=1; } resources;
 u32 num_samples=1;
};
u32 CalculateConvertedSizeBytes(const ImageInfo&, bool) { return 1234; }
}
namespace VideoCore::Surface {
bool IsPixelFormatBCn(int format) { return format==0; }
bool IsPixelFormatSRGB(int format) { return format==2; }
}
bool BisectEnabled(const char*) { return false; }
using namespace D3D12;
FormatInfo Select(const VideoCommon::ImageInfo& info) {
 auto host_format=LookupFormat(info.format==0 ? "BC3_UNORM" :
                               info.format==2 ? "ASTC_2D_8X8_SRGB" : "ASTC_2D_8X8_UNORM");
 int flags=0;
 [[maybe_unused]] bool astc_uncompressed=false;
 [[maybe_unused]] bool astc_bc_candidate=false;
 [[maybe_unused]] u32 converted_size_bytes=0;
 [[maybe_unused]] u64 gpu_addr=0;
"""
            + selection
            + r"""
 assert(flags & VideoCommon::ImageFlagBits::CostlyLoad || info.format==0);
 if (astc_uncompressed) {
  assert(converted_size_bytes==1234);
  assert(host_format.block_width==1);
 }
 return host_format;
}
int main() {
 VideoCommon::ImageInfo info;
 if (const char* rule=std::getenv("NXBOX_ASTC_BC_SKIP"); rule && *rule) {
  assert(Select(info).block_width==1);
  return 0;
 }
 assert(Select(info).block_width==4); // Keep the memory saving for ordinary assets.
 info.format=2;
 assert(Select(info).view_format==LookupFormat("BC3_SRGB").view_format);
 for (int layers : {2, 3, 6, 12}) {
  info.resources.layers=layers;
  assert(Select(info).block_width==1); // ASTC must not bypass the Xbox array/cube protection.
 }
 info.resources.layers=1; info.type=VideoCommon::ImageType::e3D; info.size.depth=4;
 assert(Select(info).block_width==1);
 info.type=VideoCommon::ImageType::e1D; info.size.depth=1;
 assert(Select(info).block_width==1);
 info.type=VideoCommon::ImageType::e2D;
 for (u32 width : {1u,2u,5u,6u}) {
  info.size.width=width;
  assert(Select(info).block_width==1); // BC base padding must not change UVs/mip dimensions.
 }
 info.size.width=16; info.size.height=7;
 assert(Select(info).block_width==1);
 info.size.height=16;
 // Existing native BC protection is preserved.
 info.type=VideoCommon::ImageType::e2D; info.format=0; info.resources.layers=6;
 assert(Select(info).block_width==1);
}
""",
            environments=[{}, {"NXBOX_ASTC_BC_SKIP": "all"}, {"NXBOX_ASTC_BC_SKIP": "8X8"}],
        )

    def test_skip_substrings_and_partial_block_classes(self):
        self.run_cpp(r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_astc_bc.h"
using namespace D3D12;
int main() {
 const auto plain=AstcBcClasses(16,16,1,1,"ASTC_2D_8X8_UNORM");
 const auto tail=AstcBcClasses(16,16,5,1,"ASTC_2D_8X8_SRGB");
 const auto cube=AstcBcClasses(8,8,1,6,"ASTC_2D_4X4_UNORM");
 assert(!AstcBcSkip("", plain));
 assert(!AstcBcSkip(",,", plain));
 assert(!AstcBcSkip("partial", plain));
 assert(AstcBcSkip("partial", tail));
 assert(AstcBcSkip("array", cube) && AstcBcSkip("cube", cube));
 assert(AstcBcSkip("alpha", plain));
 assert(AstcBcSkip("srgb", tail) && !AstcBcSkip("srgb", plain));
 assert(AstcBcSkip("8X8", plain));
 assert(AstcBcSkip("missing,srgb", tail));
 assert(AstcBcSkip("all", plain));
 assert(!AstcBcSkip("SRgb", tail));
 const auto addressed=AstcBcClasses(16,8,1,1,"ASTC_2D_8X8_UNORM",0x123456789abcdef0ULL);
 assert(AstcBcSkip("gpu=123456789abcdef0", addressed));
 assert(AstcBcSkip("size=16x8", addressed));
}
""")

    def test_production_conversion_keeps_volume_mip_offsets_and_rgba_override(self):
        source = (ROOT / "src/video_core/texture_cache/util.cpp").read_text()
        start = source.index("void ConvertImage(")
        conversion = source[start : source.index("\n}\n", start) + 3]
        start = source.index("u32 CalculateConvertedSizeBytes(")
        sizes = source[start : source.index("\n}\n", start) + 3]
        self.run_cpp(
            r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>
using u8=uint8_t; using u32=uint32_t; using s32=int32_t;
#define ASSERT(value) assert(value)
namespace Common {
 u32 AlignUp(u32 value, u32 alignment) { return (value+alignment-1)/alignment*alignment; }
 template<class T> struct ScratchBuffer : std::vector<T> {
  void resize_destructive(size_t count) { this->resize(count); }
 };
}
namespace NxboxStall { enum class Kind { Convert }; struct Scope { explicit Scope(Kind) {} }; }
namespace Settings {
 enum class AstcRecompression { Uncompressed, Bc1, Bc3 };
 struct Setting { AstcRecompression value=AstcRecompression::Bc3;
  AstcRecompression GetValue() const { return value; } };
 struct { Setting astc_recompression; } values;
}
struct Extent2D { u32 width, height; };
struct Extent3D { u32 width, height, depth; bool operator==(const Extent3D&) const=default; };
struct Offset3D { int x=0,y=0,z=0; bool operator==(const Offset3D&) const=default; };
enum class ImageType { e2D, e3D, Buffer };
enum class PixelFormat { Astc, A8B8G8R8_UNORM };
struct ImageInfo {
 PixelFormat format=PixelFormat::Astc; ImageType type=ImageType::e3D;
 Extent3D size{8,8,4}; struct { s32 levels=4, layers=1; } resources;
};
struct BufferImageCopy {
 size_t buffer_offset=0, buffer_size=0;
 u32 buffer_row_length=0, buffer_image_height=0;
 struct { s32 base_level=0, base_layer=0, num_layers=1; } image_subresource;
 Offset3D image_offset; Extent3D image_extent;
};
Extent3D AdjustMipSize(Extent3D size, s32 level) {
 return {std::max(1u,size.width>>level),std::max(1u,size.height>>level),std::max(1u,size.depth>>level)};
}
bool IsPixelFormatASTC(PixelFormat format) { return format==PixelFormat::Astc; }
Extent2D DefaultBlockSize(PixelFormat) { return {8,8}; }
u32 BytesPerBlock(PixelFormat) { return 4; }
u32 ConvertedBytesPerBlock(PixelFormat) { return 4; }
u32 NumBlocksPerLayer(const ImageInfo& info, Extent2D) {
 u32 result=0;
 for (s32 level=0;level<info.resources.levels;++level) {
  const auto size=AdjustMipSize(info.size,level);
  result+=size.width*size.height*size.depth;
 }
 return result;
}
void DecompressBCn(std::span<const u8>,std::span<u8>,const BufferImageCopy&,PixelFormat) { assert(false); }
namespace Tegra::Texture::ASTC {
 void Decompress(std::span<const u8> input,u32 width,u32 height,u32 slices,u32,u32,std::span<u8> output) {
  const size_t bytes=size_t{width}*height*slices*4;
  assert(bytes<=output.size());
  std::fill_n(output.begin(),bytes,input[0]);
 }
}
namespace Tegra::Texture::BCN {
 void CompressBC3(std::span<const u8> input,u32 width,u32 height,u32 slices,std::span<u8> output) {
  const size_t bytes=size_t{Common::AlignUp(width,4)}*Common::AlignUp(height,4)*slices;
  assert(bytes<=output.size()); std::fill_n(output.begin(),bytes,input[0]);
 }
 void CompressBC1(std::span<const u8>,u32,u32,u32,std::span<u8>) { assert(false); }
}
"""
            + sizes
            + conversion
            + r"""
int main() {
 ImageInfo info;
 assert(CalculateConvertedSizeBytes(info,false)==320); // 256+32+16+16, depth shrinks per mip.
 assert(CalculateConvertedSizeBytes(info,true)==1172); // (256+32+4+1)*4 RGBA bytes.
 for (bool rgba : {true,false}) {
  std::vector<BufferImageCopy> copies;
  std::vector<u8> input(4);
  for (s32 level=0;level<4;++level) {
   const auto size=AdjustMipSize(info.size,level);
   BufferImageCopy copy;
   copy.image_extent=size; copy.image_subresource.base_level=level;
   copy.buffer_offset=static_cast<size_t>(level);
   copy.buffer_row_length=Common::AlignUp(size.width,8);
   copy.buffer_image_height=Common::AlignUp(size.height,8);
   copies.push_back(copy); input[static_cast<size_t>(level)]=static_cast<u8>(level+1);
  }
  std::vector<u8> output(CalculateConvertedSizeBytes(info,rgba),0);
  ConvertImage(input,info,output,copies,rgba);
  size_t expected=0;
  for (const auto& copy : copies) {
   const auto size=copy.image_extent;
   const size_t bytes=rgba ? size_t{size.width}*size.height*size.depth*4
                         : size_t{Common::AlignUp(size.width,4)}*Common::AlignUp(size.height,4)*size.depth;
   assert(copy.buffer_offset==expected);
   for (size_t index=expected;index<expected+bytes;++index)
    assert(output[index]==copy.image_subresource.base_level+1);
   expected+=bytes;
  }
  assert(expected==output.size());
 }
}
"""
        )

    def test_worker_and_accounting_use_the_latched_image_layout(self):
        cache = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        cache = " ".join(cache.split())
        self.assertIn("astc_uncompressed = image.astc_uncompressed", cache)
        self.assertIn("copies_span, astc_uncompressed)", cache)
        self.assertEqual(cache.count("? image.converted_size_bytes"), 2)
        self.assertIn("mapped_span, copies, image.astc_uncompressed)", cache)


if __name__ == "__main__":
    unittest.main()
