# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Executable coverage of every Maxwell pixel format and conversion family."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/video_core/renderer_d3d12/d3d12_formats.h"
DRIVER = r"""
#include <cassert>
#include <array>
#include <cstring>
#include <random>
#include "video_core/renderer_d3d12/d3d12_formats.h"
using namespace D3D12;
int main() {
    for (const auto& format : FormatTable) {
        assert(format.Supported());
        assert(format.view_format && format.bytes_per_block);
        assert(format.planes == (format.conversion == FormatConversion::DepthStencil ? 2u : 1u));
    }
    assert(!LookupFormat("Invalid").Supported());
    for (const auto& format : FormatTable) {
        for (unsigned selector = 0; selector < 256; ++selector) assert(FormatComponent(format, selector) <= 5);
        assert(FormatComponent(format, 0) == 4 && FormatComponent(format, 1) == 4);
        assert(FormatComponent(format, 6) == 5 && FormatComponent(format, 7) == 5);
        if (format.depth) {
            assert(format.DsvFormat() != 0);
            assert(DepthViewPlane(format, true, true) == 0);
            assert(DepthViewPlane(format, false, true) == 0);
        }
    }
    assert(DepthViewPlane(LookupFormat("S8_UINT"), true, false) == 1);
    assert(DepthViewPlane(LookupFormat("S8_UINT_D24_UNORM"), true, false) == 1);
    assert(DepthViewPlane(LookupFormat("S8_UINT_D24_UNORM"), false, false) == 0);
    assert(DepthViewPlane(LookupFormat("D24_UNORM_S8_UINT"), true, false) == 0);
    assert(DepthViewPlane(LookupFormat("D24_UNORM_S8_UINT"), false, false) == 1);
    assert(FormatComponent(LookupFormat("R16G16B16X16_FLOAT"), 5) == 5);
    for (const auto name : {"B5G6R5_UNORM", "A1B5G5R5_UNORM", "A5B5G5R1_UNORM", "A4B4G4R4_UNORM", "G4R4_UNORM"}) {
        for (unsigned bits = 0; bits < (1u << (8 * PackedBytes(name))); ++bits) {
            std::array<std::uint8_t, 4> guest{}, restored{}, host{};
            std::memcpy(guest.data(), &bits, PackedBytes(name));
            ConvertPackedRow(name, host.data(), guest.data(), 1, false);
            ConvertPackedRow(name, host.data(), restored.data(), 1, true);
            assert(guest == restored);
        }
    }
    for (unsigned bits = 0; bits < 100000; ++bits) {
        const std::uint32_t value = bits * 42949u;
        std::array<std::uint8_t, 4> guest{}, restored{};
        std::array<std::uint8_t, 8> host{};
        std::memcpy(guest.data(), &value, 4);
        ConvertPackedRow("A2R10G10B10_UNORM", host.data(), guest.data(), 1, false);
        ConvertPackedRow("A2R10G10B10_UNORM", host.data(), restored.data(), 1, true);
        assert(guest == restored);
    }
    // Canonical host channels must match the guest's bit significance.
    std::array<std::uint8_t, 4> guest{31, 0, 0, 0}, host{};
    ConvertPackedRow("B5G6R5_UNORM", host.data(), guest.data(), 1, false);
    assert(host[0] == 255 && host[1] == 0 && host[2] == 0 && host[3] == 255);
    guest = {1, 0, 0, 0};
    ConvertPackedRow("A5B5G5R1_UNORM", host.data(), guest.data(), 1, false);
    assert(host[0] == 255 && host[3] == 0);
    for (const auto name : {"D24_UNORM_S8_UINT", "S8_UINT_D24_UNORM", "D32_FLOAT_S8_UINT", "S8_UINT", "X8_D24_UNORM"}) {
        std::array<std::uint8_t, 8> packed{0x12,0x34,0x56,0x78,0x90,0,0,0}, restored = packed;
        std::array<std::uint8_t, 4> depth{}, stencil{};
        ConvertDepthRow(name, depth.data(), packed.data(), 1, 0, false);
        ConvertDepthRow(name, stencil.data(), packed.data(), 1, 1, false);
        if (std::string_view(name) == "S8_UINT_D24_UNORM") assert(depth[0] == 0x34 && stencil[0] == 0x12);
        if (std::string_view(name) == "D24_UNORM_S8_UINT") assert(depth[0] == 0x12 && stencil[0] == 0x78);
        if (std::string_view(name) == "D32_FLOAT_S8_UINT") assert(stencil[0] == 0x90);
        // Preserve padding/X bits, which D3D12 does not expose as depth/stencil.
        ConvertDepthRow(name, depth.data(), restored.data(), 1, 0, true);
        ConvertDepthRow(name, stencil.data(), restored.data(), 1, 1, true);
        assert(restored == packed);
    }
    std::array<std::uint8_t, 16> compressed{};
    std::array<std::uint8_t, 64> decoded{};
    DecodeEtcBlock(LookupFormat("ETC2_RGB_UNORM"), compressed.data(), decoded.data());
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        assert(decoded[pixel*4] == 2 && decoded[pixel*4+1] == 2 && decoded[pixel*4+2] == 2);
        assert(decoded[pixel*4+3] == 255);
    }
    DecodeEtcBlock(LookupFormat("ETC2_RGB_PTA_UNORM"), compressed.data(), decoded.data());
    for (unsigned pixel = 0; pixel < 16; ++pixel) assert(decoded[pixel*4+3] == 255);
    DecodeEtcBlock(LookupFormat("ETC2_RGBA_UNORM"), compressed.data(), decoded.data());
    for (unsigned pixel = 0; pixel < 16; ++pixel) assert(decoded[pixel*4+3] == 0);
    // Signed EAC decodes a signed base codeword before applying the modifier,
    // rather than reinterpreting unsigned normalized output as signed data.
    compressed[0] = 0x80;
    DecodeEtcBlock(LookupFormat("EAC_R11_SNORM"), compressed.data(), decoded.data());
    std::int16_t signed_sample{};
    std::memcpy(&signed_sample, decoded.data(), 2);
    assert(signed_sample == (-1019 * 32767 / 1023));
    compressed.fill(0);
    DecodeEtcBlock(LookupFormat("EAC_R11_UNORM"), compressed.data(), decoded.data());
    std::uint16_t sample{};
    std::memcpy(&sample, decoded.data(), 2);
    assert(sample == 65535 / 2047);
    // Exercise T/H/planar modes, all selectors, odd/aligned input addresses and
    // both EAC channels under ASan/UBSan without any copyrighted game data.
    std::mt19937 random(0xD312);
    for (unsigned i = 0; i < 10000; ++i) {
        std::array<std::uint8_t, 17> input{};
        for (auto& value : input) value = static_cast<std::uint8_t>(random());
        for (const auto& format : FormatTable) {
            if (format.conversion == FormatConversion::Etc2 || format.conversion == FormatConversion::Eac)
                DecodeEtcBlock(format, input.data() + 1, decoded.data());
        }
    }
}
"""


