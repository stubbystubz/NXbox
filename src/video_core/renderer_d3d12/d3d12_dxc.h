// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/nxbox_memory.h"
#include "shader_recompiler/stage.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12 {

using DxilBlob = std::shared_ptr<const std::vector<u8>>;

// DXC target profile for a stage: vs_6_0, hs_6_0, ds_6_0, gs_6_0, ps_6_0, cs_6_0.
const wchar_t* StageTarget(Shader::Stage stage);

// HLSL -> DXIL through the packaged dxcompiler.dll (ShaderCompiler, entry "main"), memoized by
// a 64-bit hash of (target, source); the source is kept to rule out hash collisions.
class FrameProfile;
class DiskPipelineCache;
class DxcCache {
public:
    explicit DxcCache(FrameProfile* profile_ = nullptr) : profile{profile_} {}
    // Throws (via Check) on a compile/validation error; the DXC log goes to LOG_WARNING.
    DxilBlob Compile(std::string_view hlsl, Shader::Stage stage, u64 pipeline_hash = 0,
                     DiskPipelineCache* disk = nullptr);
    size_t Size() const {
        return entries.size();
    }

private:
    struct Entry {
        std::string source;
        const wchar_t* target;
        DxilBlob dxil;
        std::exception_ptr failure;
    };
    NxboxMemory::Gauge source_memory{NxboxMemory::Kind::ShaderSource};
    FrameProfile* profile{};
    ShaderCompiler compiler;
    std::unordered_multimap<u64, Entry> entries;
};

} // namespace D3D12
