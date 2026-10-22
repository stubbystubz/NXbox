// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <bit>
#include "common/nxbox_seam_diagnostics.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/backend/hlsl/hlsl_helpers.h"
#include "shader_recompiler/backend/hlsl/hlsl_source.h"
#include "shader_recompiler/backend/hlsl/hlsl_vertex_semantic.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::HLSL {
namespace {
std::string TextureClass(TextureType type, bool writable, bool multisample = false) {
    const std::string prefix = writable ? "RW" : "";
    switch (type) {
    case TextureType::Buffer:
        return writable ? "RWBuffer" : "Buffer";
    case TextureType::Color1D:
        return prefix + "Texture1D";
    case TextureType::ColorArray1D:
        return prefix + "Texture1DArray";
    case TextureType::Color2D:
    case TextureType::Color2DRect:
        return prefix + (multisample ? "Texture2DMS" : "Texture2D");
    case TextureType::ColorArray2D:
        return prefix + (multisample ? "Texture2DMSArray" : "Texture2DArray");
    case TextureType::Color3D:
        return prefix + "Texture3D";
    case TextureType::ColorCube:
        if (!writable)
            return "TextureCube";
        break;
    case TextureType::ColorArrayCube:
        if (!writable)
            return "TextureCubeArray";
        break;
    }
    throw NotImplementedException("HLSL resource type {}", static_cast<u32>(type));
}
const char* StageName(Stage stage) {
    switch (stage) {
    case Stage::VertexA:
        return "VertexA";
    case Stage::VertexB:
        return "VertexB";
    case Stage::TessellationControl:
        return "TessellationControl";
    case Stage::TessellationEval:
        return "TessellationEval";
    case Stage::Geometry:
        return "Geometry";
    case Stage::Fragment:
        return "Fragment";
    case Stage::Compute:
        return "Compute";
    }
    return "Unknown";
}
// The IR names resources by index into the lists built from Info. An index outside its list is
// an IR/Info mismatch; at() reported it as a bare "invalid vector subscript" with no context.
const EmitContext::ResourceDefinition& Declared(
    const std::vector<EmitContext::ResourceDefinition>& definitions, u32 descriptor,
    std::string_view kind, Stage stage) {
    if (descriptor >= definitions.size())
        throw LogicError("HLSL {} descriptor {} is not declared: stage {} declares {} of them",
                         kind, descriptor, StageName(stage), definitions.size());
    return definitions[descriptor];
}
std::string VectorType(AttributeType type) {
    switch (type) {
    case AttributeType::SignedInt:
    case AttributeType::SignedScaled:
        return "int4";
    case AttributeType::UnsignedInt:
    case AttributeType::UnsignedScaled:
    case AttributeType::SignedNorm10:
    case AttributeType::SignedInt10:
        return "uint4";
    default:
        return "float4";
    }
}
// Global memory is searched window by window, the first window holding the access wins. The
// search is a loop: with the windows unrolled into every access, the Series X driver compiled
// Wonder shaders with 13 windows for more than 45 s while one window took 1 s.
std::string GlobalMemoryHelpers(const Info& info, const Profile& profile) {
    const auto& windows = info.storage_buffers_descriptors;
    std::string text;
    if (windows.empty()) {
        // Without a window, loads read zero and stores are dropped.
        for (const auto& [type, bits] : {std::pair{"uint", 32}, {"uint2", 64}, {"uint4", 128}})
            text += fmt::format("{0} nx_global_load{1}(uint64_t address) {{ return ({0})0; }}\n"
                                "void nx_global_write{1}(uint64_t address, {0} value) {{}}\n",
                                type, bits);
        return text + "uint nx_global_load_subword(uint64_t address, uint bits) { return 0u; }\n"
                      "void nx_global_write_subword(uint64_t address, uint value, uint bits) {}\n";
    }
    const u32 cbuf = windows.front().cbuf_index;
    const bool rows =
        std::ranges::all_of(windows,
                            [&](const auto& desc) {
                                return desc.cbuf_index == cbuf && desc.cbuf_offset % 16 == 0;
                            }) &&
        std::ranges::any_of(info.constant_buffer_descriptors,
                            [&](const auto& desc) { return desc.index == cbuf; });
    std::string offsets, cbufs;
    for (const auto& desc : windows) {
        offsets += fmt::format("{}{}u", offsets.empty() ? "" : ", ", desc.cbuf_offset);
        cbufs += fmt::format("{}{}u", cbufs.empty() ? "" : ", ", desc.cbuf_index);
    }
    const auto word = [&](u32 index) {
        return rows ? fmt::format("cbuf{}[o >> 4].{}", cbuf, "xyzw"[index])
                    : fmt::format("nx_cbuf_load(nx_global_cbufs[i], o + {}u)", index * 4);
    };
    const auto alignment = std::max(static_cast<u32>(profile.min_ssbo_alignment), 1u);
    text =
        fmt::format("static const uint nx_global_offsets[{}] = {{{}}};\n", windows.size(), offsets);
    if (!rows)
        text +=
            fmt::format("static const uint nx_global_cbufs[{}] = {{{}}};\n", windows.size(), cbufs);
    text += fmt::format(
        "bool nx_global_find(uint64_t address, uint bytes, out uint index, out uint offset) {{\n"
        "index = 0u; offset = 0u;\n"
        "[loop] for (uint i = 0u; i < {}u; ++i) {{ uint o = nx_global_offsets[i];\n"
        "uint64_t base = packUint2x32(uint2({} & {}u, {})); "
        "uint64_t size = packUint2x32(uint2({}, {}));\n"
        "if (address >= base && address - base <= size && size - (address - base) >= bytes) {{ "
        "index = i; offset = uint(address - base); return true; }} }}\n"
        "return false; }}\n",
        windows.size(), word(0), ~(alignment - 1u), word(1), word(2), word(3));
    constexpr std::string_view buffer = "ssbo[NonUniformResourceIndex(index)]";
    for (u32 bits : {32u, 64u, 128u}) {
        const auto type = bits == 32 ? "uint" : bits == 64 ? "uint2" : "uint4";
        const auto suffix = bits == 32 ? "" : bits == 64 ? "2" : "4";
        text += fmt::format(
            "{0} nx_global_load{1}(uint64_t address) {{ uint index, offset; if "
            "(nx_global_find(address, {2}u, index, offset)) return {3}.Load{4}(offset); "
            "return ({0})0; }}\n"
            "void nx_global_write{1}(uint64_t address, {0} value) {{ uint index, offset; if "
            "(nx_global_find(address, {2}u, index, offset)) {3}.Store{4}(offset, value); }}\n",
            type, bits, bits / 8, buffer, suffix);
    }
    return text +
           fmt::format("uint nx_global_load_subword(uint64_t address, uint bits) {{\n"
                       "address &= ~uint64_t(bits == 16u ? 1u : 0u); uint index, offset;\n"
                       "if (!nx_global_find(address, bits / 8u, index, offset)) return 0u;\n"
                       "return bitfieldExtract({0}.Load(offset & ~3u), int((offset & 3u) * 8u), "
                       "int(bits)); }}\n"
                       "void nx_global_write_subword(uint64_t address, uint value, uint bits) {{\n"
                       "address &= ~uint64_t(bits == 16u ? 1u : 0u); uint index, offset;\n"
                       "if (!nx_global_find(address, bits / 8u, index, offset)) return;\n"
                       "uint aligned = offset & ~3u; uint shift = (offset & (bits == 8u ? 3u : "
                       "2u)) * 8u; uint old_value = {0}.Load(aligned); uint previous;\n"
                       "do {{ uint new_value = bitfieldInsert(old_value, value, int(shift), "
                       "int(bits)); {0}.InterlockedCompareExchange(aligned, old_value, new_value, "
                       "previous); if (previous == old_value) break; old_value = previous; }} "
                       "while (true); }}\n",
                       buffer);
}
} // namespace
EmitContext::EmitContext(IR::Program& program, Bindings& bindings, const Profile& profile_,
                         const RuntimeInfo& runtime_info_)
    : info{program.info}, profile{profile_}, runtime_info{runtime_info_}, stage{program.stage} {
    var_alloc.drop_precise = stage == Stage::Compute;
    if (stage != Stage::VertexA && stage != Stage::VertexB && stage != Stage::Fragment &&
        stage != Stage::Compute && stage != Stage::Geometry)
        throw NotImplementedException("HLSL tessellation stage {} requires stage linkage",
                                      static_cast<u32>(stage));
    // Stream-output declarations and dual-source RTV linkage belong to the native PSO.
    if (program.is_geometry_passthrough)
        throw NotImplementedException(
            "HLSL geometry passthrough requires passthrough varying linkage");
    if (info.loads.Legacy() || info.stores.Legacy())
        throw NotImplementedException("HLSL legacy attributes must be lowered to generic varyings");
    for (const auto* block : program.blocks)
        for (const auto& inst : block->Instructions())
            uses_shared_wide |= inst.GetOpcode() == IR::Opcode::SharedAtomicExchange64 ||
                                inst.GetOpcode() == IR::Opcode::SharedAtomicExchange32x2;
    if (uses_shared_wide && !profile.support_shared_int64_atomics)
        throw NotImplementedException(
            "HLSL shared wide exchange requires SM 6.6 shared int64 atomics");
    header = std::string(ROOT_CONSTANT_DECLARATION) + std::string(ARITHMETIC_HELPERS);
    if (info.uses_int64 || info.uses_fp64 || info.uses_global_memory || uses_shared_wide)
        header += WIDE_ARITHMETIC_HELPERS;
    if (info.uses_subgroup_vote || info.uses_subgroup_shuffles ||
        info.uses_subgroup_invocation_id || info.uses_subgroup_mask || info.uses_quad_shuffles ||
        info.uses_fswzadd)
        header += WAVE_HELPERS;
    for (const auto& desc : info.constant_buffer_descriptors) {
        if (desc.count != 1)
            throw NotImplementedException("HLSL constant-buffer descriptor arrays");
        // HLSL arrays have 16-byte cbuffer stride: uint4 preserves byte-addressed guest layout.
        header +=
            fmt::format("cbuffer nx_cbuf{} : register(b{}, space0) {{ uint4 cbuf{}[4096]; }};\n",
                        desc.index, bindings.uniform_buffer++, desc.index);
    }
    header += "uint nx_cbuf_load(uint binding, uint offset) { if (offset >= 65536u) return 0u; "
              "switch(binding) {\n";
    for (const auto& desc : info.constant_buffer_descriptors)
        header += fmt::format("case {}u: return cbuf{}[offset >> 4][(offset >> 2) & 3u];\n",
                              desc.index, desc.index);
    header += "default: return 0u; }}\n";
    for (const auto& desc : info.texture_descriptors) {
        const auto binding = bindings.texture;
        textures.push_back({binding, desc.count});
        header += RegisterDeclaration(TextureClass(desc.type, false, desc.is_multisample) +
                                          (desc.is_depth     ? "<float>"
                                           : desc.is_integer ? "<uint4>"
                                                             : "<float4>"),
                                      fmt::format("tex{}", binding), 't', binding, desc.count);
        if (!desc.is_multisample)
            header += RegisterDeclaration(desc.is_depth ? "SamplerComparisonState" : "SamplerState",
                                          fmt::format("samp{}", binding), 's',
                                          bindings.hlsl_sampler, desc.count);
        bindings.texture += desc.count;
        bindings.hlsl_sampler += desc.count;
    }
    for (const auto& desc : info.texture_buffer_descriptors) {
        const auto binding = bindings.texture;
        texture_buffers.push_back({binding, desc.count});
        header += RegisterDeclaration("Buffer<float4>", fmt::format("tex{}", binding), 't', binding,
                                      desc.count);
        bindings.texture += desc.count;
    }
    const u32 first_storage_register = bindings.storage_buffer + bindings.image;
    for (const auto& desc : info.storage_buffers_descriptors) {
        if (desc.count != 1)
            throw NotImplementedException("HLSL storage-buffer descriptor arrays");
        storage_bindings.push_back(bindings.storage_buffer++);
    }
    // One array, so a global memory access can pick its window with a dynamic index.
    if (!storage_bindings.empty())
        header += fmt::format("RWByteAddressBuffer ssbo[{}] : register(u{}, space0);\n",
                              storage_bindings.size(), first_storage_register);
    if (info.uses_global_memory)
        header += GlobalMemoryHelpers(info, profile);
    // Image register numbers share the UAV namespace with storage buffers.
    u32 image_binding = bindings.storage_buffer + bindings.image;
    for (const auto& desc : info.image_descriptors) {
        images.push_back({image_binding, desc.count});
        const auto element =
            desc.format == ImageFormat::Typeless ? (desc.is_integer ? "uint4" : "float4")
            : desc.format == ImageFormat::R8_SINT || desc.format == ImageFormat::R16_SINT ? "int"
            : (desc.format == ImageFormat::R32_UINT || desc.format == ImageFormat::R8_UINT ||
               desc.format == ImageFormat::R16_UINT)
                ? "uint"
            : desc.format == ImageFormat::R32G32_UINT ? "uint2"
                                                      : "uint4";
        header += RegisterDeclaration(TextureClass(desc.type, true) + "<" + element + ">",
                                      fmt::format("img{}", image_binding), 'u', image_binding,
                                      desc.count);
        image_binding += desc.count;
        bindings.image += desc.count;
    }
    for (const auto& desc : info.image_buffer_descriptors) {
        image_buffers.push_back({image_binding, desc.count});
        const auto element =
            desc.format == ImageFormat::Typeless ? (desc.is_integer ? "uint4" : "float4")
            : desc.format == ImageFormat::R8_SINT || desc.format == ImageFormat::R16_SINT ? "int"
            : (desc.format == ImageFormat::R32_UINT || desc.format == ImageFormat::R8_UINT ||
               desc.format == ImageFormat::R16_UINT)
                ? "uint"
            : desc.format == ImageFormat::R32G32_UINT ? "uint2"
                                                      : "uint4";
        header += RegisterDeclaration(std::string("RWBuffer<") + element + ">",
                                      fmt::format("img{}", image_binding), 'u', image_binding,
                                      desc.count);
        image_binding += desc.count;
        bindings.image += desc.count;
    }
    if (program.shared_memory_size) {
        if (stage != Stage::Compute || program.shared_memory_size > 32768)
            throw NotImplementedException(
                "HLSL groupshared memory exceeds SM 6.0 limit or used outside compute");
        if (uses_shared_wide) {
            header += fmt::format("groupshared uint64_t smem64[{}];\n",
                                  (program.shared_memory_size + 7) / 8);
            header += SHARED_WIDE_HELPERS;
        } else {
            header +=
                fmt::format("groupshared uint smem[{}];\n", (program.shared_memory_size + 3) / 4);
        }
    }
    if (stage == Stage::Compute)
        return;
    header += "struct Input {\n";
    std::string system_inputs;
    if (stage == Stage::Fragment || stage == Stage::Geometry) {
        header += "float4 position : SV_Position;\n";
        if (runtime_info.previous_stage_stores[IR::Attribute::Layer])
            system_inputs += "uint layer : SV_RenderTargetArrayIndex;\n";
        if (runtime_info.previous_stage_stores[IR::Attribute::ViewportIndex])
            system_inputs += "uint viewport : SV_ViewportArrayIndex;\n";
        for (u32 group = 0; group < 2; ++group) {
            bool used = false;
            for (u32 element = 0; element < 4; ++element)
                used |= runtime_info.previous_stage_stores[static_cast<IR::Attribute>(
                    static_cast<u32>(IR::Attribute::ClipDistance0) + group * 4 + element)];
            if (used)
                system_inputs += fmt::format("float4 clip{} : SV_ClipDistance{};\n", group, group);
        }
        if (stage == Stage::Fragment) {
            if (info.loads[IR::Attribute::FrontFace])
                system_inputs += "bool front_face : SV_IsFrontFace;\n";
            if (info.uses_sample_id)
                system_inputs += "uint sample_id : SV_SampleIndex;\n";
            if (info.loads[IR::Attribute::PrimitiveId])
                system_inputs += "uint primitive_id : SV_PrimitiveID;\n";
        }
    } else {
        header += "uint vertex_id : SV_VertexID; uint instance_id : SV_InstanceID;\n";
    }
    for (size_t i = 0; i < 32; ++i) {
        if ((stage == Stage::Fragment || stage == Stage::Geometry)
                ? !runtime_info.previous_stage_stores.Generic(i)
                : !info.loads.Generic(i))
            continue;
        std::string interpolation;
        if (stage == Stage::Fragment) {
            interpolation = info.interpolation[i] == Interpolation::Flat ? "nointerpolation "
                            : info.interpolation[i] == Interpolation::NoPerspective
                                ? "noperspective "
                                : "";
        }
        const auto type = (stage == Stage::Fragment || stage == Stage::Geometry)
                              ? "float4"
                              : VectorType(runtime_info.generic_input_types[i]);
        const auto semantic = GenericSemantic(static_cast<std::uint32_t>(i));
        header += fmt::format("{}{} attr{} : {}{};\n", interpolation, type, i, semantic.name,
                              semantic.index);
    }
    header += system_inputs;
    header += "};\nstruct Output {\n";
    if (stage != Stage::Fragment) {
        // Mark the output sink, so precision propagates through guest arithmetic and
        // the final Y/depth conversion. Fragment/geometry inputs remain unqualified.
        header += (Nxbox::SeamDiagnostics::Mode() & Nxbox::SeamDiagnostics::PrecisePosition)
                      ? "precise float4 position : SV_Position;\n"
                      : "float4 position : SV_Position;\n";
        for (size_t i = 0; i < 32; ++i)
            if (info.stores.Generic(i))
                header += fmt::format("float4 attr{} : TEXCOORD{};\n", i, i);
        if (info.stores[IR::Attribute::Layer])
            header += "uint layer : SV_RenderTargetArrayIndex;\n";
        if (info.stores[IR::Attribute::ViewportIndex])
            header += "uint viewport : SV_ViewportArrayIndex;\n";
        // Same rule as the next stage's input: a group exists when any of its elements is
        // stored. used_clip_distances is a count, and only set for indexed attribute stores.
        for (u32 group = 0; group < 2; ++group) {
            bool used = false;
            for (u32 element = 0; element < 4; ++element)
                used |= info.stores[static_cast<IR::Attribute>(
                    static_cast<u32>(IR::Attribute::ClipDistance0) + group * 4 + element)];
            if (used)
                header += fmt::format("float4 clip{} : SV_ClipDistance{};\n", group, group);
        }
        if (runtime_info.xfb_count)
            header += "float4 nx_xfb_position : NX_XFB_POSITION;\n";
    } else {
        bool has_output = info.stores_frag_depth || info.stores_sample_mask;
        for (size_t i = 0; i < 8; ++i) {
            if (!info.stores_frag_color[i])
                continue;
            has_output = true;
            header += fmt::format("{} color{} : SV_Target{};\n",
                                  VectorType(runtime_info.color_output_types[i]), i, i);
        }
        if (info.stores_frag_depth)
            header += "float depth : SV_Depth;\n";
        if (info.stores_sample_mask)
            header += "uint coverage : SV_Coverage;\n";
        // HLSL permits a void pixel shader. Keep an internal empty-output carrier.
        if (!has_output)
            header += "uint unused;\n";
    }
    header += "};\n";
}
std::string EmitContext::Attribute(IR::Attribute attribute, bool output) {
    const auto prefix = output ? "output." : "input.";
    if (!output &&
        ((attribute == IR::Attribute::PrimitiveId && stage != Stage::Fragment &&
          stage != Stage::Geometry) ||
         (attribute >= IR::Attribute::PositionX && attribute <= IR::Attribute::PositionW &&
          stage != Stage::Fragment && stage != Stage::Geometry) ||
         (attribute >= IR::Attribute::ClipDistance0 && attribute <= IR::Attribute::ClipDistance7 &&
          stage != Stage::Geometry)))
        throw NotImplementedException("HLSL input attribute {} in stage {}", attribute,
                                      static_cast<u32>(stage));
    if (output &&
        (attribute == IR::Attribute::Layer || attribute == IR::Attribute::ViewportIndex) &&
        stage != Stage::Geometry && !profile.support_viewport_index_layer_non_geometry)
        throw NotImplementedException(
            "HLSL vertex layer/viewport output needs device feature support");
    if (IR::IsGeneric(attribute)) {
        const auto index = IR::GenericAttributeIndex(attribute);
        return fmt::format("{}attr{}.{}", prefix, index,
                           "xyzw"[IR::GenericAttributeElement(attribute)]);
    }
    const auto raw = static_cast<u32>(attribute);
    if (attribute >= IR::Attribute::PositionX && attribute <= IR::Attribute::PositionW)
        return fmt::format("{}position.{}", prefix, "xyzw"[raw % 4]);
    if (attribute >= IR::Attribute::ClipDistance0 && attribute <= IR::Attribute::ClipDistance7) {
        const auto index = raw - static_cast<u32>(IR::Attribute::ClipDistance0);
        return fmt::format("{}clip{}.{}", prefix, index / 4, "xyzw"[index % 4]);
    }
    switch (attribute) {
    case IR::Attribute::VertexId:
        return "(input.vertex_id + nx_base_vertex)";
    case IR::Attribute::InstanceId:
        return "(input.instance_id + nx_base_instance)";
    case IR::Attribute::PrimitiveId:
        return stage == Stage::Geometry ? "nx_primitive_id" : "input.primitive_id";
    case IR::Attribute::FrontFace:
        return "(input.front_face ? 0xffffffffu : 0u)";
    case IR::Attribute::Layer:
        return std::string(prefix) + "layer";
    case IR::Attribute::ViewportIndex:
        if (!output && stage != Stage::Geometry)
            throw NotImplementedException("HLSL viewport index input");
        return std::string(prefix) + "viewport";
    case IR::Attribute::BaseVertex:
        return "nx_base_vertex";
    case IR::Attribute::BaseInstance:
        return "nx_base_instance";
    case IR::Attribute::DrawID:
        return "nx_draw_id";
    default:
        throw NotImplementedException("HLSL attribute {}", attribute);
    }
}
std::string EmitContext::Resource(const IR::Value& index, u32 descriptor, bool image, bool buffer) {
    const auto& definitions =
        image ? (buffer ? image_buffers : images) : (buffer ? texture_buffers : textures);
    const auto def = Declared(definitions, descriptor,
                              image ? (buffer ? "image buffer" : "image")
                                    : (buffer ? "texture buffer" : "texture"),
                              stage);
    return fmt::format("{}{}{}", image ? "img" : "tex", def.binding,
                       def.count > 1
                           ? fmt::format("[NonUniformResourceIndex({})]", var_alloc.Consume(index))
                           : "");
}
std::string EmitContext::CbufferLoad(u32 binding, u32 offset) {
    return CbufferLoad(IR::Value{binding}, IR::Value{offset});
}
std::string EmitContext::CbufferLoad(const IR::Value& binding, const IR::Value& offset, u32 delta) {
    const std::optional<u32> index =
        binding.IsImmediate() ? std::optional{binding.U32()} : std::nullopt;
    const std::optional<u32> bytes =
        offset.IsImmediate() ? std::optional{offset.U32() + delta} : std::nullopt;
    const bool declared =
        index && std::any_of(info.constant_buffer_descriptors.begin(),
                             info.constant_buffer_descriptors.end(),
                             [&](const auto& desc) { return desc.index == *index; });
    if (IsDirectCbufferAddress(index, bytes, declared))
        return fmt::format("cbuf{}[{}][{}]", *index, *bytes >> 4, (*bytes >> 2) & 3u);
    const auto byte_offset = delta ? fmt::format("{} + {}u", var_alloc.Consume(offset), delta)
                                   : var_alloc.Consume(offset);
    return fmt::format("nx_cbuf_load({}, {})", var_alloc.Consume(binding), byte_offset);
}
bool EmitContext::GlobalMemoryAccess(std::string_view address, u32 bytes) {
    if (storage_bindings.empty())
        return false;
    Add("{{ uint nx_global_index, nx_global_offset; "
        "if (nx_global_find({}, {}u, nx_global_index, nx_global_offset)) {{",
        address, bytes);
    return true;
}
std::string EmitContext::Sampler(const IR::Value& index, u32 descriptor) {
    // Samplers are declared next to textures only; a texture buffer has none.
    const auto def = Declared(textures, descriptor, "texture sampler", stage);
    return fmt::format("samp{}{}", def.binding,
                       def.count > 1
                           ? fmt::format("[NonUniformResourceIndex({})]", var_alloc.Consume(index))
                           : "");
}
std::string EmitContext::ReturnStatement() const {
    if (stage == Stage::Compute || stage == Stage::Geometry)
        return "return;";
    if (stage == Stage::Fragment && !info.stores_frag_depth && !info.stores_sample_mask &&
        std::none_of(info.stores_frag_color.begin(), info.stores_frag_color.end(),
                     [](bool v) { return v; }))
        return "return;";
    return "return output;";
}
void EmitContext::FinishOutput(std::string_view output) {
    if (stage == Stage::Fragment && runtime_info.alpha_test_func) {
        const auto function = *runtime_info.alpha_test_func;
        if (function == CompareFunction::Never) {
            Add("discard;");
        } else if (function != CompareFunction::Always) {
            const auto op = function == CompareFunction::Less            ? "<"
                            : function == CompareFunction::Equal         ? "=="
                            : function == CompareFunction::LessThanEqual ? "<="
                            : function == CompareFunction::Greater       ? ">"
                            : function == CompareFunction::NotEqual      ? "!="
                                                                         : ">=";
            Add("if (!(nx_alpha {} asfloat(0x{:08x}u))) discard;", op,
                std::bit_cast<u32>(runtime_info.alpha_test_reference));
        }
        return;
    }
    if (stage != Stage::VertexA && stage != Stage::VertexB && stage != Stage::Geometry)
        return;
    if (runtime_info.xfb_count)
        Add("{0}.nx_xfb_position = {0}.position;", output);
    if (runtime_info.y_negate)
        Add("{0}.position.y = -{0}.position.y;", output);
    if (runtime_info.convert_depth_mode)
        Add("{0}.position.z = ({0}.position.z + {0}.position.w) * 0.5f;", output);
}
} // namespace Shader::Backend::HLSL