RUNTIME_DRIVER = r"""
#include <cstddef>
#include "bc_decoder.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_formats.h"
using u8=std::uint8_t; using u32=std::uint32_t; using u64=std::uint64_t;
using s32=std::int32_t; using UINT=unsigned; using UINT16=std::uint16_t; using UINT64=std::uint64_t;
namespace VideoCommon {
enum class ImageType { e2D, e3D };
struct Extent3D { u32 width{},height{},depth{}; };
struct Offset3D { s32 x{},y{},z{}; };
struct Subresource { s32 base_level{},base_layer{},num_layers{}; };
struct BufferImageCopy {
    u64 buffer_offset{}; u32 buffer_row_length{},buffer_image_height{};
    Subresource image_subresource; Offset3D image_offset; Extent3D image_extent;
};
struct ImageInfo {
    std::string_view format; ImageType type{ImageType::e2D}; Extent3D size;
    struct { s32 levels{1}, layers{2}; } resources;
};
}
namespace VideoCore::Surface {
u32 BytesPerBlock(std::string_view name) {
    const auto f=D3D12::LookupFormat(name);
    if(f.conversion==D3D12::FormatConversion::PackedColor) return D3D12::PackedBytes(name);
    if(name=="S8_UINT") return 1;
    if(f.conversion==D3D12::FormatConversion::Etc2) return name.find("RGBA")!=name.npos ? 16 : 8;
    if(f.conversion==D3D12::FormatConversion::Eac) return f.bytes_per_block*4;
    return f.bytes_per_block;
}
u32 DefaultBlockWidth(std::string_view name) { const auto f=D3D12::LookupFormat(name); return name.starts_with("BC") || f.conversion==D3D12::FormatConversion::Etc2 || f.conversion==D3D12::FormatConversion::Eac ? 4 : 1; }
u32 DefaultBlockHeight(std::string_view name) { return DefaultBlockWidth(name); }
}
struct D3D12_RESOURCE_DESC { u64 Width{}; UINT Height{},Format{}; UINT16 DepthOrArraySize{},MipLevels{}; };
struct D3D12_SUBRESOURCE_FOOTPRINT { UINT Format{},Width{},Height{},Depth{},RowPitch{}; };
struct D3D12_PLACED_SUBRESOURCE_FOOTPRINT { u64 Offset{}; D3D12_SUBRESOURCE_FOOTPRINT Footprint; };
constexpr int D3D12_RESOURCE_STATE_COPY_SOURCE=1, D3D12_RESOURCE_STATE_COPY_DEST=2;
constexpr int D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT=1, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX=2;
struct Resource {
    D3D12_RESOURCE_DESC desc;
    std::vector<u8> bytes;
    std::span<u8> mapped;
    std::vector<std::vector<u8>> planes;
    struct Handle {
        Resource* pointer;
        Resource* Get() const { return pointer; }
        Resource* operator->() const { return pointer; }
    } handle{this};
    D3D12_RESOURCE_DESC GetDesc() const { return desc; }
};
using ResourcePtr=std::shared_ptr<Resource>;
struct StagingBufferRef { ResourcePtr buffer; size_t offset{}; std::span<u8> mapped_span; };
struct D3D12_TEXTURE_COPY_LOCATION {
    Resource* pResource{}; int Type{}; D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint;
    UINT SubresourceIndex{};
};
struct D3D12_BOX { UINT left{},top{},front{},right{},bottom{},back{}; };
namespace D3D12 {
FormatInfo MaxwellFormat(std::string_view name) { return LookupFormat(name); }
unsigned warnings{};
void UnsupportedImage(const VideoCommon::ImageInfo&,FormatInfo,std::string_view) { ++warnings; }
UINT Subresource(const VideoCommon::ImageInfo& info,s32 level,s32 layer) { return static_cast<UINT>(level+layer*info.resources.levels); }
struct FakeDevice {
    void GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT plane, UINT, UINT64,
                              D3D12_PLACED_SUBRESOURCE_FOOTPRINT* out, UINT* rows, UINT64* row_bytes, UINT64* total) {
        const bool depth=desc->Format==44 || desc->Format==19;
        const UINT bytes=depth ? (plane ? 1 : 4) : desc->Format==9 ? 8 : desc->Format==53 || desc->Format==48 ? 2 : desc->Format==60 ? 1 : 4;
        *row_bytes=desc->Width*bytes;
        const UINT pitch=(static_cast<UINT>(*row_bytes)+255)&~255u;
        *rows=desc->Height; *total=u64{pitch}*desc->Height*desc->DepthOrArraySize;
        out->Footprint={desc->Format, static_cast<UINT>(desc->Width),desc->Height,desc->DepthOrArraySize,pitch};
    }
};
struct FakeCommands {
    void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* target, UINT x, UINT y, UINT z,
                           const D3D12_TEXTURE_COPY_LOCATION* source,const D3D12_BOX* box) {
        const bool download=target->Type==D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        const auto& linear=download ? *target : *source;
        const auto& texture=download ? *source : *target;
        const auto& fp=linear.PlacedFootprint.Footprint;
        const UINT plane=texture.SubresourceIndex/(texture.pResource->desc.MipLevels*texture.pResource->desc.DepthOrArraySize);
        const UINT bytes=fp.Format==44 || fp.Format==19 ? (plane ? 1 : 4) : fp.Format==9 ? 8 : fp.Format==53 || fp.Format==48 ? 2 : fp.Format==60 ? 1 : 4;
        auto& resident=texture.pResource->planes.at(texture.SubresourceIndex);
        const UINT sx=download && box ? box->left : x;
        const UINT sy=download && box ? box->top : y;
        const UINT sz=download && box ? box->front : z;
        for(UINT d=0;d<fp.Depth;++d) for(UINT row=0;row<fp.Height;++row) {
            auto* tight=resident.data()+((sz+d)*texture.pResource->desc.Height*texture.pResource->desc.Width+(sy+row)*texture.pResource->desc.Width+sx)*bytes;
            auto* padded=linear.pResource->bytes.data()+linear.PlacedFootprint.Offset+(u64{d}*fp.Height+row)*fp.RowPitch;
            if(download) std::memcpy(padded,tight,fp.Width*bytes);
            else std::memcpy(tight,padded,fp.Width*bytes);
        }
    }
};
enum class FrameCount { StagingCopyBytes, ForcedSubmitTextureDownload, CpuReinterpretBytes };
struct FrameProfile { bool Enabled() const { return false; } void Count(FrameCount, u64) {} };
struct Context {
    FrameProfile profile; FrameProfile& Profile() { return profile; }
    FakeDevice device; FakeCommands commands; std::vector<std::function<void()>> pending;
    FakeDevice* Device() { return &device; } FakeCommands* Commands() { return &commands; }
    void Retain(const ResourcePtr&) {} void Transition(const ResourcePtr&,int) {} void MarkWork(const char*) {}
    void BeforeCopyWrite(const ResourcePtr&, u64, u64) {}
    void AfterCompletion(std::function<void()> callback) { pending.push_back(std::move(callback)); }
    void Finish(FrameCount = FrameCount::ForcedSubmitTextureDownload) { for(auto& callback:pending) callback(); pending.clear(); }
};
struct TextureCacheRuntime {
    Context context;
    StagingBufferRef UploadStagingBuffer(size_t size) {
        auto buffer=std::make_shared<Resource>(); buffer->bytes.resize(size+512,0xCD); buffer->mapped=buffer->bytes;
        return {buffer,512,buffer->mapped.subspan(512)};
    }
    StagingBufferRef DownloadStagingBuffer(size_t size) { return UploadStagingBuffer(size); }
};
struct Image {
    VideoCommon::ImageInfo info; FormatInfo host_format; ResourcePtr resource; TextureCacheRuntime* runtime{};
    bool TransferConverted(const StagingBufferRef&,std::span<const VideoCommon::BufferImageCopy>,bool);
};
__CONVERTED__
} // namespace D3D12
int main() {
    using namespace D3D12;
    auto formats=std::vector<FormatInfo>(FormatTable.begin(),FormatTable.end());
    for(const auto& native:FormatTable) if(native.name.starts_with("BC")) formats.push_back(BcnFallbackFormat(native));
    for(const auto& format:formats) {
        if(format.conversion==FormatConversion::None) continue;
        TextureCacheRuntime runtime;
        Image image;
        image.info.format=format.name; image.info.size={5,3,1}; image.host_format=format; image.runtime=&runtime;
        image.resource=std::make_shared<Resource>();
        image.resource->desc={5,3,format.resource_format,2,1};
        for(UINT plane=0;plane<format.planes;++plane) for(unsigned layer=0;layer<2;++layer)
            image.resource->planes.emplace_back(5*3*(format.depth ? (plane ? 1u : 4u) : format.bytes_per_block));
        const u32 block=VideoCore::Surface::DefaultBlockWidth(format.name);
        const u32 bytes=VideoCore::Surface::BytesPerBlock(format.name);
        const u32 rows=(3+block-1)/block, pitch=(8/block)*bytes;
        auto map=runtime.UploadStagingBuffer(pitch*rows*2+19);
        for(size_t i=0;i<map.mapped_span.size();++i) map.mapped_span[i]=static_cast<u8>(i*37);
        const auto original=std::vector<u8>(map.mapped_span.begin(),map.mapped_span.end());
        VideoCommon::BufferImageCopy copy{19,8,block==4 ? 4u : 3u,{0,0,2},{0,0,0},{5,3,1}};
        assert(image.TransferConverted(map,std::span{&copy,1},false));
        if(format.conversion==FormatConversion::PackedColor || format.depth) {
            // Overwrite only the live guest pixel bytes, leaving padding unchanged.
            for(unsigned layer=0;layer<2;++layer) for(unsigned y=0;y<3;++y)
                std::memset(map.mapped_span.data()+19+layer*pitch*rows+y*pitch,0,5*bytes);
            assert(image.TransferConverted(map,std::span{&copy,1},true));
            runtime.context.Finish();
            // X8 and D32S8 padding is not stored on the GPU.
            for(unsigned layer=0;layer<2;++layer) for(unsigned y=0;y<3;++y) for(unsigned x=0;x<5;++x)
                for(unsigned b=0;b<bytes;++b) {
                    if((format.name=="X8_D24_UNORM" && b==3) || (format.name=="D32_FLOAT_S8_UINT" && b>=5)) continue;
                    const auto position=19+layer*pitch*rows+y*pitch+x*bytes+b;
                    assert(map.mapped_span[position]==original[position]);
                }
        }
        if(format.name=="BC1_RGBA_UNORM") {
            for(unsigned layer=0;layer<2;++layer) {
                alignas(16) std::array<u8,16> block{};
                alignas(16) std::array<u8,64> expected{};
                std::memcpy(block.data(),original.data()+19+layer*pitch*rows+bytes,bytes);
                bcn::DecodeBc1(block.data(),expected.data(),0,0,4,4);
                for(unsigned y=0;y<3;++y)
                    assert(std::memcmp(image.resource->planes[layer].data()+(y*5+4)*4,expected.data()+y*16,4)==0);
            }
        }
        const auto before=warnings;
        copy.buffer_offset=map.mapped_span.size();
        assert(!image.TransferConverted(map,std::span{&copy,1},false));
        assert(warnings==before+1);
    }
}
"""


