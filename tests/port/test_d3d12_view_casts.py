# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production resource casting and alias selection with counted host doubles."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_texture_cache_fastpath import function

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


def method(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


RESOURCE_DRIVER = r"""
#include <cassert>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_formats.h"
#include "video_core/renderer_d3d12/d3d12_format_cast.h"
using namespace D3D12;
using u8 = unsigned char; using u32 = unsigned; using u64 = std::uint64_t;
using UINT32 = unsigned; using SIZE_T = size_t; using DXGI_FORMAT = unsigned;
using HRESULT = int; using D3D12_HEAP_TYPE = unsigned; using D3D12_RESOURCE_STATES = unsigned;
inline constexpr int E_INVALIDARG = -1, DXGI_ERROR_UNSUPPORTED = -2, E_OUTOFMEMORY = -3;
constexpr bool FAILED(int r) { return r < 0; }
constexpr bool SUCCEEDED(int r) { return r >= 0; }
inline constexpr unsigned D3D12_HEAP_TYPE_DEFAULT=0, D3D12_HEAP_TYPE_READBACK=2;
inline constexpr unsigned D3D12_RESOURCE_DIMENSION_BUFFER=1, D3D12_RESOURCE_DIMENSION_TEXTURE1D=2;
inline constexpr unsigned D3D12_RESOURCE_DIMENSION_TEXTURE3D=4;
inline constexpr unsigned D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET=1;
inline constexpr unsigned D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL=2;
inline constexpr unsigned D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS=4;
inline constexpr unsigned D3D12_FORMAT_SUPPORT1_TEXTURE1D=1, D3D12_FORMAT_SUPPORT1_TEXTURE2D=2;
inline constexpr unsigned D3D12_FORMAT_SUPPORT1_TEXTURE3D=4, D3D12_FORMAT_SUPPORT1_RENDER_TARGET=8;
inline constexpr unsigned D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE=1;
inline constexpr unsigned D3D12_FEATURE_FORMAT_SUPPORT=0, D3D12_HEAP_FLAG_NONE=0;
inline constexpr unsigned D3D12_RESOURCE_STATE_COMMON=0, D3D12_BARRIER_LAYOUT_COMMON=0;
inline constexpr unsigned D3D12_RESOURCE_STATE_COPY_DEST=1, D3D12_BARRIER_LAYOUT_COPY_DEST=1;
inline constexpr unsigned D3D12_RESOURCE_STATE_COPY_SOURCE=2, D3D12_BARRIER_LAYOUT_COPY_SOURCE=2;
inline constexpr unsigned D3D12_RESOURCE_STATE_RENDER_TARGET=3, D3D12_BARRIER_LAYOUT_RENDER_TARGET=3;
inline constexpr unsigned D3D12_BARRIER_LAYOUT_UNDEFINED=99;
struct D3D12_RESOURCE_DESC {
    unsigned Dimension=3, Alignment=0; u64 Width=120;
    unsigned Height=68, DepthOrArraySize=1, MipLevels=1, Format=9;
    struct { unsigned Count=1, Quality=0; } SampleDesc;
    unsigned Layout=0, Flags=5;
};
using D3D12_RESOURCE_DESC1 = D3D12_RESOURCE_DESC;
struct D3D12_FEATURE_DATA_FORMAT_SUPPORT { unsigned Format, Support1=0, Support2=0; };
struct D3D12_HEAP_PROPERTIES { unsigned Type, CreationNodeMask, VisibleNodeMask; };
struct D3D12_RANGE { size_t Begin, End; };
struct NativeResource { int Map(unsigned, D3D12_RANGE*, void**) { assert(false); return 0; } };
template <class T> struct Ptr {
    std::shared_ptr<T> value;
    T* Get() const { return value.get(); }
    T* operator->() const { return Get(); }
    explicit operator bool() const { return bool(value); }
    void Reset() { value.reset(); }
};
#define IID_PPV_ARGS(p) p
struct Budget { u64 used{}; };
struct Resource {
    Ptr<NativeResource> handle;
    std::vector<u32> castable_formats;
    unsigned heap{}, state{}; u64 allocation_size{};
    std::shared_ptr<Budget> budget; std::span<u8> mapped;
};
using ResourcePtr = std::shared_ptr<Resource>;
struct Device {
    int modern=0, legacy=0, fail=0, removed=0;
    unsigned last_layout=99;
    unsigned unsupported=0;
    std::vector<unsigned> received;
    struct Allocation { u64 SizeInBytes=65536; };
    Allocation GetResourceAllocationInfo(unsigned,unsigned,const D3D12_RESOURCE_DESC*) { return {}; }
    int GetDeviceRemovedReason() { return removed; }
    int CheckFeatureSupport(unsigned, D3D12_FEATURE_DATA_FORMAT_SUPPORT* s, size_t) {
        s->Support1 = s->Format == unsupported ? 0 : 15;
        s->Support2 = s->Format == 29 ? 0 : 1; return 0;
    }
    int CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, unsigned,
        const D3D12_RESOURCE_DESC*, unsigned, void*, Ptr<NativeResource>* out) {
        ++legacy; out->value=std::make_shared<NativeResource>(); return 0;
    }
    int CreateCommittedResource3(const D3D12_HEAP_PROPERTIES*, unsigned,
        const D3D12_RESOURCE_DESC1* d, unsigned layout, void*, void*, unsigned count,
        const unsigned* formats, Ptr<NativeResource>* out) {
        ++modern;
        assert((d->Format == 9 || d->Format == 27) && d->Width == 120 && d->Height == 68 && d->Flags == 5);
        last_layout=layout;
        received.assign(formats,formats+count);
        if (fail) return std::exchange(fail,0);
        out->value=std::make_shared<NativeResource>(); return 0;
    }
};
namespace FrameCount { constexpr unsigned ForcedSubmitMemoryPressure=1, ForcedSubmitAllocationRetry=2; }
u64 NativeBufferMemoryTarget(u64) { return UINT64_MAX; }
struct Pool { void Trim() {} } pool;
Pool* BufferPool(std::shared_ptr<Budget>) { return &pool; }
void Check(int r,const char*,Device*) { if (FAILED(r)) throw std::runtime_error("device error"); }
struct Context {
    Ptr<Device> device{std::make_shared<Device>()}, casting_device;
    std::shared_ptr<Budget> budget=std::make_shared<Budget>();
    void Collect() {} void Finish(unsigned) {}
    ResourcePtr CreateResource(const D3D12_RESOURCE_DESC&,D3D12_HEAP_TYPE,D3D12_RESOURCE_STATES);
};
// CREATE_RESOURCE
int main() {
    static_assert(CompatibleViewFormat(9,12));
    static_assert(!CompatibleViewFormat(9,17));
    static_assert(!CompatibleViewFormat(9,16));
    static_assert(!CompatibleViewFormat(9,9));
    static_assert(CompatibleViewFormat(44,46) && CompatibleViewFormat(44,47));
    static_assert(CompatibleViewFormat(19,20) && CompatibleViewFormat(19,22));
    static_assert(CompatibleViewFormat(90,87) && CompatibleViewFormat(90,91));
    for (const auto& f : FormatTable) {
        assert(CompatibleViewFormat(f.resource_format,f.view_format));
        if (f.depth) assert(CompatibleViewFormat(f.resource_format,f.DsvFormat()));
    }
    Context c;
    D3D12_RESOURCE_DESC desc;
    auto r=c.CreateResource(desc,0,0);
    assert(c.device->legacy==1 && r->castable_formats.empty());
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    c.casting_device=c.device;
    r=c.CreateResource(desc,0,0);
    assert(c.device->modern==1 && c.device->legacy==1);
    assert(CompatibleViewFormat(9,17,r->castable_formats));
    assert(CompatibleViewFormat(9,16,r->castable_formats));
    assert(!CompatibleViewFormat(9,42,r->castable_formats));
    for (auto f : r->castable_formats) {
        assert(std::count(r->castable_formats.begin(),r->castable_formats.end(),f)==1);
        const auto entry=std::ranges::find(FormatTable,f,&FormatInfo::view_format);
        assert(entry!=FormatTable.end() && entry->bytes_per_block==8 && !entry->depth);
    }
    c.device->unsupported=17;
    r=c.CreateResource(desc,0,0);
    assert(!CompatibleViewFormat(9,17,r->castable_formats));
    c.device->unsupported=0;
    c.device->fail=E_INVALIDARG;
    r=c.CreateResource(desc,0,0);
    assert(r->castable_formats.empty() && !CompatibleViewFormat(9,17,r->castable_formats));
    assert(c.device->legacy==2);
    c.device->fail=E_OUTOFMEMORY;
    const auto before=c.device->modern;
    r=c.CreateResource(desc,0,0);
    assert(c.device->modern==before+2 && CompatibleViewFormat(9,17,r->castable_formats));
    desc.SampleDesc.Count=4;
    r=c.CreateResource(desc,0,0);
    assert(c.device->modern==before+2 && r->castable_formats.empty());
    desc.SampleDesc.Count=1; desc.Format=27;
    r=c.CreateResource(desc,0,0);
    // sRGB remains a legal SRV even though it cannot be a typed UAV.
    assert(CompatibleViewFormat(27,29,r->castable_formats));
    desc.Format=9;
    r=c.CreateResource(desc,0,D3D12_RESOURCE_STATE_COPY_DEST);
    assert(c.device->last_layout==D3D12_BARRIER_LAYOUT_COPY_DEST);
    assert(CompatibleViewFormat(9,17,r->castable_formats));
    desc.Flags=2;
    r=c.CreateResource(desc,0,0);
    assert(c.device->modern==before+4 && r->castable_formats.empty());
#endif
}
"""

ALIAS_DRIVER = r"""
#include <cassert>
#include <optional>
#include <vector>
using ImageId=unsigned; using ImageViewId=unsigned; using PixelFormat=unsigned;
constexpr ImageViewId NULL_IMAGE_VIEW_ID=0;
unsigned BytesPerBlock(unsigned f) { return f==99 ? 4 : 8; }
unsigned DefaultBlockWidth(unsigned) { return 1; }
unsigned DefaultBlockHeight(unsigned) { return 1; }
struct Base { int level=0,layer=0; };
struct Range { Base base; };
struct ImageViewInfo {
    unsigned type, format; Range range;
    ImageViewInfo(unsigned t,unsigned f,Range r):type(t),format(f),range(r) {}
};
struct Info { unsigned format=1; };
struct Image {
    Info info; unsigned gpu_addr=4096;
    std::optional<Base> TryFindBase(unsigned a) { assert(a==4096); return Base{}; }
};
struct View {
    unsigned image_id=1,type=3; Range range{{2,3}};
    bool IsBuffer() const { return type==0; }
};
enum class RelaxedOptions { ForceBrokenViews };
template <class P> struct TextureCache {
    std::vector<Image> slot_images=std::vector<Image>(2);
    std::vector<View> slot_image_views=std::vector<View>(2);
    unsigned finds=0,prepared=0;
    ImageViewInfo last{0,0,{}};
    ImageId FindOrInsertImage(Info info,unsigned address,RelaxedOptions options) {
        assert(options==RelaxedOptions::ForceBrokenViews && info.format==2 && address==4096);
        ++finds;
        if (slot_images.size()==2) slot_images.push_back({info,address});
        // Force relocation to catch borrowed metadata surviving insertion.
        slot_image_views.reserve(slot_image_views.capacity()+100);
        slot_images.reserve(slot_images.capacity()+100);
        return 2;
    }
    ImageViewId FindOrEmplaceImageView(ImageId id,ImageViewInfo info) {
        assert(id==2); last=info; return 2;
    }
    void PrepareImageView(unsigned id,bool write,bool discard) {
        assert(id==2 && !write && !discard); ++prepared;
    }
    ImageViewId ReinterpretImageView(ImageViewId,PixelFormat);
};
// ALIAS_METHOD
int main() {
    TextureCache<int> c;
    assert(c.ReinterpretImageView(1,2)==2);
    assert(c.finds==1 && c.prepared==1);
    assert(c.last.type==3 && c.last.format==2);
    assert(c.last.range.base.level==2 && c.last.range.base.layer==3);
    assert(c.ReinterpretImageView(1,2)==2 && c.finds==2 && c.prepared==2);
    assert(c.ReinterpretImageView(1,99)==NULL_IMAGE_VIEW_ID && c.finds==2);
    c.slot_image_views[1].type=0;
    assert(c.ReinterpretImageView(1,2)==1 && c.finds==2);
    assert(c.ReinterpretImageView(0,2)==0 && c.finds==2);
}
"""


class ViewCastTests(unittest.TestCase):
    def run_cpp(self, source, definitions=()):
        with tempfile.TemporaryDirectory(prefix="nxbox-view-casts-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text("#include <utility>\n" + source)
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
                    *definitions,
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_creation_with_and_without_device10(self):
        source = (BACKEND / "d3d12_context.cpp").read_text()
        driver = RESOURCE_DRIVER.replace(
            "// CREATE_RESOURCE", method(source, "ResourcePtr Context::CreateResource(")
        )
        for definitions in ((), ("-D__ID3D12Device10_INTERFACE_DEFINED__",)):
            with self.subTest(definitions=definitions):
                self.run_cpp(driver, definitions)

    def test_alias_selection_preserves_subresources_and_prepares_contents(self):
        source = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        driver = ALIAS_DRIVER.replace(
            "// ALIAS_METHOD", function(source, "ImageViewId TextureCache<P>::ReinterpretImageView")
        )
        self.run_cpp(driver)

    def test_gpu_alias_round_trip_uses_matching_buffer_footprints(self):
        from test_d3d12_format_coverage import RUNTIME_DRIVER

        driver = RUNTIME_DRIVER.split("__CONVERTED__", 1)[0].replace('#include "bc_decoder.h"', "")
        driver = driver.replace(
            "struct BufferImageCopy {",
            "struct ImageCopy { Subresource src_subresource,dst_subresource; Offset3D src_offset,dst_offset; Extent3D extent; };\nstruct BufferImageCopy {",
        ).replace("struct ImageInfo {", "struct ImageInfo { unsigned num_samples=1;")
        driver = driver.replace("fp.Format==9 ? 8", "fp.Format==9 || fp.Format==15 ? 8")
        driver = driver.replace(
            "        const auto& fp=linear.PlacedFootprint.Footprint;",
            """        const auto& fp=linear.PlacedFootprint.Footprint;
        assert(target->Type==D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
               source->Type==D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        assert(fp.Format==texture.pResource->desc.Format);""",
        )
        driver = driver.replace(
            "struct Context {",
            """constexpr int D3D12_HEAP_TYPE_DEFAULT=0;
constexpr unsigned D3D12_TEXTURE_DATA_PITCH_ALIGNMENT=256;
constexpr unsigned D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT=512;
using ID3D12Resource=Resource;
namespace Common { template<class T> T AlignUp(T v,T a) { return (v+a-1)/a*a; } }
bool BisectEnabled(const char*) { return false; }
struct Context {
    ResourcePtr CreateBuffer(u64 size,int) {
        auto r=std::make_shared<Resource>(); r->bytes.resize(size); return r;
    }""",
        )
        driver = driver.replace(
            "struct TextureCacheRuntime {",
            "struct Image;\nstruct TextureCacheRuntime {\n    bool ReinterpretOnGpu(Image&,Image&,std::span<const VideoCommon::ImageCopy>);",
        ).replace(
            "struct Image {",
            """struct Image {
    ResourcePtr Handle() const { return resource; }
    FormatInfo HostFormat() const { return host_format; }""",
        )
        source = (BACKEND / "d3d12_texture_cache.cpp").read_text()
        driver += method(source, "bool TextureCacheRuntime::ReinterpretOnGpu(")
        driver += r"""
} // namespace D3D12
int main() {
    using namespace D3D12;
    TextureCacheRuntime runtime;
    Image source, target;
    source.info.format="R16G16B16A16_UNORM";
    target.info.format="R32G32_UINT";
    source.info.size=target.info.size={120,68,1};
    source.info.resources=target.info.resources={1,1};
    source.host_format=LookupFormat(source.info.format);
    target.host_format=LookupFormat(target.info.format);
    source.resource=std::make_shared<Resource>();
    target.resource=std::make_shared<Resource>();
    source.resource->desc={120,68,9,1,1};
    target.resource->desc={120,68,15,1,1};
    source.resource->planes.resize(1); target.resource->planes.resize(1);
    source.resource->planes[0].resize(120*68*8);
    target.resource->planes[0].resize(120*68*8);
    for (size_t i=0;i<source.resource->planes[0].size();++i)
        source.resource->planes[0][i]=static_cast<u8>(i*37);
    const VideoCommon::ImageCopy copy{{0,0,1},{0,0,1},{},{},{120,68,1}};
    assert(runtime.ReinterpretOnGpu(target,source,{&copy,1}));
    assert(target.resource->planes[0]==source.resource->planes[0]);
    // Model shader writes on the UINT alias and synchronize back to the original.
    for(auto& byte:target.resource->planes[0]) byte^=0xa5;
    assert(runtime.ReinterpretOnGpu(source,target,{&copy,1}));
    assert(source.resource->planes[0]==target.resource->planes[0]);
}
"""
        self.run_cpp(driver)

    def test_storage_override_precedes_preparation_and_all_explicit_views_are_guarded(self):
        cache = (ROOT / "src/video_core/texture_cache/texture_cache.h").read_text()
        fill = function(cache, "void TextureCache<P>::FillImageViews")
        self.assertLess(
            fill.index("ReinterpretImageView("),
            fill.index("if (blacklist) {", fill.index("VisitImageView(")),
        )
        rasterizer = (BACKEND / "d3d12_rasterizer.cpp").read_text()
        self.assertIn(".format_override = PixelFormatFromImageFormat(desc.format)", rasterizer)
        self.assertIn(".format_override = format", rasterizer)
        # Every explicit native texture view must validate before calling the runtime.
        import re

        for path in BACKEND.rglob("*.cpp"):
            source = path.read_text()
            for match in re.finditer(
                r"context.Device\(\)->Create(?:ShaderResource|UnorderedAccess|RenderTarget|DepthStencil)View\(",
                source,
            ):
                call = source[match.start() : source.index(";", match.start())]
                if "nullptr" in call and ("&null_desc" in call or ", nullptr," in call):
                    continue
                self.assertIn(
                    "ValidateViewFormat(",
                    source[max(0, match.start() - 250) : match.start()],
                    (path, call),
                )


if __name__ == "__main__":
    unittest.main()
