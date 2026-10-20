// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>
#include <fmt/format.h>
#include "video_core/renderer_d3d12/d3d12_build_stats.h"
#include "video_core/shader_discovery.h"
#ifdef NXBOX_UWP
#include <winrt/Windows.System.h>
#endif

#include "video_core/renderer_d3d12/d3d12_compile_policy.h"

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/program_header.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_hlsl_dump.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_pso_diagnostics.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/d3d12_transform_feedback.h"
#include "video_core/submission_trace.h"
#include "video_core/surface.h"
#include "video_core/transform_feedback.h"

namespace D3D12 {
namespace {
bool PipelineLookupFastpathEnabled() {
    static const bool enabled = [] {
        char value[2]{};
        return !(GetEnvironmentVariableA("NXBOX_D3D12_PIPELINE_LOOKUP_FASTPATH", value,
                                         sizeof(value)) == 1 &&
                 value[0] == '0');
    }();
    return enabled;
}

// On by default: fewer cold-cache hitches in Odyssey and MK8D on Series X. =0 waits as before.
bool RtHistoryEnabled() {
    static const bool enabled = [] {
        char value[2]{};
        return !(GetEnvironmentVariableA("NXBOX_D3D12_PSO_WAIT_RT_HISTORY", value, sizeof(value)) ==
                     1 &&
                 value[0] == '0');
    }();
    return enabled;
}

// No hashing, allocation, locking or file IO unless explicitly enabled.
void DumpGraphicsHlsl(u64 guest_hash, Shader::Stage stage, std::string_view source) {
    static const bool enabled = BisectEnabled("NXBOX_D3D12_DUMP_HLSL");
    if (!enabled)
        return;
    static HlslDump dump;
    try {
        dump.Write(Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir), guest_hash,
                   static_cast<int>(stage), source);
    } catch (const std::exception& error) {
        Diagnostic(
            fmt::format("D3D12 HLSL dump guest={:016x} failed={}", guest_hash, error.what()));
    }
}

// Diagnostic for array-texture sampling in pixel shaders.
// NXBOX_D3D12_TEX_PROBE_LOAD=1 replaces tex0.Sample with a nearest Load at mip 0 (bypasses the
// sampler); NXBOX_D3D12_TEX_PROBE_LAYER=<n> forces the array layer. Both only touch shaders
// that sample a single Texture2DArray tex0.
void ApplyTextureProbe(Shader::Stage stage, std::string& source) {
    static const char* const load_env = std::getenv("NXBOX_D3D12_TEX_PROBE_LOAD");
    static const char* const layer_env = std::getenv("NXBOX_D3D12_TEX_PROBE_LAYER");
    if ((!load_env && !layer_env) || stage != Shader::Stage::Fragment)
        return;
    const std::string anchor = "SamplerState samp0 : register(s0, space0);\n";
    const auto at = source.find(anchor);
    if (at == std::string::npos || source.find("Texture2DArray<float4> tex0 :") == std::string::npos)
        return;
    std::string helper = "float4 nx_probe_tex0(float3 c) {\n";
    if (layer_env)
        helper += fmt::format("    c.z = {}.0;\n", std::strtol(layer_env, nullptr, 10));
    if (load_env) {
        helper += "    uint w, h, e, l;\n    tex0.GetDimensions(0, w, h, e, l);\n"
                  "    return tex0.Load(int4(int2(c.xy * float2(w, h)), int(c.z + 0.5), 0));\n}\n";
    } else {
        helper += "    return tex0.Sample(samp0, c);\n}\n";
    }
    source.insert(at + anchor.size(), helper);
    static const std::regex sample{R"(tex0\.Sample\(samp0, (v\d+)\))"};
    const auto head = source.substr(0, at + anchor.size() + helper.size());
    source = head + std::regex_replace(source.substr(head.size()), sample, "nx_probe_tex0($1)");
}

// How long a draw waits for its PSO the first time it is requested (NXBOX_D3D12_PSO_WAIT_MS).
std::chrono::milliseconds PsoWaitBudget() {
    static const auto budget = [] {
        const char* value = std::getenv("NXBOX_D3D12_PSO_WAIT_MS");
        return std::chrono::milliseconds{value ? std::strtol(value, nullptr, 10) : 5000};
    }();
    return budget;
}

bool NonBlockingPsoEnabled() {
    // Explicit opt-in: small draws and dispatches can bake persistent textures only once.
    // Keep the established compatibility policy until full-async rendering is verified per game.
    const char* value = std::getenv("NXBOX_D3D12_ASYNC_PSO");
    return value && std::string_view{value} == "1";
}

bool FrameBoundedPsoEnabled() {
    // Also moves compute translation off the GPU thread in the explicit async mode.
    return NonBlockingPsoEnabled() || BisectEnabled("NXBOX_D3D12_PSO_FRAME_BUDGET");
}

size_t RuntimePsoThreads() {
    // Each worker owns its DXC instance/cache. Keep the existing two-worker memory footprint
    // unless the third parallel compiler is explicitly requested.
    static const size_t count =
        RuntimePsoThreadCount(BisectEnabled("NXBOX_D3D12_PSO_PARALLEL_DXC"));
    return count;
}

// Shared by foreground requests and the disk warmup coordinator. `pool_idle` admits the compile
// anyway: memory held elsewhere can keep the headroom below the threshold for good, and deferring
// every key would then skip its draws forever. One compile at a time still makes progress.
bool CompileHeadroomAvailable(bool pool_idle) {
#ifdef NXBOX_UWP
    static std::mutex mutex;
    static CompileHeadroomGuard guard;
    std::scoped_lock lock{mutex};
    const auto now = CompileHeadroomGuard::Clock::now();
    const bool allowed = guard.Allows(CompileHeadroomBytes(), now, [] {
        using winrt::Windows::System::MemoryManager;
        return std::pair{MemoryManager::AppMemoryUsageLimit(), MemoryManager::AppMemoryUsage()};
    });
    if (allowed || pool_idle)
        return true;
    if (guard.ShouldLog(now))
        Diagnostic(
            fmt::format("D3D12 compile deferred: headroom={}", guard.Headroom() / (1024 * 1024)));
    return false;
#else
    return true;
#endif
}

void ReportGlobalFallbackSkip(u64 request) {
    // Disk warmup and foreground requests can encounter the same key.
    static std::mutex mutex;
    static std::set<u64> reported;
    std::scoped_lock lock{mutex};
    if (reported.insert(request).second)
        Diagnostic(
            fmt::format("D3D12 compute request={:016x} skipped: global memory fallback", request));
}

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

void ReportUnsupported(std::string_view reason) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::scoped_lock lock{mutex};
    if (reported.emplace(reason).second) {
        Diagnostic(fmt::format("D3D12_STATE_FALLBACK reason={} action=skip_draw", reason));
        LOG_WARNING(Render, "D3D12 state fallback: {}", reason);
    }
}

// A rejected state must not produce a valid-looking PSO with different semantics.
// These cases need shader linkage/emulation beyond the fixed-function D3D12 API.
const char* UnsupportedState(const Maxwell& regs, u32 topology) {
    if (topology > 14)
        return "invalid Maxwell PrimitiveTopology";
    if (Pso::CullMode(regs.gl_cull_test_enabled != 0, static_cast<u32>(regs.gl_cull_face))
            .cull_all &&
        !regs.transform_feedback_enabled &&
        (Pso::TopologyType(topology) == Pso::kTopologyTypeTriangle ||
         Pso::TopologyType(topology) == Pso::kTopologyTypePatch))
        return "gl_cull_face=FrontAndBack requires rasterizer discard preserving vertex/query "
               "effects";
    if (!regs.rasterize_enable && !regs.transform_feedback_enabled)
        return "rasterize_enable=0 requires discard with vertex/query side effects";
    if (!regs.rasterize_enable && regs.transform_feedback_enabled &&
        std::none_of(regs.transform_feedback.controls.begin(),
                     regs.transform_feedback.controls.end(),
                     [](const auto& control) { return control.varying_count != 0; }))
        return "rasterize_enable=0 requires a non-empty stream-output layout";
    if (regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
        regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation))
        return "tessellation.params require HLSL hull/domain stage linkage";
    if (regs.transform_feedback_enabled) {
        for (const auto& control : regs.transform_feedback.controls)
            if (control.varying_count && control.stream != 0)
                return "transform_feedback/stream_out_layout require multi-stream geometry linkage";
    }
    if (regs.depth_bounds_enable &&
        (!std::isfinite(regs.depth_bounds[0]) || !std::isfinite(regs.depth_bounds[1]) ||
         regs.depth_bounds[0] > regs.depth_bounds[1]))
        return "depth_bounds_enable/depth_bounds invalid range";
    if (regs.anti_alias_alpha_control.alpha_to_one)
        return "anti_alias_alpha_control.alpha_to_one requires fragment epilogue";
    if (regs.iterated_blend.enable || regs.iterated_blend.enable_alpha)
        return "iterated_blend requires programmable blend emulation";
    if (regs.polygon_mode_front == Maxwell::PolygonMode::Line &&
        (topology == 7 || topology == 8 || topology == 9))
        return "polygon_mode_front=Line for quads/quad strips/polygons requires boundary-edge "
               "emulation";
    if (regs.polygon_mode_front == Maxwell::PolygonMode::Point)
        return "polygon_mode_front=Point requires point rasterization emulation";
    if (regs.polygon_mode_front != regs.polygon_mode_back)
        return "polygon_mode_back differs from polygon_mode_front; requires per-face polygon "
               "emulation";
    if (topology == 0 && regs.point_size != 1.0f)
        return "point_size requires expanded point geometry";
    if (Pso::TopologyType(topology) == Pso::kTopologyTypeLine) {
        if (regs.line_stipple_enable)
            return "line_stipple_enable/line_stipple_params require fragment stipple emulation";
        const float width =
            regs.line_anti_alias_enable ? regs.line_width_smooth : regs.line_width_aliased;
        if (width != 1.0f)
            return "line_width_smooth/line_width_aliased require expanded line geometry";
    }
    if (regs.viewport_scale_offset_enabled) {
        for (const auto& viewport : regs.viewport_transform) {
            if (viewport.swizzle.x != Maxwell::ViewportSwizzle::PositiveX ||
                (viewport.swizzle.y != Maxwell::ViewportSwizzle::PositiveY &&
                 viewport.swizzle.y != Maxwell::ViewportSwizzle::NegativeY) ||
                viewport.swizzle.z != Maxwell::ViewportSwizzle::PositiveZ ||
                viewport.swizzle.w != Maxwell::ViewportSwizzle::PositiveW || viewport.scale_x < 0)
                return "viewport_transform.swizzle requires general position swizzle emulation";
        }
    }
    if (!std::isfinite(regs.depth_bias) || !std::isfinite(regs.slope_scale_depth_bias) ||
        !std::isfinite(regs.depth_bias_clamp) ||
        static_cast<double>(regs.depth_bias) / 2.0 < std::numeric_limits<s32>::min() ||
        static_cast<double>(regs.depth_bias) / 2.0 > std::numeric_limits<s32>::max())
        return "depth_bias/slope_scale_depth_bias/depth_bias_clamp invalid or outside D3D12 range";
    return nullptr;
}

u8 StencilFaceMask(u32 value) {
    return static_cast<u8>(value & 0xFF);
}

Pso::StencilFaceState Face(const Maxwell::StencilOp& op) {
    return {static_cast<u8>(Pso::StencilOp(static_cast<u32>(op.fail))),
            static_cast<u8>(Pso::StencilOp(static_cast<u32>(op.zfail))),
            static_cast<u8>(Pso::StencilOp(static_cast<u32>(op.zpass))),
            static_cast<u8>(Pso::ComparisonFunc(static_cast<u32>(op.func)))};
}

template <typename Blend>
void SetBlend(Pso::BlendTargetState& out, const Blend& b) {
    out.enable = 1;
    out.src = static_cast<u8>(Pso::BlendFactor(static_cast<u32>(b.color_source)));
    out.dst = static_cast<u8>(Pso::BlendFactor(static_cast<u32>(b.color_dest)));
    out.op = static_cast<u8>(Pso::BlendOp(static_cast<u32>(b.color_op)));
    out.src_alpha = static_cast<u8>(Pso::AlphaBlendFactor(static_cast<u32>(b.alpha_source)));
    out.dst_alpha = static_cast<u8>(Pso::AlphaBlendFactor(static_cast<u32>(b.alpha_dest)));
    out.op_alpha = static_cast<u8>(Pso::BlendOp(static_cast<u32>(b.alpha_op)));
}

void Append32(DiskBytes& bytes, u32 value) {
    for (u32 i = 0; i < 4; ++i)
        bytes.push_back(static_cast<u8>(value >> (i * 8)));
}
void Append64(DiskBytes& bytes, u64 value) {
    Append32(bytes, static_cast<u32>(value));
    Append32(bytes, static_cast<u32>(value >> 32));
}
DiskBytes Recipe(const Pso::GraphicsPsoState* state, u32 topology,
                 std::span<const StageInput> stages) {
    DiskBytes bytes;
    Append32(bytes, 4);
    u32 cbv_count{};
    for (const auto& stage : stages)
        cbv_count += stage.counts.cbv;
    Append32(bytes, UseRootCbvs(cbv_count) ? 1u : 0u);
    Append32(bytes, topology);
    if (state) {
        const auto* data = reinterpret_cast<const u8*>(state);
        bytes.insert(bytes.end(), data, data + sizeof(*state));
    }
    Append32(bytes, static_cast<u32>(stages.size()));
    for (const auto& stage : stages) {
        Append32(bytes, static_cast<u32>(stage.stage));
        Append64(bytes, stage.hash);
        Append32(bytes, stage.counts.cbv);
        Append32(bytes, stage.counts.srv);
        Append32(bytes, stage.counts.uav);
        Append32(bytes, stage.counts.sampler);
        Append32(bytes, static_cast<u32>(stage.hlsl.size()));
        bytes.insert(bytes.end(), stage.hlsl.begin(), stage.hlsl.end());
    }
    return bytes;
}
size_t Occurrences(std::string_view text, std::string_view token) {
    size_t count{}, position{};
    while ((position = text.find(token, position)) != std::string_view::npos) {
        ++count;
        position += token.size();
    }
    return count;
}
void ReportSlowPso(u64 hash, std::chrono::steady_clock::time_point start, ID3D12RootSignature* root,
                   DescriptorCounts counts, std::span<const StageInput> sources,
                   std::span<const StageProgram> programs) {
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (elapsed < 1000 || !BisectEnabled("NXBOX_D3D12_PSO_DIAGNOSTICS"))
        return;
    static std::mutex mutex;
    static std::set<u64> reported;
    {
        std::scoped_lock lock{mutex};
        if (!reported.emplace(hash).second)
            return;
    }
    Diagnostic(fmt::format("D3D12_SLOW_PSO hash={:016x} native_wall_ms={:.3f} "
                           "root={} root_schema=10 cbv={} srv={} uav={} sampler={}",
                           hash, elapsed, fmt::ptr(root), counts.cbv, counts.srv, counts.uav,
                           counts.sampler));
    try {
        ShaderCompiler compiler;
        for (size_t i = 0; i < programs.size(); ++i) {
            const auto& program = programs[i];
            const auto assembly = compiler.Disassemble(*program.dxil);
            const auto source = sources[i].hlsl;
            const auto local = source.find("uint lmem[");
            const auto end = local == std::string_view::npos ? local : source.find(';', local);
            const auto local_array =
                local == std::string_view::npos
                    ? std::string_view{"none"}
                    : source.substr(local, end == std::string_view::npos ? 80 : end - local);
            Diagnostic(fmt::format(
                "D3D12_SLOW_PSO_STAGE hash={:016x} stage={} dxil_bytes={} "
                "hlsl_bytes={} llvm_instruction_count={} llvm_instruction_results={} llvm_calls={} "
                "llvm_alloca={} "
                "llvm_gep={} llvm_switch={} hlsl_unroll={} hlsl_loops={} "
                "texcoord_declarations={} local_array={} lmem_nonliteral_indices={} "
                "register_array={} register_nonliteral_indices={}",
                hash, static_cast<u32>(program.stage), program.dxil->size(), source.size(),
                CountLlvmInstructions(assembly), Occurrences(assembly, " = "),
                Occurrences(assembly, "call "), Occurrences(assembly, "alloca "),
                Occurrences(assembly, "getelementptr "), Occurrences(assembly, "switch "),
                Occurrences(source, "[unroll"), Occurrences(source, "while ("),
                Occurrences(source, ": TEXCOORD"), local_array,
                CountNonliteralIndices(source, "lmem["),
                source.find("nx_regs[255]") != std::string_view::npos,
                CountNonliteralIndices(source, "nx_regs[")));
        }
    } catch (const std::exception& error) {
        Diagnostic(
            fmt::format("D3D12_SLOW_PSO hash={:016x} analysis_failed={}", hash, error.what()));
    }
}
class RecipeReader {
public:
    explicit RecipeReader(std::span<const u8> data_) : data{data_} {}
    std::span<const u8> Read(size_t size) {
        if (size > data.size())
            throw std::runtime_error("D3D12 truncated PSO recipe");
        const auto result = data.first(size);
        data = data.subspan(size);
        return result;
    }
    u32 U32() {
        const auto bytes = Read(4);
        return u32{bytes[0]} | (u32{bytes[1]} << 8) | (u32{bytes[2]} << 16) | (u32{bytes[3]} << 24);
    }
    u64 U64() {
        const u64 low = U32();
        return low | (u64{U32()} << 32);
    }
    bool Empty() const {
        return data.empty();
    }

private:
    std::span<const u8> data;
};