ALIAS_SUPPORT = r"""
namespace fmt {
template<class... Args> int format(const char*, Args&&...) { return 0; }
}
void Diagnostic(int) {}
void Image::UploadMemory(const StagingBufferRef& map,std::span<const VideoCommon::BufferImageCopy> copies) {
    if(host_format.conversion!=FormatConversion::None) { assert(TransferConverted(map,copies,false)); return; }
    const auto bytes=VideoCore::Surface::BytesPerBlock(info.format);
    const auto block=VideoCore::Surface::DefaultBlockWidth(info.format);
    for(const auto& copy:copies) for(s32 layer=0;layer<copy.image_subresource.num_layers;++layer)
        for(u32 y=0;y<(copy.image_extent.height+block-1)/block;++y) {
            auto* destination=resource->planes.at(copy.image_subresource.base_layer+layer).data()+
                ((copy.image_offset.y/block+y)*((info.size.width+block-1)/block)+copy.image_offset.x/block)*bytes;
            const auto* source=map.mapped_span.data()+copy.buffer_offset+
                (u64{static_cast<u32>(layer)}*((copy.buffer_image_height+block-1)/block)+y)*((copy.buffer_row_length+block-1)/block)*bytes;
            std::memcpy(destination,source,((copy.image_extent.width+block-1)/block)*bytes);
        }
}
void Image::DownloadMemory(const StagingBufferRef& map,std::span<const VideoCommon::BufferImageCopy> copies) {
    if(host_format.conversion!=FormatConversion::None) { assert(TransferConverted(map,copies,true)); return; }
    const auto bytes=VideoCore::Surface::BytesPerBlock(info.format);
    const auto block=VideoCore::Surface::DefaultBlockWidth(info.format);
    for(const auto& copy:copies) for(s32 layer=0;layer<copy.image_subresource.num_layers;++layer)
        for(u32 y=0;y<(copy.image_extent.height+block-1)/block;++y) {
            const auto* source=resource->planes.at(copy.image_subresource.base_layer+layer).data()+
                ((copy.image_offset.y/block+y)*((info.size.width+block-1)/block)+copy.image_offset.x/block)*bytes;
            auto* destination=map.mapped_span.data()+copy.buffer_offset+
                (u64{static_cast<u32>(layer)}*((copy.buffer_image_height+block-1)/block)+y)*((copy.buffer_row_length+block-1)/block)*bytes;
            std::memcpy(destination,source,((copy.image_extent.width+block-1)/block)*bytes);
        }
}
"""
ALIAS_CHECK = r"""
        if(format.conversion==FormatConversion::PackedColor) {
            Image destination;
            destination.info=image.info;
            destination.info.format=bytes==1 ? "R8_UINT" : bytes==2 ? "R16_UINT" : "R32_UINT";
            destination.host_format=LookupFormat(destination.info.format);
            destination.runtime=&runtime;
            destination.resource=std::make_shared<Resource>();
            destination.resource->desc={5,3,destination.host_format.resource_format,2,1};
            destination.resource->planes.resize(2,std::vector<u8>(5*3*bytes));
            VideoCommon::ImageCopy region{{0,0,2},{0,0,2},{0,0,0},{0,0,0},{5,3,1}};
            runtime.ReinterpretImage(destination,image,std::span{&region,1});
            for(unsigned layer=0;layer<2;++layer) for(unsigned y=0;y<3;++y)
                assert(std::memcmp(destination.resource->planes[layer].data()+y*5*bytes,
                    original.data()+19+layer*pitch*rows+y*pitch,5*bytes)==0);
        }
"""


