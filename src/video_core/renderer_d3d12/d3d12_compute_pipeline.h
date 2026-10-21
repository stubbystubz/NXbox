// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_d3d12/d3d12_disk_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_dxc.h"

namespace D3D12 {

class ComputePipeline {
public:
    ComputePipeline(ID3D12Device* device,
                    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature,
                    const DxilBlob& dxil, DiskPipelineCache* disk = nullptr, u64 key = 0,
                    std::span<const u8> recipe = {}, FrameProfile* profile = nullptr);

    ID3D12PipelineState* PipelineState() const {
        return pso.Get();
    }
    ID3D12RootSignature* RootSignature() const {
        return root_signature.Get();
    }

    bool UsesRootCbvs() const {
        return root_cbvs;
    }
    void SetRootCbvs(bool enabled) {
        root_cbvs = enabled;
    }

    Shader::Info info;

private:
    NxboxMemory::Gauge object_count{NxboxMemory::Kind::PipelineObjects, 1};
    bool root_cbvs{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};

} // namespace D3D12