struct DecodedRecipe {
    bool root_cbvs{};
    u32 topology{};
    Pso::GraphicsPsoState state{};
    std::vector<StageInput> stages;
};
DecodedRecipe ReadRecipe(const DiskPipelineRecord& record) {
    DecodedRecipe decoded;
    RecipeReader reader{record.payload};
    if (reader.U32() != 4)
        throw std::runtime_error("Invalid D3D12 PSO recipe");
    decoded.root_cbvs = reader.U32() != 0;
    decoded.topology = reader.U32();
    auto& state = decoded.state;
    if (record.kind == DiskRecordKind::Graphics) {
        const auto raw = reader.Read(sizeof(state));
        std::memcpy(&state, raw.data(), sizeof(state));
    } else if (record.kind != DiskRecordKind::Compute)
        throw std::runtime_error("Invalid D3D12 PSO recipe");
    const u32 count = reader.U32();
    if (!count || count > 6)
        throw std::runtime_error("Invalid D3D12 PSO recipe");
    auto& stages = decoded.stages;
    for (u32 i = 0; i < count; ++i) {
        const auto stage = static_cast<Shader::Stage>(reader.U32());
        if (static_cast<u32>(stage) > static_cast<u32>(Shader::Stage::Compute))
            throw std::runtime_error("Invalid D3D12 PSO recipe");
        const u64 hash = reader.U64();
        DescriptorCounts counts{reader.U32(), reader.U32(), reader.U32(), reader.U32()};
        if (counts.cbv > 1024 || counts.srv > 16384 || counts.uav > 16384 || counts.sampler > 2048)
            throw std::runtime_error("Invalid D3D12 PSO recipe");
        const u32 size = reader.U32();
        const auto source = reader.Read(size);
        stages.push_back(
            {stage, hash, {reinterpret_cast<const char*>(source.data()), source.size()}, counts});
    }
    u32 cbv_count{};
    for (const auto& stage : stages)
        cbv_count += stage.counts.cbv;
    if (decoded.root_cbvs != UseRootCbvs(cbv_count) || !reader.Empty())
        throw std::runtime_error("Invalid D3D12 PSO recipe");
    return decoded;
}

// Only binding/primitive metadata consumed by the D3D12 rasterizer is persisted.
// Keep this list and the schema version in sync when that consumer gains fields.
auto BindingFields(Shader::ConstantBufferDescriptor& value) {
    return std::tie(value.index, value.count);
}
auto BindingFields(Shader::StorageBufferDescriptor& value) {
    return std::tie(value.cbuf_index, value.cbuf_offset, value.count, value.is_written);
}
auto BindingFields(Shader::TextureBufferDescriptor& value) {
    return std::tie(value.has_secondary, value.cbuf_index, value.cbuf_offset, value.shift_left,
                    value.secondary_cbuf_index, value.secondary_cbuf_offset,
                    value.secondary_shift_left, value.count, value.size_shift);
}
auto BindingFields(Shader::ImageBufferDescriptor& value) {
    return std::tie(value.format, value.is_written, value.is_read, value.is_integer,
                    value.cbuf_index, value.cbuf_offset, value.count, value.size_shift);
}
auto BindingFields(Shader::TextureDescriptor& value) {
    return std::tie(value.type, value.is_depth, value.is_multisample, value.is_integer,
                    value.has_secondary, value.cbuf_index, value.cbuf_offset, value.shift_left,
                    value.secondary_cbuf_index, value.secondary_cbuf_offset,
                    value.secondary_shift_left, value.count, value.size_shift);
}
auto BindingFields(Shader::ImageDescriptor& value) {
    return std::tie(value.type, value.format, value.is_written, value.is_read, value.is_integer,
                    value.cbuf_index, value.cbuf_offset, value.count, value.size_shift);
}

template <typename T>
void WriteScalar(DiskBytes& bytes, T value) {
    Append32(bytes, static_cast<u32>(value));
}
template <typename T>
void ReadScalar(RecipeReader& reader, T& value) {
    const auto raw = reader.U32();
    if constexpr (std::is_same_v<T, bool>) {
        if (raw > 1)
            throw std::runtime_error("Invalid PSO metadata boolean");
    }
    value = static_cast<T>(raw);
}
template <typename Container>
void WriteBindings(DiskBytes& bytes, const Container& descriptors) {
    Append32(bytes, static_cast<u32>(descriptors.size()));
    for (auto descriptor : descriptors)
        std::apply([&](auto&... fields) { (WriteScalar(bytes, fields), ...); },
                   BindingFields(descriptor));
}
template <typename Container>
void ReadBindings(RecipeReader& reader, Container& descriptors) {
    const auto count = reader.U32();
    if (count > 1024 || count > descriptors.max_size())
        throw std::runtime_error("Invalid PSO descriptor count");
    for (u32 i = 0; i < count; ++i) {
        typename Container::value_type descriptor{};
        std::apply([&](auto&... fields) { (ReadScalar(reader, fields), ...); },
                   BindingFields(descriptor));
        descriptors.push_back(descriptor);
    }
}
void WriteInfo(DiskBytes& bytes, const Shader::Info& info) {
    Append32(bytes, info.constant_buffer_mask);
    for (auto size : info.constant_buffer_used_sizes)
        Append32(bytes, size);
    for (auto value : info.stores_frag_color)
        WriteScalar(bytes, value);
    for (auto value : info.interpolation)
        WriteScalar(bytes, value);
    for (size_t i = 0; i < info.loads.mask.size(); ++i) {
        WriteScalar(bytes, info.loads.mask.test(i));
        WriteScalar(bytes, info.stores.mask.test(i));
    }
    WriteBindings(bytes, info.constant_buffer_descriptors);
    WriteBindings(bytes, info.storage_buffers_descriptors);
    WriteBindings(bytes, info.texture_buffer_descriptors);
    WriteBindings(bytes, info.image_buffer_descriptors);
    WriteBindings(bytes, info.texture_descriptors);
    WriteBindings(bytes, info.image_descriptors);
}
Shader::Info ReadInfo(RecipeReader& reader) {
    Shader::Info info;
    info.constant_buffer_mask = reader.U32();
    for (auto& size : info.constant_buffer_used_sizes)
        size = reader.U32();
    for (auto& value : info.stores_frag_color)
        ReadScalar(reader, value);
    for (auto& value : info.interpolation)
        ReadScalar(reader, value);
    for (size_t i = 0; i < info.loads.mask.size(); ++i) {
        bool load{}, store{};
        ReadScalar(reader, load);
        ReadScalar(reader, store);
        info.loads.mask.set(i, load);
        info.stores.mask.set(i, store);
    }
    ReadBindings(reader, info.constant_buffer_descriptors);
    ReadBindings(reader, info.storage_buffers_descriptors);
    ReadBindings(reader, info.texture_buffer_descriptors);
    ReadBindings(reader, info.image_buffer_descriptors);
    ReadBindings(reader, info.texture_descriptors);
    ReadBindings(reader, info.image_descriptors);
    return info;
}
DiskBytes RequestBytes(const GraphicsPipelineKey& key) {
    const auto* bytes = reinterpret_cast<const u8*>(&key);
    return {bytes, bytes + sizeof(key)};
}
void WriteFeedback(DiskBytes& bytes, const Shader::RuntimeInfo* feedback) {
    Append32(bytes, feedback ? feedback->xfb_count : 0);
    if (!feedback)
        return;
    for (const auto& varying : feedback->xfb_varyings) {
        Append32(bytes, varying.buffer);
        Append32(bytes, varying.stream);
        Append32(bytes, varying.stride);
        Append32(bytes, varying.offset);
        Append32(bytes, varying.components);
    }
}
} // namespace

bool GraphicsPipelineKey::operator==(const GraphicsPipelineKey& rhs) const noexcept {
    return std::memcmp(this, &rhs, sizeof(*this)) == 0;
}

void FillGraphicsState(Tegra::Engines::Maxwell3D& maxwell3d, Pso::GraphicsPsoState& s) {
    const Maxwell& regs = maxwell3d.regs;
    std::memset(&s, 0, sizeof(s));

    const u32 topology = static_cast<u32>(maxwell3d.draw_manager.draw_state.topology);
    s.topology_type = static_cast<u8>(Pso::TopologyType(topology));
    s.patch_vertices = static_cast<u8>(regs.patch_vertices);

    // Rasterizer. Front face flips with the window origin like the Vulkan backend.
    const auto cull =
        Pso::CullMode(regs.gl_cull_test_enabled != 0, static_cast<u32>(regs.gl_cull_face));
    s.cull_mode = static_cast<u8>(cull.mode);
    static const bool no_cull = BisectEnabled("NXBOX_D3D12_NO_CULL");
    if (no_cull)
        s.cull_mode = static_cast<u8>(Pso::CullMode(false, 0).mode);
    s.cull_all = cull.cull_all && (s.topology_type == Pso::kTopologyTypeTriangle ||
                                   s.topology_type == Pso::kTopologyTypePatch);
    // Bit 1 requests SO rasterizer discard while preserving shader/query effects.
    if (regs.transform_feedback_enabled && (!regs.rasterize_enable || s.cull_all))
        s.cull_all = 2;
    bool ccw = regs.gl_front_face == Maxwell::FrontFace::CounterClockWise;
    if (regs.window_origin.flip_y != 0) {
        ccw = !ccw;
    }
    s.front_ccw = ccw;
    s.fill_mode = regs.polygon_mode_front == Maxwell::PolygonMode::Fill ? 3 : 2; // no point mode
    using Clip = Maxwell::ViewportClipControl::GeometryClip;
    const auto geometry_clip = regs.viewport_clip_control.geometry_clip;
    s.depth_clip = geometry_clip == Clip::Passthrough || geometry_clip == Clip::FrustumXYZ ||
                   geometry_clip == Clip::FrustumZ;
    static const bool no_depth_clip = BisectEnabled("NXBOX_D3D12_NO_DEPTH_CLIP");
    if (no_depth_clip)
        s.depth_clip = 0;
    // Polygon offset is enabled per primitive class (point / line / fill).
    const u32 class_ = Pso::TopologyType(topology);
    const bool bias = class_ == Pso::kTopologyTypePoint  ? regs.polygon_offset_point_enable != 0
                      : class_ == Pso::kTopologyTypeLine ? regs.polygon_offset_line_enable != 0
                                                         : regs.polygon_offset_fill_enable != 0;
    if (bias) {
        // GL/Maxwell constant units are "r" like D3D12's integer DepthBias.
        s.depth_bias = static_cast<s32>(std::lround(regs.depth_bias / 2.0f));
        s.slope_scaled_bias_bits = std::bit_cast<u32>(regs.slope_scale_depth_bias);
        s.bias_clamp_bits = std::bit_cast<u32>(regs.depth_bias_clamp);
    }

    // Depth-stencil.
    s.depth_enable = regs.zeta_enable != 0 && regs.depth_test_enable != 0;
    s.depth_write = regs.zeta_enable != 0 && regs.depth_write_enabled != 0;
    s.depth_func = static_cast<u8>(Pso::ComparisonFunc(static_cast<u32>(regs.depth_test_func)));
    s.stencil_enable = regs.zeta_enable != 0 && regs.stencil_enable != 0;
    const bool back_only = regs.stencil_two_side_enable && cull.mode == Pso::kCullFront;
    s.stencil_read_mask =
        StencilFaceMask(back_only ? regs.stencil_back_func_mask : regs.stencil_front_func_mask);
    s.stencil_write_mask =
        StencilFaceMask(back_only ? regs.stencil_back_mask : regs.stencil_front_mask);
    s.front = Face(regs.stencil_front_op);
    s.back = regs.stencil_two_side_enable ? Face(regs.stencil_back_op) : s.front;
    s.stencil_back_read_mask = regs.stencil_two_side_enable
                                   ? StencilFaceMask(regs.stencil_back_func_mask)
                                   : s.stencil_read_mask;
    s.stencil_back_write_mask = regs.stencil_two_side_enable
                                    ? StencilFaceMask(regs.stencil_back_mask)
                                    : s.stencil_write_mask;
    if (s.stencil_enable &&
        Pso::IndependentStencil(regs.stencil_two_side_enable != 0, cull.mode,
                                regs.stencil_front_ref, regs.stencil_back_ref,
                                regs.stencil_front_func_mask, regs.stencil_back_func_mask,
                                regs.stencil_front_mask, regs.stencil_back_mask))
        s.stencil_enable |= 2; // Bit 1 selects native independent face state.

    // Diagnostic bisection only: preserve writes while bypassing rejection.
    static const bool no_depth_test = BisectEnabled("NXBOX_D3D12_NO_DEPTH_TEST");
    static const bool no_stencil_test = BisectEnabled("NXBOX_D3D12_NO_STENCIL_TEST");
    if (no_depth_test)
        s.depth_func = 8; // D3D12_COMPARISON_FUNC_ALWAYS
    if (no_stencil_test)
        s.stencil_enable = 0;

    // Blend (independent per render target).
    s.logic_enable = regs.logic_op.enable != 0;
    s.logic_op = static_cast<u8>(Pso::LogicOp(static_cast<u32>(regs.logic_op.op)));
    s.independent_blend = !s.logic_enable;
    s.alpha_to_coverage = regs.anti_alias_alpha_control.alpha_to_coverage != 0;
    s.conservative_raster = regs.conservative_raster_enable != 0;
    s.smooth_lines = regs.line_anti_alias_enable != 0;
    s.depth_bounds = regs.zeta_enable && regs.depth_bounds_enable && !no_depth_test;
    for (size_t i = 0; i < Pso::kNumRenderTargets; ++i) {
        auto& out = s.blend[i];
        out = {0, 0xF, Pso::kBlendOne, Pso::kBlendZero, 1, Pso::kBlendOne, Pso::kBlendZero, 1};
        const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : i];
        out.write_mask = static_cast<u8>((mask.R ? 1 : 0) | (mask.G ? 2 : 0) | (mask.B ? 4 : 0) |
                                         (mask.A ? 8 : 0));
        if (!s.logic_enable && regs.blend.enable[i]) {
            if (regs.blend_per_target_enabled) {
                SetBlend(out, regs.blend_per_target[i]);
            } else {
                SetBlend(out, regs.blend);
            }
        }
    }

    s.sample_count = 1; // Overridden by the actual bound attachment sample count.

    // Vertex input.
    for (size_t i = 0; i < Pso::kNumVertexAttributes; ++i) {
        const auto& in = regs.vertex_attrib_format[i];
        auto& out = s.attributes[i];
        const auto format = Pso::VertexFormatForInputAssembler(
            static_cast<u32>(in.size.Value()), static_cast<u32>(in.type.Value()), in.bgra != 0);
        out.enabled = in.constant == 0 && format.dxgi != 0;
        out.slot = static_cast<u8>(in.buffer);
        out.offset = static_cast<u16>(in.offset);
        out.dxgi = format.dxgi;
        out.instanced = regs.vertex_stream_instances.IsInstancingEnabled(in.buffer);
        out.divisor = regs.vertex_streams[in.buffer].frequency;
        out.swap_rb = format.swap_rb;
        out.scaled = format.scaled;
    }
}