NATIVE_ALIAS_CHECK = r"""
    for(const auto pair : {std::pair{"R32_UINT","R16G16_UNORM"},
                           std::pair{"R16G16B16A16_UINT","R32G32_FLOAT"},
                           std::pair{"BC1_RGBA_UNORM","R16G16B16A16_UINT"},
                           std::pair{"R16G16B16A16_UINT","BC1_RGBA_UNORM"}}) {
        TextureCacheRuntime runtime;
        Image source, destination;
        source.info.format=pair.first; destination.info.format=pair.second;
        const auto src_block=VideoCore::Surface::DefaultBlockWidth(pair.first);
        const auto dst_block=VideoCore::Surface::DefaultBlockWidth(pair.second);
        source.info.size=src_block==4 || dst_block==1 ? VideoCommon::Extent3D{5,3,1} : VideoCommon::Extent3D{2,1,1};
        destination.info.size=dst_block==4 || src_block==1 ? VideoCommon::Extent3D{5,3,1} : VideoCommon::Extent3D{2,1,1};
        source.host_format=LookupFormat(pair.first); destination.host_format=LookupFormat(pair.second);
        source.runtime=destination.runtime=&runtime;
        source.resource=std::make_shared<Resource>(); destination.resource=std::make_shared<Resource>();
        source.resource->desc={5,3,source.host_format.resource_format,2,1};
        destination.resource->desc={5,3,destination.host_format.resource_format,2,1};
        source.resource->planes.resize(2,std::vector<u8>(((source.info.size.width+src_block-1)/src_block)*((source.info.size.height+src_block-1)/src_block)*source.host_format.bytes_per_block));
        for(unsigned layer=0;layer<2;++layer) for(size_t i=0;i<source.resource->planes[layer].size();++i)
            source.resource->planes[layer][i]=static_cast<u8>(i*37+layer*13);
        destination.resource->planes.resize(2,std::vector<u8>(((destination.info.size.width+dst_block-1)/dst_block)*((destination.info.size.height+dst_block-1)/dst_block)*destination.host_format.bytes_per_block));
        VideoCommon::ImageCopy region{{0,0,2},{0,0,2},{0,0,0},{0,0,0},source.info.size};
        runtime.CopyImage(destination,source,std::span{&region,1});
        assert(source.resource->planes==destination.resource->planes);
    }
"""


