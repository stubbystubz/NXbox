// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>

#include <d3d12.h>
#include <wrl/client.h>

#include "shader_recompiler/backend/hlsl/hlsl_root_constants.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/stage.h"
#include "video_core/renderer_d3d12/d3d12_disk_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_dxc.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_iface.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"

namespace D3D12 {

struct StageProgram {
    Shader::Stage stage;
    DxilBlob dxil;
    u64 guest_hash{};
};

class GraphicsPipeline final : public IGraphicsPipeline {
public:
    // Throws on PSO creation failure. `programs` holds one entry per active stage.
    GraphicsPipeline(ID3D12Device* device,
                     Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature,
                     const Pso::GraphicsPsoState& state, u32 d3d_topology,
                     std::span<const StageProgram> programs, DiskPipelineCache* disk = nullptr,
                     u64 key = 0, std::span<const u8> recipe = {},
                     const D3D12_STREAM_OUTPUT_DESC& stream_output = {},
                     FrameProfile* profile = nullptr);

    // IGraphicsPipeline. A front-and-back cull draws nothing and D3D12 cannot express it, so
    // the descriptor/draw path sees "no PSO" and skips the draw.
    ID3D12PipelineState* PipelineState() const override {
        return CullsEverything() ? nullptr : pso.Get();
    }
    ID3D12RootSignature* RootSignature() const override {
        return root_signature.Get();
    }
    bool UsesRootCbvs() const override {
        return root_cbvs;
    }
    void SetRootCbvs(bool enabled) {
        root_cbvs = enabled;
    }

    D3D_PRIMITIVE_TOPOLOGY Topology() const override {
        return static_cast<D3D_PRIMITIVE_TOPOLOGY>(topology);
    }
    u32 ZeroVertexSlot() const override {
        for (const auto& attribute : state.attributes)
            if (attribute.enabled == 2)
                return attribute.slot;
        return 32;
    }
    const Shader::Info* StageInfo(size_t stage) const override {
        return stage_infos[stage] ? &*stage_infos[stage] : nullptr;
    }
    // The Info must outlive the pipeline (it lives with the translated program).
    void SetStageInfo(size_t stage, const Shader::Info* info) {
        stage_infos[stage] = *info;
    }
    std::array<u32, 16> RootConstants(const DrawInputs& inputs) const override {
        Shader::Backend::HLSL::RootConstants constants;
        constants.render_area[0] = static_cast<float>(inputs.render_area_width);
        constants.render_area[1] = static_cast<float>(inputs.render_area_height);
        constants.y_direction = inputs.lower_left_origin ? -1.0f : 1.0f;
        std::array<u32, 16> words{};
        std::memcpy(words.data(), &constants, sizeof(words));
        return words;
    }
    void SetTransformFeedback(const Shader::RuntimeInfo& runtime) {
        transform_feedback = runtime;
    }
    const Shader::RuntimeInfo* TransformFeedbackInfo() const {
        return transform_feedback ? &*transform_feedback : nullptr;
    }
    const Pso::GraphicsPsoState& State() const {
        return state;
    }
    const std::string& DrawTracePso() const {
        return draw_trace_pso;
    }
    u64 VertexGuestHash() const {
        return vertex_guest_hash;
    }
    u64 PixelGuestHash() const {
        return pixel_guest_hash;
    }
    const GraphicsPipeline* AsNative() const override {
        return this;
    }
    // Cull mode FrontAndBack: D3D12 cannot express it, the rasterizer must skip the draw.
    bool CullsEverything() const {
        return (state.cull_all & 1) != 0;
    }

private:
    NxboxMemory::Gauge object_count{NxboxMemory::Kind::PipelineObjects, 1};
    std::string draw_trace_pso;
    u64 vertex_guest_hash{};
    u64 pixel_guest_hash{};
    bool root_cbvs{};
    Pso::GraphicsPsoState state;
    u32 topology{};
    std::optional<Shader::RuntimeInfo> transform_feedback;
    std::array<std::optional<Shader::Info>, NUM_GRAPHICS_STAGES> stage_infos{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};

} // namespace D3D12