ID3D12RootSignature* PipelineCache::GetRootSignature(DescriptorCounts counts) {
    const auto key = std::pair{counts, UseRootCbvs(counts.cbv)};
    auto it = root_signatures.find(key);
    if (it == root_signatures.end()) {
        it = root_signatures.emplace(key, CreateRootSignature(context.Device(), counts, key.second))
                 .first;
    }
    return it->second.Get();
}

std::string PipelineCache::ProfileSummary() {
    std::scoped_lock lock{pipeline_mutex};
    return fmt::format(
        " guest_graphics_entries={} graphics_pso_entries={} failed_graphics_entries={} "
        "guest_compute_entries={} compute_pso_entries={} dxc_entries={}",
        guest_graphics.size(), graphics.size(), failed_graphics.size(), guest_compute.size(),
        compute.size(), dxc.Size());
}

GraphicsPipeline& PipelineCache::GetGraphics(const Pso::GraphicsPsoState& input_state,
                                             u32 d3d_topology, std::span<const StageInput> stages,
                                             const GraphicsPipelineKey* request,
                                             std::span<const u8> runtime_data) {
    std::scoped_lock lock{pipeline_mutex};
    auto state = input_state;
    if (PsoNormalizeEnabled())
        Pso::NormalizeGraphicsState(state);
    GraphicsPipelineKey key{};
    key.state = state;
    DescriptorCounts total;
    for (const auto& stage : stages) {
        DumpGraphicsHlsl(stage.hash, stage.stage, stage.hlsl);
        key.stage_hashes[static_cast<size_t>(stage.stage)] =
            Pso::HashBytes(stage.hlsl.data(), stage.hlsl.size(), stage.hash);
        total += stage.counts;
        if (stage.runtime && stage.runtime->xfb_count) {
            key.stage_hashes[static_cast<size_t>(stage.stage)] = Pso::HashBytes(
                stage.runtime->xfb_varyings.data(), sizeof(stage.runtime->xfb_varyings),
                key.stage_hashes[static_cast<size_t>(stage.stage)]);
        }
    }
    // Root/input binding ABI salt invalidates old disk PSOs without changing cache files.
    constexpr u32 StageBindingAbi = 10;
    key.stage_hashes[0] =
        Pso::HashBytes(&StageBindingAbi, sizeof(StageBindingAbi), key.stage_hashes[0]);
    const bool root_cbvs = UseRootCbvs(total.cbv);
    key.stage_hashes[0] = Pso::HashBytes(&root_cbvs, sizeof(root_cbvs), key.stage_hashes[0]);
    // Fragment occupies slot 5: folding topology after stage hashes prevents
    // its shader hash from overwriting the wrapper's IA topology identity.
    key.stage_hashes[5] = Pso::HashBytes(&d3d_topology, sizeof(d3d_topology), key.stage_hashes[5]);
    if (disk && request)
        disk->RememberFirstUse(RequestBytes(*request));
    if (const auto it = graphics.find(key); it != graphics.end()) {
        return *it->second;
    }
    if (const auto failed = failed_graphics.find(key); failed != failed_graphics.end()) {
        context.Profile().Count(FrameCount::NegativeCacheHits);
        throw std::runtime_error(failed->second);
    }
    InvalidateLastGraphicsPipeline();
    context.Profile().Count(FrameCount::PsoCacheMisses);
    const FrameProfile::Scope profile_pso{context.Profile(), FramePhase::Pso};
    try {
        std::vector<StageProgram> programs;
        for (const auto& stage : stages) {
            programs.push_back({stage.stage,
                                dxc.Compile(stage.hlsl, stage.stage,
                                            Pso::HashBytes(&key, sizeof(key)), disk.get()),
                                stage.hash});
        }
        ID3D12RootSignature* signature = GetRootSignature(total);
        const Shader::RuntimeInfo* feedback_info = nullptr;
        for (const auto& stage : stages)
            if (stage.runtime && stage.runtime->xfb_count)
                feedback_info = stage.runtime;
        std::unique_ptr<TransformFeedback> feedback;
        D3D12_STREAM_OUTPUT_DESC stream_output{};
        if (feedback_info) {
            feedback = std::make_unique<TransformFeedback>(context);
            feedback->Configure(*feedback_info);
            stream_output = feedback->Declaration((state.cull_all & 2) == 0);
        }
        const auto native_start = std::chrono::steady_clock::now();
        auto pipeline = std::make_unique<GraphicsPipeline>(
            context.Device(), signature, state, d3d_topology, programs, disk.get(),
            Pso::HashBytes(&key, sizeof(key)), Recipe(&state, d3d_topology, stages), stream_output,
            &context.Profile());
        pipeline->SetRootCbvs(UseRootCbvs(total.cbv));
        ReportSlowPso(Pso::HashBytes(&key, sizeof(key)), native_start, signature, total, stages,
                      programs);
        if (feedback_info)
            pipeline->SetTransformFeedback(*feedback_info);
        if (disk && request)
            disk->RememberPipeline({DiskRecordKind::Graphics, Pso::HashBytes(&key, sizeof(key)),
                                    Recipe(&state, d3d_topology, stages), RequestBytes(*request),
                                    DiskBytes(runtime_data.begin(), runtime_data.end())});
        return *graphics.emplace(key, std::move(pipeline)).first->second;
    } catch (const std::exception& error) {
        failed_graphics.emplace(key, error.what());
        Diagnostic(fmt::format("D3D12 failed PSO hash={:016x} cached reason={}",
                               Pso::HashBytes(&key, sizeof(key)), error.what()));
        throw;
    }
}

ComputePipeline& PipelineCache::GetCompute(const StageInput& stage,
                                           const GraphicsPipelineKey* request,
                                           std::span<const u8> runtime_data) {
    std::scoped_lock lock{pipeline_mutex};
    DumpGraphicsHlsl(stage.hash, stage.stage, stage.hlsl);
    const auto recipe = Recipe(nullptr, 0, std::span{&stage, size_t{1}});
    const u64 key = Pso::HashBytes(recipe.data(), recipe.size(), stage.hash);
    if (const auto it = compute.find(key); it != compute.end()) {
        return *it->second;
    }
    InvalidateLastGraphicsPipeline();
    context.Profile().Count(FrameCount::PsoCacheMisses);
    const FrameProfile::Scope profile_pso{context.Profile(), FramePhase::Pso};
    auto dxil = dxc.Compile(stage.hlsl, Shader::Stage::Compute, key, disk.get());
    const auto native_start = std::chrono::steady_clock::now();
    auto pipeline =
        std::make_unique<ComputePipeline>(context.Device(), GetRootSignature(stage.counts), dxil,
                                          disk.get(), key, recipe, &context.Profile());
    pipeline->SetRootCbvs(UseRootCbvs(stage.counts.cbv));
    const StageProgram compiled{Shader::Stage::Compute, dxil};
    ReportSlowPso(key, native_start, pipeline->RootSignature(), stage.counts,
                  std::span{&stage, size_t{1}}, std::span{&compiled, size_t{1}});
    if (disk && request)
        disk->RememberPipeline({DiskRecordKind::Compute, key, recipe, RequestBytes(*request),
                                DiskBytes(runtime_data.begin(), runtime_data.end())});
    return *compute.emplace(key, std::move(pipeline)).first->second;
}

namespace {
struct TranslationState {
    Shader::ObjectPool<Shader::IR::Inst> inst;
    Shader::ObjectPool<Shader::IR::Block> block;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow;
    Shader::Profile profile;
    Shader::HostTranslateInfo host;

    explicit TranslationState(ID3D12Device* device) {
        static const bool cube_one = BisectEnabled("NXBOX_D3D12_CUBE_ONE");
        profile.hlsl_cube_one = cube_one;
        static const bool shadow_one = BisectEnabled("NXBOX_D3D12_SHADOW_ONE");
        profile.hlsl_shadow_one = shadow_one;
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
              "D3D12 shader options");
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 waves{};
        Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &waves, sizeof(waves)),
              "D3D12 wave options");
        if (!waves.WaveOps || waves.WaveLaneCountMin < 32) {
            throw Shader::NotImplementedException("D3D12 requires waves of at least 32 lanes");
        }
        host.support_float64 = options.DoublePrecisionFloatShaderOps != FALSE;
        host.support_int64 = waves.Int64ShaderOps != FALSE;
        host.support_viewport_index_layer =
            options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation !=
            FALSE;
        host.min_ssbo_alignment = 16;
        host.ApplyDescriptorLimitPolicy();
        profile.support_typeless_image_loads = options.TypedUAVLoadAdditionalFormats != FALSE;
        profile.support_int64 = host.support_int64;
        profile.support_vertex_instance_id = true;
        profile.support_vote = true;
        profile.support_quad_shuffles = true;
        profile.support_derivative_control = true;
        profile.support_viewport_index_layer_non_geometry = host.support_viewport_index_layer;
        profile.warp_size_potentially_larger_than_guest = waves.WaveLaneCountMax > 32;
        profile.max_user_clip_distances = 8;
        profile.min_ssbo_alignment = 16;
    }
};

// IR references the object pools, which must survive until HLSL emission finishes.
struct ComputeTranslation {
    TranslationState translation;
    Shader::IR::Program program;
    ComputeTranslation(ID3D12Device* device, Shader::Environment& env) : translation{device} {
        Shader::Maxwell::Flow::CFG cfg{env, translation.flow, env.StartAddress()};
        program = Shader::Maxwell::TranslateProgram(translation.inst, translation.block, env, cfg,
                                                    translation.host);
    }
};

Shader::RuntimeInfo GraphicsRuntime(const Maxwell& regs, const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous, u32 topology) {
    Shader::RuntimeInfo runtime;
    runtime.force_early_z = regs.mandated_early_z != 0;
    runtime.input_topology = topology == 0    ? Shader::InputTopology::Points
                             : topology <= 3  ? Shader::InputTopology::Lines
                             : topology <= 9  ? Shader::InputTopology::Triangles
                             : topology <= 11 ? Shader::InputTopology::LinesAdjacency
                                              : Shader::InputTopology::TrianglesAdjacency;
    if (program.stage == Shader::Stage::Fragment && regs.alpha_test_enabled &&
        Pso::ComparisonFunc(static_cast<u32>(regs.alpha_test_func)) != 8) {
        runtime.alpha_test_func = static_cast<Shader::CompareFunction>(
            Pso::ComparisonFunc(static_cast<u32>(regs.alpha_test_func)) - 1);
        runtime.alpha_test_reference = regs.alpha_test_ref;
    }
    if (program.stage == Shader::Stage::Fragment && regs.blend.enable[0]) {
        const auto dual_source = [](const auto& blend) {
            const auto is_dual = [](auto factor) {
                const u32 native = Pso::BlendFactor(static_cast<u32>(factor));
                return native >= 16 && native <= 19;
            };
            return is_dual(blend.color_source) || is_dual(blend.color_dest) ||
                   is_dual(blend.alpha_source) || is_dual(blend.alpha_dest);
        };
        runtime.dual_source_blend = regs.blend_per_target_enabled
                                        ? dual_source(regs.blend_per_target[0])
                                        : dual_source(regs.blend);
    }
    if (regs.transform_feedback_enabled)
        throw Shader::NotImplementedException("D3D12 guest transform feedback linkage");
    if (previous) {
        runtime.previous_stage_stores = previous->info.stores;
        runtime.previous_stage_legacy_stores_mapping = previous->info.legacy_stores_mapping;
    } else {
        runtime.previous_stage_stores.mask.set();
    }
    const auto& viewport = regs.viewport_transform[0];
    bool positive_height = !regs.viewport_scale_offset_enabled || viewport.scale_y > 0;
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft)
        positive_height = !positive_height;
    if (regs.viewport_scale_offset_enabled &&
        viewport.swizzle.y == Maxwell::ViewportSwizzle::NegativeY)
        positive_height = !positive_height;
    const auto final_vertex_stage = regs.IsShaderConfigEnabled(Maxwell::ShaderType::Geometry)
                                        ? Shader::Stage::Geometry
                                        : Shader::Stage::VertexB;
    runtime.y_negate = program.stage == final_vertex_stage && positive_height;
    runtime.convert_depth_mode =
        program.stage == final_vertex_stage && regs.depth_mode == Maxwell::DepthMode::MinusOneToOne;
    if (regs.transform_feedback_enabled && program.stage == final_vertex_stage) {
        VideoCommon::TransformFeedbackState feedback{};
        for (size_t i = 0; i < feedback.layouts.size(); ++i) {
            const auto& control = regs.transform_feedback.controls[i];
            feedback.layouts[i] = {control.stream, control.varying_count, control.stride};
        }
        feedback.varyings = regs.stream_out_layout;
        auto [varyings, count] = VideoCommon::MakeTransformFeedbackVaryings(feedback);
        runtime.xfb_varyings = varyings;
        runtime.xfb_count = count;
    }
    for (size_t i = 0; i < runtime.generic_input_types.size(); ++i) {
        const auto& attribute = regs.vertex_attrib_format[i];
        if (attribute.constant) {
            runtime.generic_input_types[i] = Shader::AttributeType::Disabled;
            continue;
        }
        using Type = Maxwell::VertexAttribute::Type;
        if (Pso::VertexSignedPacked10(static_cast<u32>(attribute.size.Value()),
                                      static_cast<u32>(attribute.type.Value()))) {
            // Fetched as R10G10B10A2_UINT (see Pso::VertexFormatFor); HLSL decodes.
            runtime.generic_input_types[i] = attribute.type.Value() == Type::SNorm
                                                 ? Shader::AttributeType::SignedNorm10
                                                 : Shader::AttributeType::SignedInt10;
            continue;
        }
        switch (attribute.type.Value()) {
        case Type::SInt:
            runtime.generic_input_types[i] = Shader::AttributeType::SignedInt;
            break;
        case Type::UInt:
            runtime.generic_input_types[i] = Shader::AttributeType::UnsignedInt;
            break;
        case Type::SScaled:
            runtime.generic_input_types[i] = Shader::AttributeType::SignedScaled;
            break;
        case Type::UScaled:
            runtime.generic_input_types[i] = Shader::AttributeType::UnsignedScaled;
            break;
        default:
            runtime.generic_input_types[i] = Shader::AttributeType::Float;
            break;
        }
    }
    for (size_t i = 0; i < runtime.color_output_types.size(); ++i) {
        const auto format = regs.rt[regs.rt_control.Map(static_cast<u32>(i))].format;
        if (format == Tegra::RenderTargetFormat::NONE)
            continue;
        const auto pixel = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
        runtime.color_output_types[i] = VideoCore::Surface::IsPixelFormatInteger(pixel)
                                            ? (VideoCore::Surface::IsPixelFormatSignedInteger(pixel)
                                                   ? Shader::AttributeType::SignedInt
                                                   : Shader::AttributeType::UnsignedInt)
                                            : Shader::AttributeType::Float;
    }
    return runtime;
}
} // namespace