class D3D12FormatCoverageTests(unittest.TestCase):
    def test_every_pixel_format_has_host_storage(self):
        surface = (ROOT / "src/video_core/surface.h").read_text()
        names = re.findall(r"PIXEL_FORMAT_ELEM\((\w+),\s*\d+,", surface)
        rows = re.findall(r'\{"(\w+)",\s*(\d+),\s*(\d+)', HEADER.read_text())
        self.assertEqual(len(names), len(rows))
        self.assertEqual(set(names), {row[0] for row in rows})
        self.assertEqual(
            [], [name for name, resource, view in rows if int(resource) == 0 or int(view) == 0]
        )

    def test_conversions_and_decoder_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-t1-formats-") as temp:
            source = Path(temp) / "formats.cpp"
            binary = Path(temp) / "formats"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_plane_selection_and_upload_dispatch(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        for token in [
            "TransferConverted(input, copies, download)",
            "DXGI_FORMAT_X24_TYPELESS_G8_UINT",
            "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT",
            "view_plane",
            "ConvertDepthRow",
            "DecodeEtcBlock",
            "ConvertPackedRow",
            "plane * static_cast<UINT>",
        ]:
            self.assertIn(token, source)

    def compile_block_decoder(self, directory):
        output = Path(directory) / "bc_decoder.o"
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-w",
                "-fsanitize=address",
                "-c",
                str(ROOT / "externals/bc_decoder/bc_decoder.cpp"),
                "-o",
                str(output),
            ],
            check=True,
        )
        return output

    def test_runtime_conversion_transfers(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        start = source.index("bool Image::TransferConverted(")
        end = source.index("ImageView::ImageView(", start)
        with tempfile.TemporaryDirectory(prefix="nxbox-t1-runtime-") as temp:
            cpp = Path(temp) / "runtime.cpp"
            binary = Path(temp) / "runtime"
            cpp.write_text(RUNTIME_DRIVER.replace("__CONVERTED__", source[start:end]))
            decoder = self.compile_block_decoder(temp)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all",
                    "-I",
                    str(ROOT / "src"),
                    "-I",
                    str(ROOT / "externals/bc_decoder"),
                    str(cpp),
                    str(decoder),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_emulated_reinterpretation_preserves_guest_bits(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_texture_cache.cpp").read_text()
        start = source.index("bool Image::TransferConverted(")
        end = source.index("ImageView::ImageView(", start)
        alias_start = source.index("bool TextureCacheRuntime::ShouldReinterpret(")
        alias_end = source.index("void TextureCacheRuntime::TickFrame(", alias_start)
        driver = RUNTIME_DRIVER.replace(
            "__CONVERTED__", source[start:end] + ALIAS_SUPPORT + source[alias_start:alias_end]
        )
        driver = driver.replace(
            "#include <vector>",
            '#include <vector>\n#include <stdexcept>\n#include "video_core/renderer_d3d12/d3d12_footprint.h"',
        )
        driver = driver.replace(
            "struct BufferImageCopy {",
            "using SubresourceLayers=Subresource;\nstruct ImageCopy { Subresource src_subresource,dst_subresource; Offset3D src_offset,dst_offset; Extent3D extent; };\nstruct BufferImageCopy {",
        )
        driver = driver.replace("struct ImageInfo {", "struct ImageInfo { u32 num_samples{1};")
        driver = driver.replace(
            "struct TextureCacheRuntime {",
            "struct Image;\nstruct BlitSurface { ResourcePtr resource; FormatInfo format; };\nstruct FakeBlit { void CopyImage(const BlitSurface&,const BlitSurface&,std::span<const VideoCommon::ImageCopy>) { assert(false); } };\nstruct TextureCacheRuntime {\n    bool CanDownloadImage(const Image&) const { return true; }\n    bool ReinterpretDepthOnGpu(Image&,Image&,std::span<const VideoCommon::ImageCopy>) { return false; }\n    bool ReinterpretOnGpu(Image&,Image&,std::span<const VideoCommon::ImageCopy>) { return false; }\n    bool ShouldReinterpret(Image&,Image&);\n    void ReinterpretImage(Image&,Image&,std::span<const VideoCommon::ImageCopy>);\n    void CopyImage(Image&,Image&,std::span<const VideoCommon::ImageCopy>);\n    void CopyImageMSAA(Image&,Image&,std::span<const VideoCommon::ImageCopy>) { assert(false); }\n    FakeBlit blit; FakeBlit& BlitHelpers() { return blit; }",
        )
        driver = driver.replace(
            "struct Image {",
            "struct Image {\n    ResourcePtr Handle() const { return resource; }\n    FormatInfo HostFormat() const { return host_format; }\n    void UploadMemory(const StagingBufferRef&,std::span<const VideoCommon::BufferImageCopy>);\n    void DownloadMemory(const StagingBufferRef&,std::span<const VideoCommon::BufferImageCopy>);",
        )
        driver = driver.replace(
            "UINT16 DepthOrArraySize{},MipLevels{};",
            "UINT16 DepthOrArraySize{},MipLevels{}; struct { UINT Count{1}; } SampleDesc{};",
        )
        driver = driver.replace("u64 buffer_offset{};", "u64 buffer_size{},buffer_offset{};")
        # Preserve the aggregate initializer's buffer_offset position in the original fixture.
        driver = driver.replace("copy{19,8,", "copy{0,19,8,")
        driver = driver.replace(
            "        const auto before=warnings;",
            ALIAS_CHECK + "\n        const auto before=warnings;",
        )
        driver = driver.rsplit("\n}", 1)[0] + NATIVE_ALIAS_CHECK + "\n}\n"
        with tempfile.TemporaryDirectory(prefix="nxbox-t1-alias-") as temp:
            cpp = Path(temp) / "alias.cpp"
            binary = Path(temp) / "alias"
            cpp.write_text(driver)
            decoder = self.compile_block_decoder(temp)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all",
                    "-I",
                    str(ROOT / "src"),
                    "-I",
                    str(ROOT / "externals/bc_decoder"),
                    str(cpp),
                    str(decoder),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=60)
