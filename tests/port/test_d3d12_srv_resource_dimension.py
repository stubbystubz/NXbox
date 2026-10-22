# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the production SRV adaptation and binding paths against a recording device."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
using UINT = unsigned;
using u32 = unsigned;
using u64 = unsigned long long;
enum D3D12_SRV_DIMENSION {
    D3D12_SRV_DIMENSION_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER,
    D3D12_SRV_DIMENSION_TEXTURE1D, D3D12_SRV_DIMENSION_TEXTURE1DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
    D3D12_SRV_DIMENSION_TEXTURE2DMS, D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY,
    D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_SRV_DIMENSION_TEXTURECUBE,
    D3D12_SRV_DIMENSION_TEXTURECUBEARRAY
};
enum D3D12_RESOURCE_DIMENSION {
    D3D12_RESOURCE_DIMENSION_TEXTURE1D,
    D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    D3D12_RESOURCE_DIMENSION_TEXTURE3D
};
constexpr UINT DXGI_FORMAT_R8G8B8A8_UNORM = 28;
constexpr UINT D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING = 5768;
constexpr UINT D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV = 0;
struct D3D12_RESOURCE_DESC {
    D3D12_RESOURCE_DIMENSION Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    UINT Width = 32, Height = 32, DepthOrArraySize = 1, MipLevels = 4, Format = 28;
    struct { UINT Count = 1; } SampleDesc;
};
struct D3D12_SHADER_RESOURCE_VIEW_DESC {
    UINT Format{}, Shader4ComponentMapping{};
    D3D12_SRV_DIMENSION ViewDimension{};
    struct { UINT NumElements{}; } Buffer;
    struct { UINT MostDetailedMip{}, MipLevels{}; float ResourceMinLODClamp{}; }
        Texture1D, Texture3D, TextureCube;
    struct { UINT MostDetailedMip{}, MipLevels{}, FirstArraySlice{}, ArraySize{};
             float ResourceMinLODClamp{}; } Texture1DArray;
    struct { UINT MostDetailedMip{}, MipLevels{}, PlaneSlice{};
             float ResourceMinLODClamp{}; } Texture2D;
    struct { UINT MostDetailedMip{}, MipLevels{}, FirstArraySlice{}, ArraySize{}, PlaneSlice{};
             float ResourceMinLODClamp{}; } Texture2DArray;
    struct { UINT FirstArraySlice{}, ArraySize{}; } Texture2DMSArray;
    struct { UINT MostDetailedMip{}, MipLevels{}, First2DArrayFace{}, NumCubes{};
             float ResourceMinLODClamp{}; } TextureCubeArray;
};
struct Handle { D3D12_RESOURCE_DESC desc; auto GetDesc() const { return desc; } };
struct Resource { std::shared_ptr<Handle> handle = std::make_shared<Handle>(); };
using ResourcePtr = std::shared_ptr<Resource>;
struct ImageView {
    ResourcePtr resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc;
    u64 gpu_addr{}; int type{};
    auto ImageHandle() const { return resource; }
    auto Description() const { return desc; }
};
struct DescriptorTable {};
namespace fmt { template<class... T> std::string format(const char*, T...) { return {}; } }
int diagnostics{};
void Diagnostic(const std::string&) { ++diagnostics; }
bool BisectEnabled(const char*) { return false; }
struct D3D12DescriptorAllocator {
    std::unordered_set<UINT> logged_srv_mismatches;
    ResourcePtr written_resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC written{};
    void WriteCached(UINT, ResourcePtr resource, UINT, const void* desc, size_t,
                     const DescriptorTable&, u32) {
        written_resource = resource;
        written = *static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(desc);
    }
    void WriteNullSrv(const DescriptorTable&, u32, D3D12_SRV_DIMENSION);
    bool WriteImageSrv(const DescriptorTable&, u32, const ImageView&,
                       D3D12_SRV_DIMENSION, const ResourcePtr& = {},
                       D3D12_SHADER_RESOURCE_VIEW_DESC* = nullptr);
};
__PRODUCTION__
int main() {
    D3D12DescriptorAllocator allocator;
    ImageView view{std::make_shared<Resource>(), {}};
    auto& resource = view.resource->handle->desc;
    view.desc.Format = 28;
    view.desc.Shader4ComponentMapping = 1234;
    view.desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.desc.Texture2D = {1, 2, 0, 0};
    resource.DepthOrArraySize = 6;
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBE));
    assert(allocator.written_resource == view.resource);
    assert(allocator.written.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE);
    assert(allocator.written.TextureCube.MostDetailedMip == 1);
    assert(allocator.written.TextureCube.MipLevels == 2);
    assert(allocator.written.Shader4ComponentMapping == 1234);
    // Diagnostic output is the actual adapted binding, including widened cube faces.
    D3D12_SHADER_RESOURCE_VIEW_DESC resolved{};
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBE, {}, &resolved));
    assert(resolved.ViewDimension == allocator.written.ViewDimension);
    assert(resolved.TextureCube.MostDetailedMip == allocator.written.TextureCube.MostDetailedMip);
    assert(resolved.Shader4ComponentMapping == allocator.written.Shader4ComponentMapping);
    resource.DepthOrArraySize = 12;
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBEARRAY));
    assert(allocator.written.TextureCubeArray.NumCubes == 2);
    view.desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    resource.DepthOrArraySize = 6;
    view.desc.Texture2DArray = {1, 2, 0, 6, 0, 0};
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBE));
    resource.DepthOrArraySize = 12;
    view.desc.Texture2DArray.ArraySize = 12;
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBEARRAY));
    assert(allocator.written.TextureCubeArray.NumCubes == 2);
    // The cache also represents a plain 2D TIC on a multi-layer resource as an array
    // SRV exposing a single slice. The shader still needs all cube faces.
    view.desc.Texture2DArray.ArraySize = 1;
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBEARRAY));
    assert(allocator.written.TextureCubeArray.NumCubes == 2);
    resource.DepthOrArraySize = 6;
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBE));
    resource.DepthOrArraySize = 12;
    // Explicit array views preserve their face range and mip selection.
    view.desc.Texture2DArray = {1, 2, 6, 6, 0, 0};
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBEARRAY));
    assert(allocator.written.TextureCubeArray.First2DArrayFace == 6);
    assert(allocator.written.TextureCubeArray.NumCubes == 1);
    // A cube SRV cannot start at face 6: the unadapted array view stays bound, as before.
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBE));
    assert(allocator.written.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBE, {}, &resolved));
    assert(resolved.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
    assert(resolved.Texture2DArray.FirstArraySlice == 6);
    view.desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURE3D));
    assert(allocator.written.Texture3D.MostDetailedMip == 1);
    assert(allocator.written.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D);
    view.desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    view.desc.Texture3D = {2, 1, 0};
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURE3D));
    assert(allocator.written.Texture3D.MostDetailedMip == 2);
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.DepthOrArraySize = 1;
    view.desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    const int before_impossible = diagnostics;
    // A single-layer 2D texture has no cube faces: the cube slot gets a null SRV of its own
    // dimension (the mismatched view removed the device in Mario Party Superstars).
    for (auto wanted : {D3D12_SRV_DIMENSION_TEXTURECUBE,
                        D3D12_SRV_DIMENSION_TEXTURECUBEARRAY}) {
        for (int repeat = 0; repeat < 2; ++repeat) {
            assert(!allocator.WriteImageSrv({}, 0, view, wanted));
            assert(!allocator.written_resource);
            assert(allocator.written.ViewDimension == wanted);
        }
    }
    // A 2D resource cannot back a 3D slot either: null SRV (Mario Party removed the device).
    for (int repeat = 0; repeat < 2; ++repeat) {
        assert(!allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURE3D));
        assert(!allocator.written_resource);
        assert(allocator.written.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D);
    }
    // One diagnostic per dimension pair.
    assert(diagnostics == before_impossible + 3);
    resource.DepthOrArraySize = 6;
    resource.SampleDesc.Count = 4;
    assert(allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBE));
    assert(allocator.written.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D);
    // A feedback replacement must be checked, rather than the original resource: the original
    // has 6 layers and could back a cube, the single-layer replacement cannot.
    resource.SampleDesc.Count = 1;
    auto replacement = std::make_shared<Resource>();
    assert(!allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBE, replacement));
    assert(!allocator.written_resource);
    assert(allocator.written.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE);
    // A 3D replacement cannot back a cube slot either, while the 2D original could.
    replacement->handle->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    assert(!allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURECUBE, replacement));
    assert(!allocator.written_resource);
    assert(allocator.written.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE);
    view.resource.reset();
    assert(!allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURE3D));
    assert(!allocator.written_resource);
    assert(allocator.written.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D);
    assert(!allocator.WriteImageSrv({}, 0, view, D3D12_SRV_DIMENSION_TEXTURE3D, {}, &resolved));
    assert(resolved.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D);
    assert(resolved.Format == 0); // Null: never report stale successful view fields.
}
"""


class SrvResourceDimension(unittest.TestCase):
    def test_resource_views_and_null_binding(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_descriptor.cpp").read_text()
        adaptation = source[source.index("namespace {\n\nstruct SrvRange") :]
        adaptation = adaptation[: adaptation.index("\nnamespace {\nD3D12_TEXTURE_ADDRESS_MODE")]

        def function(signature):
            body = source[source.index(signature) :]
            return body[: body.index("\n}\n") + 3]

        production = adaptation + function("void D3D12DescriptorAllocator::WriteNullSrv")
        production += function("bool D3D12DescriptorAllocator::WriteImageSrv")
        with tempfile.TemporaryDirectory(prefix="nxbox-srv-dimension-") as directory:
            cpp = Path(directory) / "srv.cpp"
            binary = Path(directory) / "srv"
            cpp.write_text(DRIVER.replace("__PRODUCTION__", production))
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)