namespace {
struct StreamOutputDeclaration {
    std::vector<D3D12_SO_DECLARATION_ENTRY> entries;
    std::array<UINT, 4> strides{};

    D3D12_STREAM_OUTPUT_DESC Description(const Pso::GraphicsPsoState& state) const {
        UINT stride_count{};
        for (UINT i = 0; i < strides.size(); ++i)
            if (strides[i])
                stride_count = i + 1;
        return {entries.data(), static_cast<UINT>(entries.size()), strides.data(), stride_count,
                (state.cull_all & 2) ? D3D12_SO_NO_RASTERIZED_STREAM : 0U};
    }
};
StreamOutputDeclaration MakeStreamOutputDeclaration(const Shader::RuntimeInfo* feedback) {
    StreamOutputDeclaration declaration;
    if (feedback) {
        std::vector<StreamOutputVarying> varyings;
        for (u32 attribute = 0; attribute < feedback->xfb_count; ++attribute) {
            const auto& v = feedback->xfb_varyings[attribute];
            if (v.components)
                varyings.push_back(
                    {attribute, v.buffer, v.stream, v.stride, v.offset, v.components});
        }
        const auto layout = MakeStreamOutputLayout(varyings);
        declaration.strides = layout.strides;
        for (const auto& element : layout.elements) {
            const char* semantic{};
            u32 index{};
            if (element.attribute != StreamOutputElement::Gap) {
                if (element.attribute / 4 == 7) {
                    semantic = "NX_XFB_POSITION";
                } else if (element.attribute >= 32 && element.attribute < 160) {
                    semantic = "TEXCOORD";
                    index = (element.attribute - 32) / 4;
                } else {
                    throw std::runtime_error("Unsupported stream output semantic");
                }
            }
            u32 remaining = element.components;
            do {
                const auto components = std::min(remaining, 4U);
                declaration.entries.push_back(
                    {element.stream, semantic, index, static_cast<BYTE>(element.start_component),
                     static_cast<BYTE>(components), static_cast<BYTE>(element.buffer)});
                remaining -= components;
            } while (remaining);
        }
    }
    return declaration;
}

std::shared_ptr<GraphicsPipeline> BuildAsyncGraphics(
    ID3D12Device* device, DiskPipelineCache* disk, Pso::GraphicsPsoState state, u32 d3d_topology,
    u32 guest_topology, const Maxwell& regs, const Pso::FramebufferFormats& formats,
    const std::array<u64, 6>& guest_hashes,
    std::array<std::unique_ptr<Shader::Environment>, 6>& environments,
    const GraphicsPipelineKey& request, bool profile_build) {
    const u64 request_hash = Pso::HashBytes(&request, sizeof(request));
    // Async jobs that never return jam the worker pool; this shows the phase each one is stuck in.
    static const bool trace = BisectEnabled("NXBOX_D3D12_PSO_TRACE");
    PsoBuildStats trace_stats;
    std::optional<ScopedPsoBuildStats> collect;
    if (trace || profile_build)
        collect.emplace(trace_stats);
    const auto compile_start = trace || profile_build ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
    const auto mark = [&](std::string_view phase) {
        if (trace) {
            const auto elapsed = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - compile_start)
                                     .count();
            Diagnostic(fmt::format("PSO_TRACE request={:016x} {} elapsed_ms={:.3f} thread={}",
                                   request_hash, phase, elapsed, GetCurrentThreadId()));
        }
    };
    mark("start");
    TranslationState translation{device};
    std::array<Shader::IR::Program, 6> programs;
    std::array<std::string, 6> sources;
    std::array<Shader::RuntimeInfo, 6> runtimes;
    std::vector<StageInput> stages;
    // The async cache keeps only what() of a failed job and the console log prints just
    // "request=<hash> failed=<what>". Name the step, stage and guest shader so a terse message
    // such as std::vector::at's "invalid vector subscript" says where it came from.
    const auto tagged = [&](std::string_view phase, size_t index, auto&& step) {
        static constexpr std::array<std::string_view, 6> kGuestStages{
            "VertexA",          "VertexB",  "TessellationControl",
            "TessellationEval", "Geometry", "Fragment"};
        const auto where = [&] {
            return fmt::format("[{} {} guest={:016x}] ", phase, kGuestStages[index],
                               guest_hashes[index]);
        };
        try {
            step();
        } catch (Shader::Exception& error) {
            error.Prepend(where());
            throw;
        } catch (const std::exception& error) {
            throw std::runtime_error(where() + error.what());
        }
    };
    for (size_t index = 0; index < programs.size(); ++index) {
        if (!guest_hashes[index])
            continue;
        auto& env = *environments[index];
        tagged("translate", index, [&] {
            Shader::Maxwell::Flow::CFG cfg{
                env, translation.flow,
                static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader)), index == 0};
            programs[index] = Shader::Maxwell::TranslateProgram(translation.inst, translation.block,
                                                                env, cfg, translation.host);
        });
        if (index == 1 && guest_hashes[0]) {
            tagged("merge-dual-vertex", index, [&] {
                programs[index] =
                    Shader::Maxwell::MergeDualVertexPrograms(programs[0], programs[1], env);
            });
        }
    }
    mark("translated");
    Shader::Backend::Bindings bindings;
    const Shader::IR::Program* previous{};
    for (size_t index = guest_hashes[0] && guest_hashes[1] ? 1 : 0; index < 6; ++index) {
        if (!guest_hashes[index])
            continue;
        auto& program = programs[index];
        if (index == 0)
            program.stage = Shader::Stage::VertexB;
        if (program.stage == Shader::Stage::Fragment) {
            // Guest shaders may export more colors than this framebuffer binds.
            // Specialize the DXIL signature together with the PSO attachment key.
            for (size_t slot = 0; slot < formats.colors.size(); ++slot)
                program.info.stores_frag_color[slot] &= formats.colors[slot] != 0;
        }
        const auto& runtime = runtimes[index] =
            GraphicsRuntime(regs, program, previous, guest_topology);
        Shader::Maxwell::ConvertLegacyToGeneric(program, runtime);
        if (program.stage == Shader::Stage::Fragment &&
            guest_topology == static_cast<u32>(Maxwell::PrimitiveTopology::Quads) &&
            regs.provoking_vertex == Maxwell::ProvokingVertex::Last) {
            for (size_t attribute = 0; attribute < program.info.interpolation.size(); ++attribute) {
                if (program.info.loads.Generic(attribute) &&
                    program.info.interpolation[attribute] == Shader::Interpolation::Flat) {
                    ReportUnsupported("provoking_vertex=Last for flat quads requires "
                                      "flat-value replication without changing diagonal");
                    throw std::runtime_error("Unsupported flat quad provoking vertex");
                }
            }
        }
        tagged("emit", index, [&] {
            sources[index] =
                Shader::Backend::HLSL::EmitHLSL(translation.profile, runtime, program, bindings);
        });
        ApplyTextureProbe(program.stage, sources[index]);
        DumpGraphicsHlsl(guest_hashes[index], program.stage, sources[index]);
        const auto counts = CountStageTables(program.info);
        stages.push_back({program.stage,
                          guest_hashes[index],
                          sources[index],
                          {counts.cbv, counts.srv, counts.uav, counts.sampler},
                          &runtime});
        previous = &program;
    }
    std::uint32_t vertex_reads = 0;
    const auto& vertex_info = programs[guest_hashes[1] ? 1 : 0].info;
    for (std::size_t i = 0; i < Pso::kNumVertexAttributes; ++i)
        if (vertex_info.loads.Generic(i))
            vertex_reads |= std::uint32_t{1} << i;
    for (size_t index = 0; index < state.attributes.size(); ++index) {
        if (!(vertex_reads & (u32{1} << index)) || regs.vertex_attrib_format[index].constant)
            continue;
        const auto& input = regs.vertex_attrib_format[index];
        const auto& attribute = state.attributes[index];
        const u32 size = static_cast<u32>(input.size.Value());
        const bool component_conversion = attribute.swap_rb || attribute.scaled;
        if (!attribute.dxgi || (attribute.swap_rb && size == Pso::VSize::B10G11R11)) {
            ReportUnsupported(
                fmt::format("vertex_attrib_format[{}] size={:x} type={} bgra={} "
                            "requires vertex pulling/component conversion native_conversion={}",
                            index, size, static_cast<u32>(input.type.Value()), input.bgra != 0,
                            component_conversion));
            throw std::runtime_error("Unsupported vertex input conversion");
        }
    }
    Pso::ApplyVertexReads(state, vertex_reads);
    if (PsoNormalizeEnabled())
        Pso::NormalizeGraphicsState(state);

    const auto translation_end = trace || profile_build ? std::chrono::steady_clock::now()
                                                        : std::chrono::steady_clock::time_point{};
    thread_local DxcCache compiler;
    std::vector<StageProgram> compiled;
    DescriptorCounts total;
    for (const auto& stage : stages) {
        mark(fmt::format("dxc stage={} bytes={}", static_cast<u32>(stage.stage), stage.hlsl.size()));
        compiled.push_back({stage.stage,
                            compiler.Compile(stage.hlsl, stage.stage, request_hash, disk),
                            stage.hash});
        total += stage.counts;
    }
    mark("root_signature");
    auto signature = CreateRootSignature(device, total, UseRootCbvs(total.cbv));
    // Stream-output declaration is pure CPU state, independent of Context and buffers.
    const Shader::RuntimeInfo* feedback{};
    for (const auto& stage : stages)
        if (stage.runtime && stage.runtime->xfb_count)
            feedback = stage.runtime;
    const auto stream_output = MakeStreamOutputDeclaration(feedback);
    const auto recipe = Recipe(&state, d3d_topology, stages);
    // The immutable recipe includes source, linkage state and fixed state.
    const u64 native_key = Pso::HashBytes(recipe.data(), recipe.size(), request_hash);
    const auto native_start = std::chrono::steady_clock::now();
    mark("native_pso");
    auto pipeline =
        std::make_shared<GraphicsPipeline>(device, signature, state, d3d_topology, compiled, disk,
                                           native_key, recipe, stream_output.Description(state));
    pipeline->SetRootCbvs(UseRootCbvs(total.cbv));
    mark("done");
    if (trace || profile_build)
        Diagnostic(fmt::format(
            "PSO_BUILD_COST request={:016x} kind=graphics driver_builds={} "
            "translation_emit_ms={:.3f} dxc_ms={:.3f} root_signature_ms={:.3f} driver_ms={:.3f} "
            "wall_ms={:.3f} dxil_disk_hits={} dxil_disk_misses={} dxil_memory_hits={}",
            request_hash, trace_stats.driver_builds,
            std::chrono::duration<double, std::milli>(translation_end - compile_start).count(),
            trace_stats.dxc_us / 1000.0, trace_stats.root_us / 1000.0,
            trace_stats.driver_us / 1000.0,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                      compile_start)
                .count(),
            trace_stats.disk_hits, trace_stats.disk_misses, trace_stats.memory_hits));
    ReportSlowPso(native_key, native_start, signature.Get(), total, stages, compiled);
    if (feedback)
        pipeline->SetTransformFeedback(*feedback);
    for (size_t index = 1; index < 6; ++index)
        if (guest_hashes[index])
            pipeline->SetStageInfo(index - 1, &programs[index].info);
    if (guest_hashes[0] && !guest_hashes[1])
        pipeline->SetStageInfo(0, &programs[0].info);
    if (disk) {
        DiskBytes metadata;
        for (size_t index = 0; index < NUM_GRAPHICS_STAGES; ++index) {
            const auto* info = pipeline->StageInfo(index);
            Append32(metadata, info ? 1u : 0u);
            if (info)
                WriteInfo(metadata, *info);
        }
        WriteFeedback(metadata, pipeline->TransformFeedbackInfo());
        if (!disk->RememberPipeline({DiskRecordKind::Graphics, native_key, recipe,
                                     RequestBytes(request), std::move(metadata)}))
            Diagnostic(fmt::format("D3D12 PSO recipe write failed request={:016x}", request_hash));
    }
    return pipeline;
}

struct PrewarmPhases {
    std::atomic<u64> dxil_disk_hits{}, dxil_disk_misses{}, dxil_memory_hits{};
    std::atomic<u64> dxc_us{}, root_us{}, driver_us{}, driver_builds{}, built{}, failed{};
};

void BuildPrewarmedPipeline(ID3D12Device* device, DiskPipelineCache* disk,
                            const DiskPipelineRecord& record,
                            std::shared_ptr<GraphicsPipeline>& graphics,
                            std::shared_ptr<ComputePipeline>& compute, PrewarmPhases& phases) {
    PsoBuildStats dxc_stats;
    const ScopedPsoBuildStats collect{dxc_stats};
    bool success = false;
    SCOPE_EXIT {
        phases.dxil_disk_hits.fetch_add(dxc_stats.disk_hits, std::memory_order_relaxed);
        phases.dxil_disk_misses.fetch_add(dxc_stats.disk_misses, std::memory_order_relaxed);
        phases.dxil_memory_hits.fetch_add(dxc_stats.memory_hits, std::memory_order_relaxed);
        phases.dxc_us.fetch_add(dxc_stats.dxc_us, std::memory_order_relaxed);
        phases.root_us.fetch_add(dxc_stats.root_us, std::memory_order_relaxed);
        phases.driver_us.fetch_add(dxc_stats.driver_us, std::memory_order_relaxed);
        phases.driver_builds.fetch_add(dxc_stats.driver_builds, std::memory_order_relaxed);
        (success ? phases.built : phases.failed).fetch_add(1, std::memory_order_relaxed);
    };
    const auto decoded = ReadRecipe(record);
    const auto& stages = decoded.stages;
    const auto& state = decoded.state;
    const auto topology = decoded.topology;
    const auto root_cbvs = decoded.root_cbvs;
    DescriptorCounts total;
    thread_local DxcCache compiler;
    std::vector<StageProgram> compiled;
    for (const auto& stage : stages) {
        DumpGraphicsHlsl(stage.hash, stage.stage, stage.hlsl);
        total += stage.counts;
        compiled.push_back(
            {stage.stage, compiler.Compile(stage.hlsl, stage.stage, record.key, disk), stage.hash});
    }
    RecipeReader metadata{record.runtime_data};
    auto signature = CreateRootSignature(device, total, root_cbvs);
    if (record.kind == DiskRecordKind::Compute) {
        if (stages.size() != 1 || stages[0].stage != Shader::Stage::Compute)
            throw std::runtime_error("Invalid compute prewarm stages");
        auto info = ReadInfo(metadata);
        if (!metadata.Empty())
            throw std::runtime_error("Trailing compute prewarm metadata");
        compute = std::make_shared<ComputePipeline>(device, signature, compiled[0].dxil, disk,
                                                    record.key, record.payload);
        compute->SetRootCbvs(root_cbvs);
        compute->info = std::move(info);
    } else {
        std::array<std::optional<Shader::Info>, NUM_GRAPHICS_STAGES> infos;
        for (auto& info : infos) {
            bool present{};
            ReadScalar(metadata, present);
            if (present)
                info = ReadInfo(metadata);
        }
        Shader::RuntimeInfo runtime;
        runtime.xfb_count = metadata.U32();
        if (runtime.xfb_count > runtime.xfb_varyings.size())
            throw std::runtime_error("Invalid prewarm stream output count");
        if (runtime.xfb_count) {
            for (auto& varying : runtime.xfb_varyings) {
                varying.buffer = metadata.U32();
                varying.stream = metadata.U32();
                varying.stride = metadata.U32();
                varying.offset = metadata.U32();
                varying.components = metadata.U32();
            }
        }
        if (!metadata.Empty())
            throw std::runtime_error("Trailing graphics prewarm metadata");
        const auto* feedback = runtime.xfb_count ? &runtime : nullptr;
        const auto stream_output = MakeStreamOutputDeclaration(feedback);
        graphics = std::make_shared<GraphicsPipeline>(device, signature, state, topology, compiled,
                                                      disk, record.key, record.payload,
                                                      stream_output.Description(state));
        graphics->SetRootCbvs(root_cbvs);
        for (size_t index = 0; index < infos.size(); ++index)
            if (infos[index])
                graphics->SetStageInfo(index, &*infos[index]);
        if (feedback)
            graphics->SetTransformFeedback(runtime);
    }
    // Native LoadOrCreate writes the recipe too; preserve its guest identity after replay.
    disk->RememberPipeline(record);
    success = true;
}
} // namespace

const PipelineCache::CachedFeature<D3D12_FEATURE_DATA_FORMAT_SUPPORT>&
PipelineCache::QueryFormatSupport(DXGI_FORMAT format) {
    auto& cached = format_support[format];
    cached.data.Format = format;
    return QueryFeature(D3D12_FEATURE_FORMAT_SUPPORT, cached);
}

void PipelineCache::InvalidateLastGraphicsPipeline() {
    last_graphics_pipeline = nullptr;
    last_graphics_request_current = false;
}

void PipelineCache::StoreGuestGraphics(const GraphicsPipelineKey& request,
                                       GraphicsPipeline* pipeline) {
    InvalidateLastGraphicsPipeline();
    guest_graphics.emplace(request, pipeline);
}

IGraphicsPipeline* PipelineCache::RememberGraphicsPipeline(const GraphicsPipelineKey& request,
                                                           IGraphicsPipeline* pipeline) {
    // Pending asynchronous requests must be looked up again until compilation finishes.
    if (pipeline) {
        last_graphics_request = request;
        last_graphics_pipeline = pipeline;
        last_graphics_request_current = true;
        // Profiling is fixed for the context lifetime; avoid hashing even on installation
        // during normal gameplay.
        last_graphics_prewarmed =
            context.Profile().Enabled() && prewarmed_graphics.contains(request);
    }
    return pipeline;
}

void PipelineCache::RecordGraphicsDraw(u64 frame) {
    if (!RtHistoryEnabled() || !maxwell3d || maxwell3d->regs.rt_control.count == 0)
        return;
    const auto& rt0 = maxwell3d->regs.rt[maxwell3d->regs.rt_control.Map(0)];
    rt_draw_history.Record(rt0.Address(), frame);
}

std::unique_ptr<PipelineCache::AsyncCache> PipelineCache::CreateAsyncCache() {
    // Explicit async mode raises priority without another DXC instance on memory-limited UWP.
    // Preserve user overrides and the pool's temporary ABOVE_NORMAL boost for waiters.
    return std::make_unique<AsyncCache>(RuntimePsoThreads(), 4, std::chrono::seconds{5}, 4,
                                        [](AsyncCache::NativeHandle handle, int priority) {
                                            if (NonBlockingPsoEnabled() &&
                                                priority == THREAD_PRIORITY_BELOW_NORMAL &&
                                                !std::getenv("NXBOX_D3D12_PSO_WORKER_PRIORITY"))
                                                priority = THREAD_PRIORITY_NORMAL;
                                            AsyncCache::SetPriority(handle, priority);
                                        });
}

void PipelineCache::TickFrame() {
    std::scoped_lock lock{pipeline_mutex};
    pso_frame_budget.Reset();
}

std::chrono::steady_clock::duration PipelineCache::PendingPsoWaitBudget() {
    // An explicit async-PSO opt-in drops every pending draw/dispatch immediately. Keep
    // compiling its immutable snapshot; later requests install the completed pipeline.
    if (NonBlockingPsoEnabled())
        return std::chrono::steady_clock::duration::zero();
    return FrameBoundedPsoEnabled() ? pso_frame_budget.Remaining(PsoWaitBudget()) : PsoWaitBudget();
}

IGraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(const Pso::FramebufferFormats& formats,
                                                          bool is_indexed, u64 frame) try {
    const FrameProfile::Scope profile_lookup{context.Profile(), FramePhase::PipelineLookup};
    if (!maxwell3d)
        return nullptr;
    std::scoped_lock lock{pipeline_mutex};
    SyncGuestHost();
    if (!RefreshStages(guest_hashes))
        return nullptr;
    const auto& regs = maxwell3d->regs;
    if (VideoCommon::ShaderDiscovery::Enabled()) {
        VideoCommon::ShaderDiscovery::Base(gpu_memory->GetID(), regs.program_region.Address(),
                                           false);
        for (unsigned slot = 0; slot < guest_hashes.size(); ++slot) {
            if (guest_hashes[slot])
                VideoCommon::ShaderDiscovery::Demand(gpu_memory->GetID(),
                                                     regs.program_region.Address() +
                                                         regs.pipelines[slot].offset,
                                                     guest_hashes[slot], slot, false);
        }
    }
    const GraphicsStateInputs inputs{
        context.GraphicsStateTracker().Update(*maxwell3d),
        guest_hashes,
        formats,
        static_cast<u32>(maxwell3d->draw_manager.draw_state.topology),
        regs.zeta_enable,
        is_indexed,
        {regs.pipelines[2].enable != 0, regs.pipelines[3].enable != 0}};
    const bool reuse = StateFastpathEnabled() && graphics_state_inputs.Matches(inputs);
    // The tracker generation covers register writes; inputs also cover shader invalidation,
    // framebuffer formats and draw topology. Do not copy, compare or hash the full key on hits.
    // Verification still computes the reference key. Pending PSOs never install a pointer.
    if (PipelineLookupFastpathEnabled() && !StateVerifyEnabled() && reuse &&
        last_graphics_request_current && last_graphics_pipeline) {
        if (context.Profile().Enabled() && last_graphics_prewarmed)
            context.Profile().Count(FrameCount::PrewarmedPsoHits);
        return last_graphics_pipeline;
    }
    Pso::GraphicsPsoState state;
    Pso::TopologyInfo topology{};
    GraphicsPipelineKey request{};
    const auto compute_request = [&]() -> bool {
        if (const auto* reason = UnsupportedState(
                maxwell3d->regs, static_cast<u32>(maxwell3d->draw_manager.draw_state.topology))) {
            ReportUnsupported(reason);
            return false;
        }
        FillGraphicsState(*maxwell3d, state);
        Pso::ApplyFramebufferFormats(state, formats);
        if (!Pso::DsvHasStencil(state.dsv_format))
            state.stencil_enable = 0;
        if (state.stencil_enable & 2) {
            const auto& options = QueryFeature(D3D12_FEATURE_D3D12_OPTIONS14, graphics_options14);
            if (FAILED(options.result) ||
                !options.data.IndependentFrontAndBackStencilRefMaskSupported) {
                ReportUnsupported(
                    "stencil_back_ref/stencil_back_mask/stencil_back_func_mask differ; "
                    "host lacks IndependentFrontAndBackStencilRefMaskSupported");
                return false;
            }
        }
        if (state.depth_bounds) {
            const auto& options = QueryFeature(D3D12_FEATURE_D3D12_OPTIONS2, graphics_options2);
            if (FAILED(options.result) || !options.data.DepthBoundsTestSupported) {
                ReportUnsupported("depth_bounds_enable/depth_bounds unsupported by host device");
                return false;
            }
        }
        const u32 constant_factors = Pso::ConstantBlendFactors(state);
        if (constant_factors & 2) {
            const auto& options = QueryFeature(D3D12_FEATURE_D3D12_OPTIONS13, graphics_options13);
            if (FAILED(options.result) || !options.data.AlphaBlendFactorSupported) {
                if (constant_factors & 1) {
                    ReportUnsupported(
                        "blend mixes constant-color and constant-alpha RGB factors; host lacks "
                        "AlphaBlendFactorSupported and needs shader blend emulation");
                    return false;
                }
                // The draw path replicates blend_color.a into RGB. No shader change
                // is needed when RGB never consumes the original constant color.
                Pso::EmulateConstantAlpha(state);
            }
        }
        if (state.logic_enable || state.conservative_raster) {
            const auto& options = QueryFeature(D3D12_FEATURE_D3D12_OPTIONS, graphics_options);
            if (FAILED(options.result) ||
                (state.logic_enable && !options.data.OutputMergerLogicOp) ||
                (state.conservative_raster &&
                 options.data.ConservativeRasterizationTier ==
                     D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED)) {
                ReportUnsupported("logic_op/conservative_raster_enable unsupported by host device");
                return false;
            }
        }
        if (state.logic_enable) {
            std::optional<u8> mask;
            for (size_t i = 0; i < formats.colors.size(); ++i) {
                if (!formats.colors[i])
                    continue;
                if (mask && *mask != state.blend[i].write_mask) {
                    ReportUnsupported("logic_op with independent color_mask requires programmable "
                                      "blend emulation");
                    return false;
                }
                mask = state.blend[i].write_mask;
                const auto& support =
                    QueryFormatSupport(static_cast<DXGI_FORMAT>(formats.colors[i]));
                if (FAILED(support.result) ||
                    !(support.data.Support2 & D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP)) {
                    ReportUnsupported("logic_op RTV format lacks OUTPUT_MERGER_LOGIC_OP; requires "
                                      "programmable blend emulation");
                    return false;
                }
            }
            if (mask)
                state.blend[0].write_mask = *mask;
        }
        topology = Pso::PrimitiveTopology(
            static_cast<u32>(maxwell3d->draw_manager.draw_state.topology), state.patch_vertices);
        if (PsoNormalizeEnabled())
            Pso::NormalizeGraphicsState(state);
        request = GraphicsPipelineKey{guest_hashes, state};
        u64 specialization = Pso::HashBytes(&regs.depth_mode, sizeof(regs.depth_mode));
        specialization =
            Pso::HashBytes(&regs.window_origin, sizeof(regs.window_origin), specialization);
        specialization = Pso::HashBytes(&regs.viewport_scale_offset_enabled,
                                        sizeof(regs.viewport_scale_offset_enabled), specialization);
        const auto& viewport = regs.viewport_transform[0];
        const bool positive_y = viewport.scale_y > 0;
        specialization = Pso::HashBytes(&positive_y, sizeof(positive_y), specialization);
        specialization =
            Pso::HashBytes(&viewport.swizzle, sizeof(viewport.swizzle), specialization);
        specialization = vertex_specialization_hash.Get(
            regs.vertex_attrib_format.data(), sizeof(regs.vertex_attrib_format), specialization);
        specialization =
            Pso::HashBytes(&topology.topology, sizeof(topology.topology), specialization);
        specialization =
            Pso::HashBytes(&regs.provoking_vertex, sizeof(regs.provoking_vertex), specialization);
        specialization =
            Pso::HashBytes(&regs.mandated_early_z, sizeof(regs.mandated_early_z), specialization);
        const auto alpha = Pso::AlphaTestSpecialization(
            regs.alpha_test_enabled != 0,
            Pso::ComparisonFunc(static_cast<u32>(regs.alpha_test_func)),
            std::bit_cast<u32>(regs.alpha_test_ref));
        specialization = Pso::HashBytes(alpha.data(), sizeof(alpha), specialization);
        specialization = Pso::HashBytes(&regs.transform_feedback_enabled,
                                        sizeof(regs.transform_feedback_enabled), specialization);
        if (regs.transform_feedback_enabled) {
            specialization =
                Pso::HashBytes(regs.transform_feedback.controls.data(),
                               sizeof(regs.transform_feedback.controls), specialization);
            specialization = Pso::HashBytes(regs.stream_out_layout.data(),
                                            sizeof(regs.stream_out_layout), specialization);
        }
        request.stage_hashes[0] =
            Pso::HashBytes(&specialization, sizeof(specialization), request.stage_hashes[0]);
        return true;
    };
    if (!reuse || StateVerifyEnabled()) {
        // A new input set can miss or be unsupported. Keep the old key for the equality
        // fallback, but never associate its pointer with a newly cached pending request.
        last_graphics_request_current = false;
        const bool supported = compute_request();
        if (StateVerifyEnabled() && reuse) {
            if (!supported) {
                const bool was_supported = true;
                StateVerifyMismatch("pipeline", &was_supported, &supported, sizeof(supported));
            } else if (!(request == cached_graphics_request)) {
                StateVerifyMismatch("pipeline", &cached_graphics_request, &request,
                                    sizeof(request));
            }
        }
        if (!supported) {
            graphics_state_inputs.Invalidate();
            return nullptr;
        }
        // Verify mode always uses the freshly computed result, including on a mismatch.
        cached_graphics_request = request;
        cached_topology = topology;
        graphics_state_inputs.Store(inputs);
    } else {
        request = cached_graphics_request;
        topology = cached_topology;
        state = request.state;
    }
    // Diagnostic (NXBOX_D3D12_PSO_SKIP_TRACE=1): one line per pipeline request the first time a
    // draw is dropped for it, to see which draws a skip rule drops.
    const auto trace_skip = [&](std::string_view why, u64 request_hash) {
        static const bool enabled = BisectEnabled("NXBOX_D3D12_PSO_SKIP_TRACE");
        static std::unordered_set<u64> traced; // GPU thread only
        static const bool pso_trace = BisectEnabled("NXBOX_D3D12_PSO_TRACE");
        static std::unordered_set<u64> traced_rt_history;
        const bool history_reason = why == "rt_history";
        if (!(enabled || (history_reason && pso_trace)) ||
            !(history_reason ? traced_rt_history : traced).insert(request_hash).second)
            return;
        const auto& draw = maxwell3d->draw_manager.draw_state;
        const auto& rt0 = regs.rt[regs.rt_control.Map(0)];
        Diagnostic(fmt::format(
            "PSO_SKIP why={} request={:016x} indexed={} index_count={} vertex_count={} "
            "instances={} topology={} rts={} rt0={}x{} rt0_format={:x} rt0_addr={:x} depth={}",
            why, request_hash, is_indexed, draw.index_buffer.count, draw.vertex_buffer.count,
            draw.instance_count, static_cast<u32>(draw.topology),
            static_cast<u32>(regs.rt_control.count), rt0.width, rt0.height,
            static_cast<u32>(rt0.format), rt0.Address(), regs.zeta_enable != 0));
    };
    // A counter only: skip the per-draw hash lookup unless profiling.
    if (context.Profile().Enabled() && prewarmed_graphics.contains(request))
        context.Profile().Count(FrameCount::PrewarmedPsoHits);
    if (last_graphics_pipeline && request == last_graphics_request) {
        last_graphics_request_current = true;
        return last_graphics_pipeline;
    }
    if (const auto found = guest_graphics.find(request); found != guest_graphics.end()) {
        if (!found->second)
            context.Profile().Count(FrameCount::NegativeCacheHits);
        return RememberGraphicsPipeline(request, found->second);
    }
    if (AsyncPsoEnabled()) {
        try {
            if (!async)
                async = CreateAsyncCache();
            const u64 request_hash = Pso::HashBytes(&request, sizeof(request));
            if (CheckStalled(request, request_hash)) {
                StoreGuestGraphics(request, nullptr);
                return nullptr;
            }
            auto entry = async->Lookup(request);
            if (entry.status == AsyncCache::Status::Failed && entry.prewarmed &&
                async->ForgetFailedPrewarm(request))
                entry = async->Lookup(request);
            if (entry.status == AsyncCache::Status::Ready) {
                if (entry.prewarmed) {
                    if (disk)
                        disk->RememberFirstUse(RequestBytes(request));
                    prewarmed_graphics.insert(request);
                    context.Profile().Count(FrameCount::PrewarmedPsoHits);
                }
                StoreGuestGraphics(request, entry.result->graphics.get());
                if (!entry.prewarmed)
                    context.Profile().Count(FrameCount::PsoCompiles);
                return RememberGraphicsPipeline(request, entry.result->graphics.get());
            }
            if (entry.status == AsyncCache::Status::Failed) {
                Diagnostic(fmt::format("D3D12 async graphics request={:016x} failed={}",
                                       Pso::HashBytes(&request, sizeof(request)), entry.error));
                StoreGuestGraphics(request, nullptr);
                context.Profile().Count(FrameCount::NegativeCacheHits);
                return nullptr;
            }
            if (entry.status == AsyncCache::Status::Missing &&
                !CompileHeadroomAvailable(async->Idle())) {
                context.Profile().Count(FrameCount::DrawsSkippedPendingPso);
                trace_skip("headroom", request_hash);
                return nullptr;
            }
            // Opt in to transient missing geometry instead of first-use stalls. Retain
            // the bounded wait for small draws (SM3DW bakes ambient light at level load)
            // and stream output. Compute dispatches retain their existing wait too.
            // Read without BisectEnabled's process cache so the next game sees setting changes.
            char async_shaders_value[2]{};
            const bool skip_pending =
                GetEnvironmentVariableA("NXBOX_D3D12_ASYNC_SHADERS", async_shaders_value,
                                        sizeof(async_shaders_value)) == 1 &&
                async_shaders_value[0] == '1';
            const auto& draw = maxwell3d->draw_manager.draw_state;
            const u32 draw_count = (std::min)(draw.index_buffer.count, draw.vertex_buffer.count);
            const u32 drawn_count = is_indexed ? draw.index_buffer.count : draw.vertex_buffer.count;
            // Only draws that test depth: deciding from a bound depth buffer alone left Super Mario
            // Odyssey's coin counter undrawn for the rest of the run.
            const bool depth_tested = regs.zeta_enable != 0 && regs.depth_test_enable != 0;
            // NXBOX_D3D12_PSO_WAIT_DEPTH=1 restores the 1.0.5 rule for depth-tested draws.
            static const bool wait_depth = BisectEnabled("NXBOX_D3D12_PSO_WAIT_DEPTH");
            const bool old_may_skip = CanSkipPendingGraphicsPso(
                skip_pending, draw_count, drawn_count, !wait_depth && depth_tested,
                maxwell3d->regs.transform_feedback_enabled != 0);
            const auto last_rt_draw = RtHistoryEnabled() && formats.colors[0] != 0
                                          ? rt_draw_history.LastDrawFrame(
                                                regs.rt[regs.rt_control.Map(0)].Address(), frame)
                                          : std::nullopt;
            const bool history_skip =
                !old_may_skip && CanSkipPendingGraphicsPsoFromRtHistory(
                                     skip_pending, RtHistoryEnabled(),
                                     regs.transform_feedback_enabled != 0, frame, last_rt_draw);
            const bool may_skip = old_may_skip || history_skip;
            if (entry.status == AsyncCache::Status::Missing && async->CanSubmit(!may_skip)) {
                if (disk)
                    disk->RememberFirstUse(RequestBytes(request));
                GraphicsEnvironments live;
                GetGraphicsEnvironments(live, guest_hashes);
                auto snapshots =
                    std::make_shared<std::array<std::unique_ptr<Shader::Environment>, 6>>();
                Shader::Environment* texture_source{};
                for (size_t index = 0; index < guest_hashes.size(); ++index) {
                    if (!guest_hashes[index])
                        continue;
                    (*snapshots)[index] = live.envs[index].Snapshot(texture_source);
                    texture_source = (*snapshots)[index].get();
                }
                Microsoft::WRL::ComPtr<ID3D12Device> device{context.Device()};
                async->Submit(
                    request,
                    [device, disk_cache = disk.get(), state, d3d_topology = topology.topology,
                     guest_topology = static_cast<u32>(maxwell3d->draw_manager.draw_state.topology),
                     regs = maxwell3d->regs, formats, hashes = guest_hashes, snapshots, request,
                     profile_build = context.Profile().Enabled()] {
                        SetThreadDescription(GetCurrentThread(), L"D3D12 PSO compiler");
                        auto result = std::make_shared<AsyncResult>();
                        result->graphics = BuildAsyncGraphics(
                            device.Get(), disk_cache, state, d3d_topology, guest_topology, regs,
                            formats, hashes, *snapshots, request, profile_build);
                        return result;
                    },
                    false, !may_skip);
                context.Profile().Count(FrameCount::GuestPipelineMisses);
            }
            if (!may_skip && pso_waited.insert(request_hash).second) {
                const bool trace_wait =
                    context.Profile().Enabled() || BisectEnabled("NXBOX_D3D12_PSO_TRACE");
                const auto wait_start = trace_wait ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
                const auto settled = async->WaitFor(request, PendingPsoWaitBudget());
                // A request the full pool did not admit was never waited for; keep its one
                // wait for a frame where it is queued.
                if (settled.status == AsyncCache::Status::Missing)
                    pso_waited.erase(request_hash);
                if (trace_wait) {
                    const auto elapsed = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - wait_start)
                                             .count();
                    const auto& rt0 = regs.rt[regs.rt_control.Map(0)];
                    Diagnostic(fmt::format(
                        "PSO_WAIT request={:016x} kind=graphics wall_ms={:.3f} status={} thread={} "
                        "indexed={} count={} depth={} rt0={}x{} drawn={} depth_test={}",
                        request_hash, elapsed, static_cast<int>(settled.status),
                        GetCurrentThreadId(), is_indexed, draw_count, regs.zeta_enable != 0,
                        rt0.width, rt0.height, drawn_count, depth_tested));
                }
                if (settled.status == AsyncCache::Status::Ready) {
                    if (settled.prewarmed) {
                        if (disk)
                            disk->RememberFirstUse(RequestBytes(request));
                        prewarmed_graphics.insert(request);
                        context.Profile().Count(FrameCount::PrewarmedPsoHits);
                    } else {
                        context.Profile().Count(FrameCount::PsoCompiles);
                    }
                    StoreGuestGraphics(request, settled.result->graphics.get());
                    return settled.result->graphics.get();
                }
            }
            context.Profile().Count(FrameCount::DrawsSkippedPendingPso);
            trace_skip(history_skip ? "rt_history"
                       : may_skip   ? "pending"
                                    : "wait_timeout",
                       request_hash);
            return nullptr;
        } catch (const std::exception& error) {
            Diagnostic(fmt::format("D3D12 async graphics snapshot={:016x} failed={}",
                                   Pso::HashBytes(&request, sizeof(request)), error.what()));
            StoreGuestGraphics(request, nullptr);
            return nullptr;
        }
    }
    context.Profile().Count(FrameCount::GuestPipelineMisses);
    if (context.Profile().Enabled() &&
        (guest_graphics.size() < 16 || guest_graphics.size() % 120 == 0))
        Diagnostic(fmt::format(
            "D3D12_PIPELINE_MISS kind=graphics request={:016x} guest_entries={} pso_entries={}",
            Pso::HashBytes(&request, sizeof(request)), guest_graphics.size(), graphics.size()));
    try {
        GraphicsEnvironments environments;
        const FrameProfile::Scope profile_translation{context.Profile(),
                                                      FramePhase::ShaderTranslation};
        GetGraphicsEnvironments(environments, guest_hashes);
        TranslationState translation{context.Device()};
        std::array<Shader::IR::Program, 6> programs;
        std::array<std::string, 6> sources;
        std::array<Shader::RuntimeInfo, 6> runtimes;
        std::vector<StageInput> stages;
        for (size_t index = 0; index < programs.size(); ++index) {
            if (!guest_hashes[index])
                continue;
            auto& env = environments.envs[index];
            Shader::Maxwell::Flow::CFG cfg{
                env, translation.flow,
                static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader)), index == 0};
            context.Profile().Count(FrameCount::ShaderTranslations);
            programs[index] = Shader::Maxwell::TranslateProgram(translation.inst, translation.block,
                                                                env, cfg, translation.host);
            if (index == 1 && guest_hashes[0]) {
                programs[index] =
                    Shader::Maxwell::MergeDualVertexPrograms(programs[0], programs[1], env);
            }
        }
        Shader::Backend::Bindings bindings;
        const Shader::IR::Program* previous{};
        for (size_t index = guest_hashes[0] && guest_hashes[1] ? 1 : 0; index < 6; ++index) {
            if (!guest_hashes[index])
                continue;
            auto& program = programs[index];
            if (index == 0)
                program.stage = Shader::Stage::VertexB;
            if (program.stage == Shader::Stage::Fragment) {
                // Guest shaders may export more colors than this framebuffer binds.
                // Specialize the DXIL signature together with the PSO attachment key.
                for (size_t slot = 0; slot < formats.colors.size(); ++slot)
                    program.info.stores_frag_color[slot] &= formats.colors[slot] != 0;
            }
            const auto& runtime = runtimes[index] =
                GraphicsRuntime(maxwell3d->regs, program, previous,
                                static_cast<u32>(maxwell3d->draw_manager.draw_state.topology));
            Shader::Maxwell::ConvertLegacyToGeneric(program, runtime);
            if (program.stage == Shader::Stage::Fragment &&
                maxwell3d->draw_manager.draw_state.topology == Maxwell::PrimitiveTopology::Quads &&
                regs.provoking_vertex == Maxwell::ProvokingVertex::Last) {
                for (size_t attribute = 0; attribute < program.info.interpolation.size();
                     ++attribute) {
                    if (program.info.loads.Generic(attribute) &&
                        program.info.interpolation[attribute] == Shader::Interpolation::Flat) {
                        ReportUnsupported("provoking_vertex=Last for flat quads requires "
                                          "flat-value replication without changing diagonal");
                        StoreGuestGraphics(request, nullptr);
                        return nullptr;
                    }
                }
            }
            sources[index] =
                Shader::Backend::HLSL::EmitHLSL(translation.profile, runtime, program, bindings);
            ApplyTextureProbe(program.stage, sources[index]);
            DumpGraphicsHlsl(guest_hashes[index], program.stage, sources[index]);
            const auto counts = CountStageTables(program.info);
            stages.push_back({program.stage,
                              guest_hashes[index],
                              sources[index],
                              {counts.cbv, counts.srv, counts.uav, counts.sampler},
                              &runtime});
            previous = &program;
        }
        std::uint32_t vertex_reads = 0;
        const auto& vertex_info = programs[guest_hashes[1] ? 1 : 0].info;
        for (std::size_t i = 0; i < Pso::kNumVertexAttributes; ++i)
            if (vertex_info.loads.Generic(i))
                vertex_reads |= std::uint32_t{1} << i;
        for (size_t index = 0; index < state.attributes.size(); ++index) {
            if (!(vertex_reads & (u32{1} << index)) || regs.vertex_attrib_format[index].constant)
                continue;
            const auto& input = regs.vertex_attrib_format[index];
            const auto& attribute = state.attributes[index];
            const u32 size = static_cast<u32>(input.size.Value());
            const bool component_conversion = attribute.swap_rb || attribute.scaled;
            if (!attribute.dxgi || (attribute.swap_rb && size == Pso::VSize::B10G11R11)) {
                ReportUnsupported(
                    fmt::format("vertex_attrib_format[{}] size={:x} type={} bgra={} "
                                "requires vertex pulling/component conversion native_conversion={}",
                                index, size, static_cast<u32>(input.type.Value()), input.bgra != 0,
                                component_conversion));
                StoreGuestGraphics(request, nullptr);
                return nullptr;
            }
        }
        Pso::ApplyVertexReads(state, vertex_reads);
        const FrameProfile::Scope profile_build{context.Profile(), FramePhase::PipelineLookup};
        DiskBytes metadata;
        for (size_t index = 0; index < NUM_GRAPHICS_STAGES; ++index) {
            const size_t source = index == 0 && !guest_hashes[1] ? 0 : index + 1;
            Append32(metadata, guest_hashes[source] ? 1u : 0u);
            if (guest_hashes[source])
                WriteInfo(metadata, programs[source].info);
        }
        const Shader::RuntimeInfo* feedback{};
        for (const auto& stage : stages)
            if (stage.runtime && stage.runtime->xfb_count)
                feedback = stage.runtime;
        WriteFeedback(metadata, feedback);
        auto& pipeline = GetGraphics(state, topology.topology, stages, &request, metadata);
        for (size_t index = 1; index < 6; ++index) {
            if (guest_hashes[index])
                pipeline.SetStageInfo(index - 1, &programs[index].info);
        }
        if (guest_hashes[0] && !guest_hashes[1])
            pipeline.SetStageInfo(0, &programs[0].info);
        StoreGuestGraphics(request, &pipeline);
        return RememberGraphicsPipeline(request, &pipeline);
    } catch (const Shader::Exception& error) {
        ReportUnsupported(error.what());
    } catch (const std::exception& error) {
        Diagnostic(fmt::format("D3D12 graphics pipeline={:016x} what={}",
                               Pso::HashBytes(&request, sizeof(request)), error.what()));
        if (FAILED(context.Device()->GetDeviceRemovedReason()))
            context.ReportFailure(error);
    }
    StoreGuestGraphics(request, nullptr);
    return nullptr;
} catch (const std::exception& error) {
    ReportUnsupported(error.what());
    return nullptr;
} catch (...) {
    ReportUnsupported("unknown exception in graphics pipeline state");
    return nullptr;
}

ComputePipeline* PipelineCache::CurrentComputePipeline() try {
    const FrameProfile::Scope profile_lookup{context.Profile(), FramePhase::PipelineLookup};
    if (!kepler_compute)
        return nullptr;
    std::scoped_lock lock{pipeline_mutex};
    SyncGuestHost();
    const auto* shader = ComputeShader();
    if (!shader)
        return nullptr;
    const auto& launch = kepler_compute->launch_description;
    if (VideoCommon::ShaderDiscovery::Enabled()) {
        VideoCommon::ShaderDiscovery::Base(gpu_memory->GetID(),
                                           kepler_compute->regs.code_loc.Address(), true);
        VideoCommon::ShaderDiscovery::Demand(
            gpu_memory->GetID(), kepler_compute->regs.code_loc.Address() + launch.program_start,
            shader->unique_hash, 0, true);
    }
    const std::array<u32, 6> specialization{launch.block_dim_x,     launch.block_dim_y,
                                            launch.block_dim_z,     launch.shared_alloc,
                                            launch.local_pos_alloc, launch.local_crs_alloc};
    const u64 request =
        Pso::HashBytes(specialization.data(), sizeof(specialization), shader->unique_hash);
    VideoCommon::SubmissionTrace::Log("compute_shader guest_hash={:016x} request={:016x}",
                                      shader->unique_hash, request);
    // A counter only: skip the per-draw hash lookup unless profiling.
    if (context.Profile().Enabled() && prewarmed_compute.contains(request))
        context.Profile().Count(FrameCount::PrewarmedPsoHits);
    if (const auto found = guest_compute.find(request); found != guest_compute.end()) {
        if (!found->second)
            context.Profile().Count(FrameCount::NegativeCacheHits);
        return found->second;
    }
    if (AsyncPsoEnabled()) {
        try {
            if (!async)
                async = CreateAsyncCache();
            GraphicsPipelineKey async_key{};
            async_key.stage_hashes[0] = request;
            async_key.state.sample_count =
                0; // Graphics requests always have a nonzero sample count.
            if (CheckStalled(async_key, request)) {
                InvalidateLastGraphicsPipeline();
                guest_compute.emplace(request, nullptr);
                return nullptr;
            }
            auto entry = async->Lookup(async_key);
            if (entry.status == AsyncCache::Status::Failed && entry.prewarmed &&
                async->ForgetFailedPrewarm(async_key))
                entry = async->Lookup(async_key);
            if (entry.status == AsyncCache::Status::Ready) {
                if (entry.prewarmed) {
                    if (disk)
                        disk->RememberFirstUse(RequestBytes(async_key));
                    prewarmed_compute.insert(request);
                    context.Profile().Count(FrameCount::PrewarmedPsoHits);
                }
                InvalidateLastGraphicsPipeline();
                guest_compute.emplace(request, entry.result->compute.get());
                if (!entry.prewarmed)
                    context.Profile().Count(FrameCount::PsoCompiles);
                return entry.result->compute.get();
            }
            if (entry.status == AsyncCache::Status::Failed) {
                Diagnostic(fmt::format("D3D12 async compute request={:016x} failed={}", request,
                                       entry.error));
                InvalidateLastGraphicsPipeline();
                guest_compute.emplace(request, nullptr);
                context.Profile().Count(FrameCount::NegativeCacheHits);
                return nullptr;
            }
            if (entry.status == AsyncCache::Status::Missing &&
                !CompileHeadroomAvailable(async->Idle())) {
                context.Profile().Count(FrameCount::DispatchesSkippedPendingPso);
                return nullptr;
            }
            if (entry.status == AsyncCache::Status::Missing && async->CanSubmit(true)) {
                VideoCommon::ComputeEnvironment live{*kepler_compute, *gpu_memory,
                                                     kepler_compute->regs.code_loc.Address(),
                                                     launch.program_start};
                live.SetCachedSize(shader->size_bytes);
                std::shared_ptr<Shader::Environment> snapshot{live.Snapshot()};
                Microsoft::WRL::ComPtr<ID3D12Device> device{context.Device()};
                std::shared_ptr<ComputeTranslation> prepared;
                const bool skip_global_fallback = SkipGlobalFallbackComputeEnabled();
                if (skip_global_fallback && !FrameBoundedPsoEnabled()) {
                    try {
                        prepared = std::make_shared<ComputeTranslation>(device.Get(), *snapshot);
                    } catch (...) {
                        // Translation now runs here, not in the worker; without a Failed entry
                        // the key stays Missing and every dispatch would translate it again.
                        async->Reject(async_key, "Compute translation failed");
                        throw;
                    }
                    if (SkipGlobalFallbackCompute(true, true,
                                                  prepared->program.info.uses_global_memory)) {
                        async->Reject(async_key, "Global memory fallback");
                        ReportGlobalFallbackSkip(request);
                        InvalidateLastGraphicsPipeline();
                        guest_compute.emplace(request, nullptr);
                        return nullptr;
                    }
                }
                if (disk)
                    disk->RememberFirstUse(RequestBytes(async_key));
                auto job = [device, disk_cache = disk.get(), snapshot, prepared,
                            skip_global_fallback, shader_hash = shader->unique_hash, request,
                            profile_build = context.Profile().Enabled()] {
                    SetThreadDescription(GetCurrentThread(), L"D3D12 PSO compiler");
                    static const bool trace = BisectEnabled("NXBOX_D3D12_PSO_TRACE");
                    PsoBuildStats build_stats;
                    std::optional<ScopedPsoBuildStats> collect;
                    if (trace || profile_build)
                        collect.emplace(build_stats);
                    const auto trace_start = trace || profile_build
                                                 ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
                    const auto mark = [&](std::string_view phase) {
                        if (trace)
                            Diagnostic(fmt::format(
                                "PSO_TRACE compute={:016x} {} wall_ms={:.3f}", request, phase,
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - trace_start)
                                    .count()));
                    };
                    mark("start");
                    auto translated =
                        prepared ? prepared
                                 : std::make_shared<ComputeTranslation>(device.Get(), *snapshot);
                    auto& translation = translated->translation;
                    auto& program = translated->program;
                    mark("translated");
                    if (SkipGlobalFallbackCompute(skip_global_fallback, true,
                                                  program.info.uses_global_memory)) {
                        ReportGlobalFallbackSkip(request);
                        // The pool publishes the same stable failure as foreground rejection.
                        throw std::runtime_error("Global memory fallback");
                    }
                    const auto source =
                        Shader::Backend::HLSL::EmitHLSL(translation.profile, program);
                    DumpGraphicsHlsl(shader_hash, Shader::Stage::Compute, source);
                    mark(fmt::format("dxc bytes={}", source.size()));
                    if (trace && source.size() > 100'000) {
                        // Preserve large shader sources for compiler-cost investigations.
                        std::ofstream{Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                                          fmt::format("compute_{:016x}.hlsl", request),
                                      std::ios::binary}
                            << source;
                    }
                    const auto counts = CountStageTables(program.info);
                    const StageInput stage{Shader::Stage::Compute,
                                           shader_hash,
                                           source,
                                           {counts.cbv, counts.srv, counts.uav, counts.sampler}};
                    const auto translation_end = trace || profile_build
                                                     ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                    thread_local DxcCache compiler;
                    auto dxil =
                        compiler.Compile(source, Shader::Stage::Compute, request, disk_cache);
                    auto result = std::make_shared<AsyncResult>();
                    const auto native_start = std::chrono::steady_clock::now();
                    mark("native_pso");
                    const auto recipe = Recipe(nullptr, 0, std::span{&stage, size_t{1}});
                    const u64 native_key = Pso::HashBytes(recipe.data(), recipe.size(), request);
                    result->compute = std::make_shared<ComputePipeline>(
                        device.Get(),
                        CreateRootSignature(device.Get(), stage.counts,
                                            UseRootCbvs(stage.counts.cbv)),
                        dxil, disk_cache, native_key, recipe);
                    result->compute->SetRootCbvs(UseRootCbvs(stage.counts.cbv));
                    const StageProgram compiled{Shader::Stage::Compute, dxil};
                    ReportSlowPso(native_key, native_start, result->compute->RootSignature(),
                                  stage.counts, std::span{&stage, size_t{1}},
                                  std::span{&compiled, size_t{1}});
                    result->compute->info = program.info;
                    if (disk_cache) {
                        GraphicsPipelineKey key{};
                        key.stage_hashes[0] = request;
                        key.state.sample_count = 0; // Same key as CurrentComputePipeline.
                        DiskBytes metadata;
                        WriteInfo(metadata, program.info);
                        if (!disk_cache->RememberPipeline({DiskRecordKind::Compute, native_key,
                                                           recipe, RequestBytes(key),
                                                           std::move(metadata)}))
                            Diagnostic(fmt::format("D3D12 PSO recipe write failed request={:016x}",
                                                   request));
                    }
                    mark("done");
                    if (trace || profile_build)
                        Diagnostic(fmt::format(
                            "PSO_BUILD_COST request={:016x} kind=compute driver_builds={} "
                            "translation_emit_ms={:.3f} dxc_ms={:.3f} root_signature_ms={:.3f} "
                            "driver_ms={:.3f} wall_ms={:.3f} dxil_disk_hits={} dxil_disk_misses={} "
                            "dxil_memory_hits={}",
                            request, build_stats.driver_builds,
                            std::chrono::duration<double, std::milli>(translation_end - trace_start)
                                .count(),
                            build_stats.dxc_us / 1000.0, build_stats.root_us / 1000.0,
                            build_stats.driver_us / 1000.0,
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - trace_start)
                                .count(),
                            build_stats.disk_hits, build_stats.disk_misses,
                            build_stats.memory_hits));
                    return result;
                };
                // Dispatches always get their one wait, so they are always queued.
                async->Submit(async_key, std::move(job), false, true);
                context.Profile().Count(FrameCount::GuestPipelineMisses);
            }
            if (pso_waited.insert(request).second) {
                const bool trace_wait =
                    context.Profile().Enabled() || BisectEnabled("NXBOX_D3D12_PSO_TRACE");
                const auto wait_start = trace_wait ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
                const auto settled = async->WaitFor(async_key, PendingPsoWaitBudget());
                // A request the full pool did not admit was never waited for; keep its one
                // wait for a frame where it is queued.
                if (settled.status == AsyncCache::Status::Missing)
                    pso_waited.erase(request);
                if (trace_wait) {
                    const auto elapsed = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - wait_start)
                                             .count();
                    Diagnostic(fmt::format(
                        "PSO_WAIT request={:016x} kind=compute wall_ms={:.3f} status={} thread={}",
                        request, elapsed, static_cast<int>(settled.status), GetCurrentThreadId()));
                }
                if (settled.status == AsyncCache::Status::Ready) {
                    if (settled.prewarmed) {
                        if (disk)
                            disk->RememberFirstUse(RequestBytes(async_key));
                        prewarmed_compute.insert(request);
                        context.Profile().Count(FrameCount::PrewarmedPsoHits);
                    } else {
                        context.Profile().Count(FrameCount::PsoCompiles);
                    }
                    guest_compute.emplace(request, settled.result->compute.get());
                    return settled.result->compute.get();
                }
            }
            context.Profile().Count(FrameCount::DispatchesSkippedPendingPso);
            return nullptr;
        } catch (const std::exception& error) {
            Diagnostic(fmt::format("D3D12 async compute snapshot={:016x} failed={}", request,
                                   error.what()));
            InvalidateLastGraphicsPipeline();
            guest_compute.emplace(request, nullptr);
            return nullptr;
        }
    }
    if (disk) {
        GraphicsPipelineKey first_use_key{};
        first_use_key.stage_hashes[0] = request;
        disk->RememberFirstUse(RequestBytes(first_use_key));
    }
    context.Profile().Count(FrameCount::GuestPipelineMisses);
    try {
        const auto& qmd = kepler_compute->launch_description;
        VideoCommon::ComputeEnvironment env{*kepler_compute, *gpu_memory,
                                            kepler_compute->regs.code_loc.Address(),
                                            qmd.program_start};
        env.SetCachedSize(shader->size_bytes);
        const FrameProfile::Scope profile_translation{context.Profile(),
                                                      FramePhase::ShaderTranslation};
        context.Profile().Count(FrameCount::ShaderTranslations);
        TranslationState translation{context.Device()};
        Shader::Maxwell::Flow::CFG cfg{env, translation.flow, env.StartAddress()};
        auto program = Shader::Maxwell::TranslateProgram(translation.inst, translation.block, env,
                                                         cfg, translation.host);
        if (SkipGlobalFallbackCompute(SkipGlobalFallbackComputeEnabled(), true,
                                      program.info.uses_global_memory)) {
            ReportGlobalFallbackSkip(request);
            InvalidateLastGraphicsPipeline();
            guest_compute.emplace(request, nullptr);
            return nullptr;
        }
        const auto source = Shader::Backend::HLSL::EmitHLSL(translation.profile, program);
        const auto counts = CountStageTables(program.info);
        const FrameProfile::Scope profile_build{context.Profile(), FramePhase::PipelineLookup};
        GraphicsPipelineKey key{};
        key.stage_hashes[0] = request;
        key.state.sample_count = 0; // Same key as CurrentComputePipeline.
        DiskBytes metadata;
        WriteInfo(metadata, program.info);
        auto& pipeline = GetCompute({Shader::Stage::Compute,
                                     shader->unique_hash,
                                     source,
                                     {counts.cbv, counts.srv, counts.uav, counts.sampler}},
                                    &key, metadata);
        pipeline.info = program.info;
        InvalidateLastGraphicsPipeline();
        guest_compute.emplace(request, &pipeline);
        return &pipeline;
    } catch (const std::exception& error) {
        Diagnostic(
            fmt::format("D3D12 compute HLSL/PSO pipeline={:016x} what={}", request, error.what()));
        if (FAILED(context.Device()->GetDeviceRemovedReason()))
            context.ReportFailure(error);
        InvalidateLastGraphicsPipeline();
        guest_compute.emplace(request, nullptr);
        return nullptr;
    }
} catch (const std::exception& error) {
    ReportUnsupported(error.what());
    return nullptr;
} catch (...) {
    ReportUnsupported("unknown exception in compute pipeline state");
    return nullptr;
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback,
                                      bool wait_for_warmup,
                                      const std::function<bool()>& skip_wait) {
    if (disk)
        disk->StopWarmup();
    const size_t detached = async ? async->Stop() : 0;
    if (disk) {
        disk->Flush();
        if (detached)
            (void)disk.release(); // A driver call may still be using the old title's disk cache.
    }
    std::unique_lock lock{pipeline_mutex};
    context.Finish(); // Retire PSOs referenced by the previous title before replacing the maps.
    InvalidateLastGraphicsPipeline();
    async.reset();
    guest_graphics.clear();
    rt_draw_history = {};
    prewarmed_graphics.clear();
    prewarmed_compute.clear();
    pso_waited.clear();
    stalled_requests.clear();
    guest_compute.clear();
    graphics.clear();
    compute.clear();
    // RescalingPass bakes the scale into shader instructions. Never prewarm another
    // resolution's serialized HLSL/PSO under the same guest shader hash. 1x keeps the old
    // identity so existing caches survive this update.
    const auto resolution = Settings::values.resolution_setup.GetValue();
    const u32 emitter_identity =
        resolution == Settings::ResolutionSetup::Res1X ? 4u
                                                       : 5u + (static_cast<u32>(resolution) << 16);
    const auto identity = DiskPipelineCache::QueryIdentity(
        context.Device(), {emitter_identity, 2, PsoNormalizeEnabled() ? 7u : 6u});
    if (!identity) {
        LOG_WARNING(Render, "D3D12 adapter identity unavailable; disk PSO reuse disabled");
        disk.reset();
        if (callback)
            callback(VideoCore::LoadCallbackStage::Complete, 0, 0);
        return;
    }
    // ShaderDir is under the frontend's LocalState path on UWP.
    disk = std::make_unique<DiskPipelineCache>(
        Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir), title_id, *identity,
        context.Device());
    Diagnostic(
        fmt::format("D3D12_PSO_CACHE path={} runtime_threads={} frame_budget={} explicit_async={}",
                    (Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) / "d3d12_cache" /
                     fmt::format("{:016x}", title_id))
                        .string(),
                    RuntimePsoThreads(), FrameBoundedPsoEnabled(), NonBlockingPsoEnabled()));
    stalled_path =
        Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
        fmt::format("{:016x}_d3d12_stalled_pso_v{}.txt", title_id, PsoNormalizeEnabled() ? 3 : 2);
    {
        std::ifstream file{stalled_path};
        std::string line;
        while (std::getline(file, line))
            if (!line.empty())
                stalled_requests.insert(std::strtoull(line.c_str(), nullptr, 16));
    }
    auto legacy_stalled = std::make_shared<std::unordered_set<u64>>();
    if (PsoNormalizeEnabled()) {
        const auto old_path = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
                              fmt::format("{:016x}_d3d12_stalled_pso_v2.txt", title_id);
        std::ifstream file{old_path};
        u64 hash{};
        while (file >> std::hex >> hash)
            legacy_stalled->insert(hash);
    }
    if (!stalled_requests.empty())
        Diagnostic(fmt::format("D3D12 skipping {} PSOs that stalled the driver before",
                               stalled_requests.size()));
    const bool async_warmup = AsyncPsoEnabled();
    if (BisectEnabled("NXBOX_D3D12_NO_PREWARM")) {
        if (callback)
            callback(VideoCore::LoadCallbackStage::Complete, 0, 0);
        return;
    }
    if (async_warmup && !async)
        async = CreateAsyncCache();
    // Background callbacks own only this state, never frontend loading-scope captures.
    struct PreloadProgress {
        std::mutex mutex;
        DiskWarmupProgress progress{};
        std::vector<GraphicsPipelineKey> requests;
        size_t published_requests{};
        bool finished{};
    };
    const auto preload =
        async_warmup && wait_for_warmup ? std::make_shared<PreloadProgress>() : nullptr;
    struct WarmupStats {
        std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
        std::atomic<size_t> skipped{};
        PrewarmPhases phases;
    };
    const auto stats = std::make_shared<WarmupStats>();
    const size_t warmup_threads =
        async_warmup ? (preload ? PrewarmThreads() : RuntimePsoThreads()) : 1;
    const auto report = [stats, warmup_threads](std::string_view status) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - stats->start)
                            .count();
        const auto& p = stats->phases;
        Diagnostic(fmt::format(
            "D3D12_PSO_PREWARM status={} built={} skipped={} failed={} ms={} threads={} "
            "dxil_disk_hits={} dxil_disk_misses={} dxil_memory_hits={} "
            "dxc_ms={:.3f} root_signature_ms={:.3f} driver_ms={:.3f} driver_builds={}",
            status, p.built.load(), stats->skipped.load(), p.failed.load(), ms, warmup_threads,
            p.dxil_disk_hits.load(), p.dxil_disk_misses.load(), p.dxil_memory_hits.load(),
            p.dxc_us.load() / 1000.0, p.root_us.load() / 1000.0, p.driver_us.load() / 1000.0,
            p.driver_builds.load()));
    };
    // Keep the raised target until submitted jobs settle, not just until disk enumeration ends.
    // Scope exit also restores it on cancellation, skip-wait and callback/startup exceptions.
    SCOPE_EXIT {
        if (preload)
            async->SetWorkerTarget(RuntimePsoThreads());
    };
    if (preload) {
        const size_t threads = PrewarmThreads();
        Diagnostic(fmt::format("D3D12 prewarm threads={}", threads));
        async->SetWorkerTarget(threads);
    }
    disk->StartWarmup(
        [this, async_warmup, stats, preload, legacy_stalled](const DiskPipelineRecord& record,
                                                             std::stop_token stop) {
            if (stop.stop_requested())
                return false;
            try {
                if (async_warmup) {
                    if (record.request_key.size() != sizeof(GraphicsPipelineKey) ||
                        record.runtime_data.empty()) {
                        ++stats->skipped;
                        return true;
                    }
                    GraphicsPipelineKey request{};
                    std::memcpy(&request, record.request_key.data(), sizeof(request));
                    const u64 hash = record.kind == DiskRecordKind::Graphics
                                         ? Pso::HashBytes(&request, sizeof(request))
                                         : request.stage_hashes[0];
                    {
                        std::scoped_lock guard{pipeline_mutex};
                        const auto old_hash = record.kind == DiskRecordKind::Graphics
                                                  ? record.legacy_request_hash
                                                  : hash;
                        if (legacy_stalled->contains(old_hash) &&
                            (record.legacy || record.kind == DiskRecordKind::Compute) &&
                            stalled_requests.insert(hash).second) {
                            std::ofstream{stalled_path, std::ios::app}
                                << fmt::format("{:016x}\n", hash);
                        }
                        if (stalled_requests.contains(hash)) {
                            ++stats->skipped;
                            return true;
                        }
                    }
                    if (record.kind == DiskRecordKind::Compute &&
                        SkipGlobalFallbackComputeEnabled()) {
                        const auto decoded = ReadRecipe(record);
                        // The emitter writes this helper iff info.uses_global_memory is true.
                        const bool global = std::any_of(
                            decoded.stages.begin(), decoded.stages.end(), [](const auto& stage) {
                                return stage.hlsl.find("nx_global_load32(") != std::string::npos;
                            });
                        if (SkipGlobalFallbackCompute(true, true, global)) {
                            async->Reject(request, "Global memory fallback");
                            ReportGlobalFallbackSkip(hash);
                            ++stats->skipped;
                            return true;
                        }
                    }
                    // Admission and foreground submission use the same atomic key insertion.
                    // A full pool makes this coordinator retry, never the loading/GPU thread.
                    for (;;) {
                        if (stop.stop_requested())
                            return false;
                        const auto entry = async->Lookup(request);
                        if (entry.status != AsyncCache::Status::Missing) {
                            if (record.legacy)
                                disk->DiscardLegacyDuplicate(record);
                            if (preload) {
                                std::scoped_lock guard{preload->mutex};
                                preload->requests.push_back(request);
                            }
                            ++stats->skipped;
                            return true;
                        }
                        // A deferred warmup key never starves a draw: the foreground compiles it.
                        if (!CompileHeadroomAvailable(false)) {
                            ++stats->skipped;
                            return true;
                        }
                        Microsoft::WRL::ComPtr<ID3D12Device> device{context.Device()};
                        if (async->Submit(
                                request,
                                [device, disk_cache = disk.get(), record, stop, stats] {
                                    SetThreadDescription(GetCurrentThread(), L"D3D12 PSO compiler");
                                    if (stop.stop_requested())
                                        return std::shared_ptr<AsyncResult>{};
                                    auto result = std::make_shared<AsyncResult>();
                                    BuildPrewarmedPipeline(device.Get(), disk_cache, record,
                                                           result->graphics, result->compute,
                                                           stats->phases);
                                    return result;
                                },
                                true))
                            break;
                        // Wait only on this disk coordinator, never on the loading/GPU thread.
                        std::this_thread::sleep_for(std::chrono::milliseconds{10});
                    }
                    if (preload) {
                        std::scoped_lock guard{preload->mutex};
                        preload->requests.push_back(request);
                    }
                    // Do not wait for the build: the pool's admission limit keeps the workers
                    // busy in parallel, and runtime lookups pick each PSO up when it is ready.
                    return true;
                }
                PsoBuildStats build_stats;
                const ScopedPsoBuildStats collect{build_stats};
                bool success = false;
                SCOPE_EXIT {
                    auto& p = stats->phases;
                    p.dxil_disk_hits += build_stats.disk_hits;
                    p.dxil_disk_misses += build_stats.disk_misses;
                    p.dxil_memory_hits += build_stats.memory_hits;
                    p.dxc_us += build_stats.dxc_us;
                    p.root_us += build_stats.root_us;
                    p.driver_us += build_stats.driver_us;
                    p.driver_builds += build_stats.driver_builds;
                    (success ? p.built : p.failed).fetch_add(1);
                };
                const auto decoded = ReadRecipe(record);
                const auto& stages = decoded.stages;
                if (SkipGlobalFallbackCompute(
                        SkipGlobalFallbackComputeEnabled(), record.kind == DiskRecordKind::Compute,
                        std::any_of(stages.begin(), stages.end(), [](const auto& stage) {
                            return stage.hlsl.find("nx_global_load32(") != std::string::npos;
                        }))) {
                    GraphicsPipelineKey request{};
                    if (record.request_key.size() == sizeof(request)) {
                        std::memcpy(&request, record.request_key.data(), sizeof(request));
                        ReportGlobalFallbackSkip(request.stage_hashes[0]);
                    }
                    return true;
                }
                if (record.kind == DiskRecordKind::Graphics)
                    GetGraphics(decoded.state, decoded.topology, stages);
                else if (stages.size() == 1 && stages[0].stage == Shader::Stage::Compute)
                    GetCompute(stages[0]);
                else
                    return false;
                disk->RememberPipeline(record);
                success = true;
                return true;
            } catch (const std::exception& error) {
                LOG_WARNING(Render, "D3D12 disk PSO warmup rejected: {}", error.what());
                return false;
            }
        },
        [callback = async_warmup ? VideoCore::DiskResourceLoadCallback{} : callback, stats, preload,
         report,
         warmup_pool = preload ? async.get() : nullptr](const DiskWarmupProgress& progress) {
            if (warmup_pool && (progress.stage == DiskWarmupStage::Cancelled ||
                                progress.stage == DiskWarmupStage::Failed))
                warmup_pool->SetWorkerTarget(RuntimePsoThreads());
            if (preload) {
                std::scoped_lock guard{preload->mutex};
                preload->progress = progress;
                preload->published_requests = preload->requests.size();
                preload->finished = progress.stage != DiskWarmupStage::Begin &&
                                    progress.stage != DiskWarmupStage::Build;
            }
            if (progress.stage == DiskWarmupStage::Begin) {
                Diagnostic(fmt::format("D3D12_PSO_PREWARM start records={}", progress.total));
            } else if (progress.stage != DiskWarmupStage::Build) {
                // Enumeration completion is not async worker completion.
                if (!preload)
                    report("enumerated");
            }
            if (callback)
                callback(progress.stage == DiskWarmupStage::Complete
                             ? VideoCore::LoadCallbackStage::Complete
                             : VideoCore::LoadCallbackStage::Build,
                         progress.completed, progress.total);
        },
        stop_loading);
    lock.unlock();
    if (preload) {
        for (;;) {
            // Poll independently of job completions, including slow or stuck driver calls.
            // B releases only the loading thread; closing still cancels the coordinator.
            if (stop_loading.stop_requested() || (skip_wait && skip_wait())) {
                report("interrupted");
                return;
            }
            DiskWarmupProgress progress;
            std::vector<GraphicsPipelineKey> requests;
            bool finished;
            {
                std::scoped_lock guard{preload->mutex};
                progress = preload->progress;
                requests.assign(preload->requests.begin(),
                                preload->requests.begin() + preload->published_requests);
                finished = preload->finished;
            }
            size_t pending = 0;
            for (const auto& request : requests)
                if (async->Lookup(request).status == AsyncCache::Status::Pending)
                    ++pending;
            // Rejected/skipped records count as processed; submitted jobs count only when settled.
            const size_t built = progress.completed - std::min(progress.completed, pending);
            if (callback)
                callback(VideoCore::LoadCallbackStage::Build, built, progress.total);
            if (finished && pending == 0) {
                disk->WaitWarmup();
                report("complete");
                async->SetWorkerTarget(RuntimePsoThreads());
                if (callback)
                    callback(VideoCore::LoadCallbackStage::Complete, built, progress.total);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    } else if (async_warmup) {
        // No background callback may retain the frontend's loading-scope captures.
        if (callback)
            callback(VideoCore::LoadCallbackStage::Complete, 0, 0);
    } else {
        // The synchronous frontend callback captures loading-scope state.
        disk->WaitWarmup();
    }
}
void PipelineCache::BindToChannel(s32 id) {
    std::scoped_lock lock{pipeline_mutex};
    graphics_state_inputs.Invalidate();
    InvalidateLastGraphicsPipeline();
    VideoCommon::ShaderCache::BindToChannel(id);
    if (maxwell3d)
        maxwell3d->dirty.flags[VideoCommon::Dirty::Shaders] = true;
}
bool PipelineCache::CheckStalled(const GraphicsPipelineKey& key, u64 hash) {
    if (stalled_requests.contains(hash))
        return true;
    // The pool already replaces workers stuck past its 5 s limit. Skipping a request for good
    // needs far longer: DXC alone takes ~5 s on the Xbox for a 100 KB vertex shader that then
    // finishes fine, and a false positive here hides that geometry in every later boot.
    if (!async || !async->IsStalled(key, std::chrono::seconds{60}))
        return false;
    stalled_requests.insert(hash);
    Diagnostic(fmt::format("D3D12 PSO request={:016x} stalled the driver; skipped from now on",
                           hash));
    if (!stalled_path.empty())
        std::ofstream{stalled_path, std::ios::app} << fmt::format("{:016x}\n", hash);
    return true;
}
PipelineCache::~PipelineCache() {
    try {
        size_t detached = 0;
        if (disk)
            disk->StopWarmup();
        if (async) {
            detached = async->Stop();
            context.Finish();
            async.reset();
        }
        if (disk) {
            disk->StopWarmup();
            disk->Flush();
            // A detached compile that ever returns still writes through this pointer.
            if (detached)
                (void)disk.release();
        }
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12 pipeline cache teardown: {}", error.what());
    } catch (...) {
        LOG_ERROR(Render, "D3D12 pipeline cache teardown: unknown exception");
    }
}
void PipelineCache::FlushDiskCache() {
    if (disk)
        disk->Flush();
}

} // namespace D3D12
