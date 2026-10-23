// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "common/nxbox_seam_diagnostics.h"
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"

#include "shader_recompiler/frontend/ir/modifiers.h"

using namespace Shader;
using Shader::Backend::Bindings;
namespace {
void Require(bool value, std::string_view message) {
  if (!value)
    throw std::runtime_error(std::string(message));
}
void Contains(const std::string &text, std::string_view token) {
  Require(text.find(token) != text.npos, token);
}
struct Fixture {
  ObjectPool<IR::Inst> pool;
  IR::Block block{pool};
  IR::Program program;
  Fixture(Stage stage) {
    program.stage = stage;
    program.blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block},
         .type = IR::AbstractSyntaxNode::Type::Block});
    program.workgroup_size = {8, 4, 1};
  }
  IR::Value Add(IR::Opcode opcode, std::initializer_list<IR::Value> args = {},
                u32 flags = 0) {
    auto it = block.PrependNewInst(block.end(), opcode, args, flags);
    return IR::Value{&*it};
  }
  std::string Emit(Bindings &bindings, const Profile &profile = {},
                   const RuntimeInfo &runtime = {}) {
    return Backend::HLSL::EmitHLSL(profile, runtime, program, bindings);
  }
};
void Vertex() {
  Fixture f{Stage::VertexB};
  f.program.info.loads.Set(IR::Attribute::Generic0X);
  f.program.info.stores.Set(IR::Attribute::PositionX);
  const auto input =
      f.Add(IR::Opcode::GetAttribute,
            {IR::Value{IR::Attribute::Generic0X}, IR::Value{0u}});
  const auto sum = f.Add(IR::Opcode::FPAdd32, {input, IR::Value{2.0f}});
  f.Add(IR::Opcode::SetAttribute,
        {IR::Value{IR::Attribute::PositionX}, sum, IR::Value{0u}});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "Output main(Input input)");
  Contains(text, "SV_Position");
  Contains(text, "TEXCOORD0");
  Contains(text, "output.position.x =");
  const auto second = f.Emit(bindings);
  Require(text == second, "Emission must preserve the IR");
  std::ofstream("hlsl_vertex.hlsl") << text;
}
void SeamPositionOutput() {
    for (const auto stage : {Stage::VertexB, Stage::Geometry}) {
        Fixture f{stage};
        f.program.output_topology = OutputTopology::TriangleStrip;
        f.program.output_vertices = 3;
        f.program.invocations = 1;
        f.program.info.stores.Set(IR::Attribute::PositionY);
        f.Add(IR::Opcode::SetAttribute,
              {IR::Value{IR::Attribute::PositionY}, IR::Value{0.25f}, IR::Value{0u}});
        if (stage == Stage::Geometry) {
            f.Add(IR::Opcode::EmitVertex, {IR::Value{0u}});
            f.Add(IR::Opcode::EmitVertex, {IR::Value{0u}});
        }
        RuntimeInfo runtime{};
        runtime.input_topology = InputTopology::Triangles;
        runtime.y_negate = true;
        runtime.convert_depth_mode = true;
        Bindings bindings;
        const auto text = f.Emit(bindings, {}, runtime);
        const auto output_start = text.find("struct Output {");
        const auto output_end = text.find("};", output_start);
        const auto output = text.substr(output_start, output_end - output_start);
        const bool precise =
            (Nxbox::SeamDiagnostics::Mode() & Nxbox::SeamDiagnostics::PrecisePosition) != 0;
        Require((output.find("precise float4 position") != output.npos) == precise,
                "Position precision must follow the diagnostic mode");
        const auto input_start = text.find("struct Input {");
        Require(text.substr(input_start, output_start - input_start).find("precise") == text.npos,
                "Do not qualify rasterizer inputs as precise");
        const auto target = stage == Stage::Geometry ? "nx_vertex" : "output";
        Contains(text, std::string(target) + ".position.y = -" + target + ".position.y;");
        Contains(text, std::string(target) + ".position.z = (" + target + ".position.z + " +
                           target + ".position.w) * 0.5f;");
        if (stage == Stage::Geometry) {
            Require(text.find("output.position.y = -") == text.npos,
                    "Repeated GS emission must transform copies, not the guest output");
        }
    }
}
void DirectClipDistances() {
  // Direct stores leave used_clip_distances at zero; the output must still
  // declare every written group (Mario Tennis Aces failed DXC on clip0).
  Fixture f{Stage::VertexB};
  f.program.info.stores.Set(IR::Attribute::PositionX);
  f.program.info.stores.Set(IR::Attribute::ClipDistance0);
  f.program.info.stores.Set(IR::Attribute::ClipDistance5);
  f.Add(IR::Opcode::SetAttribute,
        {IR::Value{IR::Attribute::ClipDistance0}, IR::Value{1.0f}, IR::Value{0u}});
  f.Add(IR::Opcode::SetAttribute,
        {IR::Value{IR::Attribute::ClipDistance5}, IR::Value{1.0f}, IR::Value{0u}});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "float4 clip0 : SV_ClipDistance0;");
  Contains(text, "float4 clip1 : SV_ClipDistance1;");
  Contains(text, "output.clip0.x =");
  Contains(text, "output.clip1.y =");
  std::ofstream("hlsl_clip.hlsl") << text;
}
void IntegerTextureSample() {
  // Integer textures cannot be filtered in HLSL; nearest sampling becomes a
  // clamped Load of the texel under the coordinate.
  for (const auto type : {TextureType::Color2D, TextureType::ColorArray2D}) {
    Fixture f{Stage::Fragment};
    f.program.info.stores_frag_color[0] = true;
    f.program.info.texture_descriptors.push_back(
        {.type = type, .is_integer = true, .count = 1});
    const bool array = type == TextureType::ColorArray2D;
    const auto coords =
        array ? f.Add(IR::Opcode::CompositeConstructF32x3,
                      {IR::Value{0.5f}, IR::Value{0.25f}, IR::Value{1.0f}})
              : f.Add(IR::Opcode::CompositeConstructF32x2,
                      {IR::Value{0.5f}, IR::Value{0.25f}});
    IR::TextureInstInfo flags{};
    flags.type.Assign(type);
    flags.is_integer.Assign(1);
    const auto sample = f.Add(IR::Opcode::ImageSampleImplicitLod,
                              {IR::Value{0u}, coords, {}, {}}, flags.raw);
    const auto red =
        f.Add(IR::Opcode::CompositeExtractF32x4, {sample, IR::Value{0u}});
    f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, red});
    Bindings bindings;
    const auto text = f.Emit(bindings);
    Contains(text, array ? "Texture2DArray<uint4> tex0" : "Texture2D<uint4> tex0");
    Contains(text, ".GetDimensions(nx_mip, nx_d.x, nx_d.y, ");
    Contains(text, array ? ".Load(int4(clamp(int2(floor(" : ".Load(int3(clamp(int2(floor(");
    Require(text.find(".Sample(") == text.npos, "Integer textures must not be filtered");
    std::ofstream(array ? "hlsl_intsample_array.hlsl" : "hlsl_intsample.hlsl") << text;
  }
}
void Fragment() {
  Fixture f{Stage::Fragment};
  f.program.info.stores_frag_color[0] = true;
  f.program.info.stores_frag_depth = true;
  f.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .count = 1});
  const auto coords = f.Add(IR::Opcode::CompositeConstructF32x2,
                            {IR::Value{0.5f}, IR::Value{0.25f}});
  IR::TextureInstInfo flags{};
  flags.type.Assign(TextureType::Color2D);
  const auto sample = f.Add(IR::Opcode::ImageSampleImplicitLod,
                            {IR::Value{0u}, coords, {}, {}}, flags.raw);
  const auto red =
      f.Add(IR::Opcode::CompositeExtractF32x4, {sample, IR::Value{0u}});
  f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, red});
  f.Add(IR::Opcode::SetFragDepth, {IR::Value{0.75f}});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "Texture2D<float4> tex0 : register(t0, space0)");
  Contains(text, "SamplerState samp0 : register(s0, space0)");
  Contains(text, ".Sample(samp0,");
  Contains(text, "SV_Target0");
  Contains(text, "SV_Depth");
  Require(bindings.texture == 1, "Texture counter");
  std::ofstream("hlsl_fragment.hlsl") << text;
  // Attachment specialization clears export metadata but keeps the original IR.
  // Stores into unbound targets must disappear from both declarations and body.
  f.Add(IR::Opcode::SetFragColor, {IR::Value{3u}, IR::Value{0u}, red});
  Bindings masked_bindings;
  const auto masked = f.Emit(masked_bindings);
  Require(masked.find("SV_Target3") == masked.npos, "Unbound target signature");
  Require(masked.find("output.color3") == masked.npos, "Unbound target store");
  f.program.info.stores_frag_color[0] = false;
  Bindings depth_bindings;
  const auto depth_only = f.Emit(depth_bindings);
  Require(depth_only.find("SV_Target") == depth_only.npos,
          "Depth-only color signature");
  Require(depth_only.find("output.color0") == depth_only.npos,
          "Depth-only color store");
  Contains(depth_only, "SV_Depth");
}
void MultisampleFetch() {
  for (const auto type : {TextureType::Color2D, TextureType::ColorArray2D}) {
    for (const bool depth : {false, true}) {
      Fixture f{Stage::Fragment};
      f.program.info.stores_frag_color[0] = true;
      f.program.info.texture_descriptors.push_back({.type = type,
                                                    .is_depth = depth,
                                                    .is_multisample = true,
                                                    .count = 1});
      const bool array = type == TextureType::ColorArray2D;
      const auto coords =
          array ? f.Add(IR::Opcode::CompositeConstructU32x3,
                        {IR::Value{1u}, IR::Value{2u}, IR::Value{0u}})
                : f.Add(IR::Opcode::CompositeConstructU32x2,
                        {IR::Value{1u}, IR::Value{2u}});
      IR::TextureInstInfo flags{};
      flags.type.Assign(type);
      flags.is_depth.Assign(depth);
      const auto sample = f.Add(
          IR::Opcode::ImageFetch,
          {IR::Value{0u}, coords, {}, IR::Value{0u}, IR::Value{2u}}, flags.raw);
      const auto red =
          f.Add(IR::Opcode::CompositeExtractF32x4, {sample, IR::Value{0u}});
      f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, red});
      // An omitted sample operand still selects the MSAA overload at sample
      // zero.
      const auto zero =
          f.Add(IR::Opcode::ImageFetch,
                {IR::Value{0u}, coords, {}, IR::Value{0u}, {}}, flags.raw);
      const auto green =
          f.Add(IR::Opcode::CompositeExtractF32x4, {zero, IR::Value{0u}});
      f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{1u}, green});
      Bindings bindings;
      const auto text = f.Emit(bindings);
      Contains(text, array ? "Texture2DMSArray<" : "Texture2DMS<");
      Contains(text, "tex0.Load(");
      Contains(text, ", int(2u)");
      Contains(text, ", int(0)");
      Require(text.find("SamplerState") == text.npos,
              "MSAA has no sampler declaration");
      const std::string name = std::string{"hlsl_msaa"} +
                               (array ? "_array" : "") +
                               (depth ? "_depth" : "") + ".hlsl";
      std::ofstream(name) << text;
    }
  }
}
void Compute() {
  Fixture f{Stage::Compute};
  f.program.shared_memory_size = 128;
  f.program.info.constant_buffer_descriptors.push_back(
      {.index = 3, .count = 1});
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  const auto value =
      f.Add(IR::Opcode::GetCbufU32, {IR::Value{3u}, IR::Value{16u}});
  const auto old = f.Add(IR::Opcode::StorageAtomicIAdd32,
                         {IR::Value{0u}, IR::Value{0u}, value});
  f.Add(IR::Opcode::WriteSharedU32, {IR::Value{4u}, old});
  f.Add(IR::Opcode::Barrier);
  f.Add(IR::Opcode::WriteStorage32, {IR::Value{0u}, IR::Value{4u}, old});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "register(b0, space1)");
  Contains(text, "nx_cbuf3 : register(b0, space0)");
  Contains(text, "RWByteAddressBuffer ssbo[1] : register(u0, space0)");
  Contains(text, ".InterlockedAdd(");
  Contains(text, "groupshared uint smem[32]");
  Contains(text, "GroupMemoryBarrierWithGroupSync");
  Contains(text, "[numthreads(8, 4, 1)]");
  Require(bindings.uniform_buffer == 1 && bindings.storage_buffer == 1,
          "Buffer counters");
  std::ofstream("hlsl_compute.hlsl") << text;
}
void ControlFlowAndWaves() {
  Fixture f{Stage::Compute};
  f.program.info.uses_subgroup_vote = true;
  f.program.info.uses_subgroup_shuffles = true;
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  IR::Block header{f.pool};
  IR::Block body{f.pool};
  f.program.blocks.push_back(&header);
  f.program.blocks.push_back(&body);
  auto first_it = header.PrependNewInst(header.end(), IR::Opcode::Phi, {},
                                        static_cast<u32>(IR::Type::U32));
  auto second_it = header.PrependNewInst(header.end(), IR::Opcode::Phi, {},
                                         static_cast<u32>(IR::Type::U32));
  first_it->AddPhiOperand(&f.block, IR::Value{11u});
  second_it->AddPhiOperand(&f.block, IR::Value{22u});
  first_it->AddPhiOperand(&body, IR::Value{&*second_it});
  second_it->AddPhiOperand(&body, IR::Value{&*first_it});
  const auto condition = f.Add(IR::Opcode::ConditionRef, {IR::Value{true}});
  f.Add(IR::Opcode::SubgroupBallot, {condition});
  f.Add(IR::Opcode::VoteAll, {condition});
  const auto shuffle =
      f.Add(IR::Opcode::ShuffleIndex,
            {IR::Value{7u}, IR::Value{1u}, IR::Value{31u}, IR::Value{0u}});
  const auto flag = f.Add(IR::Opcode::GetInBoundsFromOp, {shuffle});
  f.Add(IR::Opcode::LogicalNot, {flag});
  body.AppendNewInst(IR::Opcode::WriteStorage32,
                     {IR::Value{0u}, IR::Value{0u}, IR::Value{&*first_it}});
  using Node = IR::AbstractSyntaxNode;
  f.program.syntax_list.push_back(
      {.data = {.loop = {.body = &header}}, .type = Node::Type::Loop});
  f.program.syntax_list.push_back(
      {.data = {.block = &header}, .type = Node::Type::Block});
  f.program.syntax_list.push_back(
      {.data = {.if_node = {.cond = IR::U1{condition}}},
       .type = Node::Type::If});
  f.program.syntax_list.push_back(
      {.data = {.block = &body}, .type = Node::Type::Block});
  f.program.syntax_list.push_back({.type = Node::Type::EndIf});
  f.program.syntax_list.push_back(
      {.data = {.repeat = {.cond = IR::U1{IR::Value{false}}}},
       .type = Node::Type::Repeat});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "incoming_v");
  Contains(text, "while (true)");
  Contains(text, "int loop0 = 0x2000");
  Contains(text, "nx_shuffle(");
  Contains(text, "nx_ballot(");
  Contains(text, "WaveReadLaneAt");
  // Check parallel phi assignments: every snapshot precedes the first write.
  const auto start = text.find("uint incoming_");
  const auto next = text.find("uint incoming_", start + 1);
  const auto write = text.find(" = incoming_", start);
  Require(start < next && next < write,
          "Phi snapshots must precede phi writes");
  Bindings repeated;
  Require(text == f.Emit(repeated), "Wave/pseudo/phi emission modified IR");
  std::ofstream("hlsl_waves.hlsl") << text;
}
void ResourceOrderAndCompare() {
  Fixture f{Stage::Fragment};
  f.program.info.stores_frag_color[0] = true;
  f.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .is_depth = true, .count = 1});
  f.program.info.texture_buffer_descriptors.push_back({.count = 1});
  f.program.info.storage_buffers_descriptors.push_back({.count = 1});
  f.program.info.image_descriptors.push_back({.type = TextureType::Color2D,
                                              .format = ImageFormat::R32_UINT,
                                              .is_written = true,
                                              .is_integer = true,
                                              .count = 1});
  const auto coords = f.Add(IR::Opcode::CompositeConstructF32x2,
                            {IR::Value{0.5f}, IR::Value{0.25f}});
  IR::TextureInstInfo depth{};
  depth.type.Assign(TextureType::Color2D);
  depth.is_depth.Assign(1);
  const auto compare = f.Add(
      IR::Opcode::ImageSampleDrefExplicitLod,
      {IR::Value{0u}, coords, IR::Value{0.5f}, IR::Value{0.0f}, {}}, depth.raw);
  f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, compare});
  f.Add(IR::Opcode::ImageQueryDimensions,
        {IR::Value{0u}, IR::Value{0u}, IR::Value{false}}, depth.raw);
  IR::TextureInstInfo gather{};
  gather.type.Assign(TextureType::Color2D);
  gather.is_depth.Assign(1);
  f.Add(IR::Opcode::ImageGatherDref,
        {IR::Value{0u}, coords, {}, {}, IR::Value{0.5f}}, gather.raw);
  const auto int_coords = f.Add(IR::Opcode::CompositeConstructU32x2,
                                {IR::Value{1u}, IR::Value{2u}});
  IR::TextureInstInfo image{};
  image.type.Assign(TextureType::Color2D);
  image.image_format.Assign(ImageFormat::R32_UINT);
  const auto value =
      f.Add(IR::Opcode::CompositeConstructU32x4,
            {IR::Value{4u}, IR::Value{0u}, IR::Value{0u}, IR::Value{1u}});
  f.Add(IR::Opcode::ImageWrite, {IR::Value{0u}, int_coords, value}, image.raw);
  f.Add(IR::Opcode::ImageRead, {IR::Value{0u}, int_coords}, image.raw);
  f.Add(IR::Opcode::ImageAtomicIAdd32,
        {IR::Value{0u}, int_coords, IR::Value{3u}}, image.raw);
  Bindings bindings{
      .uniform_buffer = 2, .storage_buffer = 3, .texture = 4, .image = 2};
  const auto text = f.Emit(bindings);
  Contains(text, "tex4 : register(t4, space0)");
  Contains(text, "tex5 : register(t5, space0)");
  Contains(text, "ssbo[1] : register(u5, space0)");
  Contains(text, "img6 : register(u6, space0)");
  Contains(text, "SamplerComparisonState samp4");
  Contains(text, ".SampleCmpLevelZero(");
  Contains(text, ".GatherCmp(");
  Contains(text, ".GetDimensions(");
  Contains(text, "InterlockedAdd(img6[");
  Require(bindings.texture == 6 && bindings.storage_buffer == 4 &&
              bindings.image == 3,
          "Shared UAV/SRV counters");
  std::ofstream("hlsl_resources.hlsl") << text;
}
void StorageImageFormats() {
  for (const auto format :
       {ImageFormat::Typeless, ImageFormat::R8_SINT, ImageFormat::R16_SINT}) {
    for (const bool buffer : {false, true}) {
      Fixture f{Stage::Compute};
      if (buffer)
        f.program.info.image_buffer_descriptors.push_back({.format = format,
                                                           .is_written = true,
                                                           .is_read = true,
                                                           .count = 1});
      else
        f.program.info.image_descriptors.push_back(
            {.type = TextureType::Color2D,
             .format = format,
             .is_written = true,
             .is_read = true,
             .count = 1});
      IR::TextureInstInfo flags{};
      flags.type.Assign(buffer ? TextureType::Buffer : TextureType::Color2D);
      flags.image_format.Assign(format);
      const auto coords = buffer ? IR::Value{0u}
                                 : f.Add(IR::Opcode::CompositeConstructU32x2,
                                         {IR::Value{0u}, IR::Value{0u}});
      const auto color =
          f.Add(IR::Opcode::ImageRead, {IR::Value{0u}, coords}, flags.raw);
      f.Add(IR::Opcode::ImageWrite, {IR::Value{0u}, coords, color}, flags.raw);
      Bindings bindings;
      Profile profile{};
      profile.support_typeless_image_loads = true;
      const auto text = f.Emit(bindings, profile);
      const bool typeless = format == ImageFormat::Typeless;
      Contains(text, std::string(buffer ? "RWBuffer<" : "RWTexture2D<") +
                         (typeless ? "float4> img0" : "int> img0"));
      if (typeless) {
        Contains(text, "= asuint(img0[");
        Contains(text, "] = asfloat(");
        Bindings fallback_bindings;
        const auto fallback = f.Emit(fallback_bindings);
        Contains(fallback, "= uint4(0u, 0u, 0u, 0u);");
        Require(fallback.find("= asuint(img0[") == fallback.npos,
                "Unsupported typeless load must return zero");
        Contains(fallback, "] = asfloat(");
      } else {
        Contains(text, ", 0u, 0u, 1u)");
        Contains(text, "] = asint(");
        Contains(text, ".x);");
      }
      std::ofstream("hlsl_storage_" + std::to_string(static_cast<int>(format)) +
                    (buffer ? "_buffer.hlsl" : "_texture.hlsl"))
          << text;
      // Narrow integer UAVs cannot perform D3D12 32-bit image atomics. Reject
      // deliberately rather than emitting uint atomic operands against an int
      // UAV.
      if (!typeless) {
        f.Add(IR::Opcode::ImageAtomicIAdd32,
              {IR::Value{0u}, coords, IR::Value{1u}}, flags.raw);
        Bindings rejected;
        try {
          f.Emit(rejected);
          throw std::runtime_error("Narrow signed image atomic accepted");
        } catch (const NotImplementedException &error) {
          Contains(error.what(), "32-bit UAV");
        }
      }
    }
  }
}
void IntegerTypelessImage() {
  // A typeless image over an integer view keeps the raw uint4 value.
  Fixture f{Stage::Compute};
  f.program.info.image_descriptors.push_back({.type = TextureType::Color2D,
                                              .format = ImageFormat::Typeless,
                                              .is_written = true,
                                              .is_read = true,
                                              .is_integer = true,
                                              .count = 1});
  IR::TextureInstInfo flags{};
  flags.type.Assign(TextureType::Color2D);
  flags.image_format.Assign(ImageFormat::Typeless);
  const auto coords = f.Add(IR::Opcode::CompositeConstructU32x2,
                            {IR::Value{0u}, IR::Value{0u}});
  const auto color = f.Add(IR::Opcode::ImageRead, {IR::Value{0u}, coords}, flags.raw);
  f.Add(IR::Opcode::ImageWrite, {IR::Value{0u}, coords, color}, flags.raw);
  Bindings bindings;
  Profile profile{};
  profile.support_typeless_image_loads = true;
  const auto text = f.Emit(bindings, profile);
  Contains(text, "RWTexture2D<uint4> img0");
  Require(text.find("asuint(img0[") == text.npos && text.find("] = asfloat(") == text.npos,
          "Integer typeless image must not be reinterpreted as float");
}
void ArithmeticAndPseudoResults() {
  Fixture f{Stage::Compute};
  f.program.info.uses_int64 = true;
  f.program.info.uses_fp64 = true;
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  const auto sum =
      f.Add(IR::Opcode::IAdd32, {IR::Value{0xffffffffu}, IR::Value{1u}});
  const auto carry = f.Add(IR::Opcode::GetCarryFromOp, {sum});
  f.Add(IR::Opcode::GetZeroFromOp, {sum});
  f.Add(IR::Opcode::GetSignFromOp, {sum});
  f.Add(IR::Opcode::GetOverflowFromOp, {sum});
  const auto select =
      f.Add(IR::Opcode::SelectU32, {carry, IR::Value{1u}, IR::Value{0u}});
  f.Add(IR::Opcode::WriteStorage32, {IR::Value{0u}, IR::Value{0u}, select});
  const auto wide = f.Add(IR::Opcode::IAdd64,
                          {IR::Value{u64{0x100000001}}, IR::Value{u64{2}}});
  f.Add(IR::Opcode::IAbs64, {wide});
  const auto pair = f.Add(IR::Opcode::UnpackUint2x32, {wide});
  f.Add(IR::Opcode::WriteStorage64, {IR::Value{0u}, IR::Value{8u}, pair});
  const auto rounded = f.Add(IR::Opcode::FPRoundEven64, {IR::Value{2.5}});
  const auto narrowed = f.Add(IR::Opcode::ConvertF32F64, {rounded});
  const auto equal =
      f.Add(IR::Opcode::FPOrdEqual32, {narrowed, IR::Value{2.0f}});
  f.Add(IR::Opcode::SelectU32, {equal, IR::Value{3u}, IR::Value{4u}});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "uaddCarry(");
  Contains(text, "carry!=0");
  Contains(text, "nx_abs_int64(");
  Contains(text, "(uint64_t(1u) | (uint64_t(1u) << 32))");
  Contains(text, "nx_round64(");
  Contains(text, "&&!isnan(");
  Bindings repeated;
  Require(text == f.Emit(repeated),
          "Arithmetic pseudo-result emission modified IR");
  std::ofstream("hlsl_arithmetic.hlsl") << text;
}
void ChainedResourceBindings() {
  Fixture vertex{Stage::VertexB};
  vertex.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .count = 1});
  vertex.program.info.texture_buffer_descriptors.push_back({.count = 1});
  vertex.program.info.storage_buffers_descriptors.push_back({.count = 1});
  vertex.program.info.image_descriptors.push_back(
      {.type = TextureType::Color2D,
       .format = ImageFormat::R32_UINT,
       .count = 1});
  vertex.program.info.image_buffer_descriptors.push_back(
      {.format = ImageFormat::R32_UINT, .count = 1});
  Bindings bindings;
  const auto first = vertex.Emit(bindings);
  Contains(first, "samp0 : register(s0, space0)");
  Contains(first, "ssbo[1] : register(u0, space0)");
  Contains(first, "img1 : register(u1, space0)");
  Contains(first, "img2 : register(u2, space0)");
  Fixture fragment{Stage::Fragment};
  fragment.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .count = 1});
  fragment.program.info.storage_buffers_descriptors.push_back({.count = 1});
  const auto second = fragment.Emit(bindings);
  Contains(second, "tex2 : register(t2, space0)");
  Contains(second, "samp2 : register(s1, space0)");
  Contains(second, "ssbo[1] : register(u3, space0)");
  Require(bindings.texture == 3 && bindings.hlsl_sampler == 2 &&
              bindings.storage_buffer + bindings.image == 4,
          "Stage append order must match native descriptor tables");
}
IR::Value Argument(Fixture &f, IR::Type type) {
  switch (type) {
  case IR::Type::U1:
    return IR::Value{true};
  case IR::Type::U8:
    return IR::Value{u8{1}};
  case IR::Type::U16:
    return IR::Value{u16{1}};
  case IR::Type::U32:
    return IR::Value{1u};
  case IR::Type::U64:
    return IR::Value{u64{1}};
  case IR::Type::F16:
    return f.Add(IR::Opcode::BitCastF16U16, {IR::Value{u16{0x3c00}}});
  case IR::Type::F32:
    return IR::Value{1.0f};
  case IR::Type::F64:
    return IR::Value{1.0};
  default:
    break;
  }
  const auto construct =
      type == IR::Type::U32x2   ? IR::Opcode::CompositeConstructU32x2
      : type == IR::Type::U32x3 ? IR::Opcode::CompositeConstructU32x3
      : type == IR::Type::U32x4 ? IR::Opcode::CompositeConstructU32x4
      : type == IR::Type::F16x2 ? IR::Opcode::CompositeConstructF16x2
      : type == IR::Type::F16x3 ? IR::Opcode::CompositeConstructF16x3
      : type == IR::Type::F16x4 ? IR::Opcode::CompositeConstructF16x4
      : type == IR::Type::F32x2 ? IR::Opcode::CompositeConstructF32x2
      : type == IR::Type::F32x3 ? IR::Opcode::CompositeConstructF32x3
      : type == IR::Type::F32x4 ? IR::Opcode::CompositeConstructF32x4
      : type == IR::Type::F64x2 ? IR::Opcode::CompositeConstructF64x2
      : type == IR::Type::F64x3 ? IR::Opcode::CompositeConstructF64x3
                                : IR::Opcode::CompositeConstructF64x4;
  const auto value = Argument(f, IR::ArgTypeOf(construct, 0));
  const auto count = IR::NumArgsOf(construct);
  return count == 2   ? f.Add(construct, {value, value})
         : count == 3 ? f.Add(construct, {value, value, value})
                      : f.Add(construct, {value, value, value, value});
}
void NumericCoverage() {
  Fixture f{Stage::Compute};
  f.Add(IR::Opcode::IsHelperInvocation);
  f.program.info.uses_int64 = true;
  f.program.info.uses_fp64 = true;
  size_t count = 0;
#define OPCODE(name, ...) IR::Opcode::name,
  constexpr std::array opcodes{
#include "shader_recompiler/frontend/ir/opcodes.inc"
  };
#undef OPCODE
  for (const auto op : opcodes) {
    const auto name = IR::NameOf(op);
    if (!name.starts_with("FP") && !name.starts_with("Convert") &&
        !name.starts_with("Composite") && !name.starts_with("Select") &&
        !name.starts_with("BitCast"))
      continue;
    std::vector<IR::Value> args;
    for (size_t i = 0; i < IR::NumArgsOf(op); ++i)
      args.push_back(Argument(f, IR::ArgTypeOf(op, i)));
    switch (args.size()) {
    case 0:
      f.Add(op);
      break;
    case 1:
      f.Add(op, {args[0]});
      break;
    case 2:
      f.Add(op, {args[0], args[1]});
      break;
    case 3:
      f.Add(op, {args[0], args[1], args[2]});
      break;
    case 4:
      f.Add(op, {args[0], args[1], args[2], args[3]});
      break;
    default:
      throw std::runtime_error("Unsupported numeric fixture arity");
    }
    ++count;
  }
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "nx_half(");
  Require(Backend::HLSL::RequiredShaderModel(f.program) == 60u,
          "Non-fragment helper queries require no pixel intrinsic");
  Contains(text, "double4");
  Contains(text, "nx_rsqrt64(");
  Require(count == 189, "Numeric coverage unexpectedly decreased");
  std::cout << "Numeric opcodes emitted: " << count << '\n';
  std::ofstream("hlsl_numeric.hlsl") << text;
}
void GlobalAndRegisterState() {
  Fixture f{Stage::Compute};
  f.program.info.uses_global_memory = true;
  f.program.info.uses_int64 = true;
  f.program.info.constant_buffer_descriptors.push_back(
      {.index = 0, .count = 1});
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  const auto address = IR::Value{u64{0x1000}};
  const auto value = f.Add(IR::Opcode::LoadGlobalS8, {address});
  f.Add(IR::Opcode::WriteGlobalU16, {address, value});
  f.Add(IR::Opcode::GlobalAtomicIAdd32, {address, value});
  f.Add(IR::Opcode::GlobalAtomicSMin32, {address, value});
  f.Add(IR::Opcode::GlobalAtomicAddF32, {address, IR::Value{1.0f}});
  f.Add(IR::Opcode::SetRegister, {IR::Value{IR::Reg::R0}, value});
  const auto reg = f.Add(IR::Opcode::GetRegister, {IR::Value{IR::Reg::R0}});
  f.Add(IR::Opcode::WriteStorage32, {IR::Value{0u}, IR::Value{0u}, reg});
  f.Add(IR::Opcode::SetPred, {IR::Value{IR::Pred::P0}, IR::Value{true}});
  f.Add(IR::Opcode::GetPred, {IR::Value{IR::Pred::P0}});
  f.Add(IR::Opcode::GetPred, {IR::Value{IR::Pred::PT}});
  f.Add(IR::Opcode::SetGotoVariable, {IR::Value{17u}, IR::Value{true}});
  f.Add(IR::Opcode::GetGotoVariable, {IR::Value{17u}});
  Bindings bindings;
  for (size_t i = 0; i < std::size(IR::Detail::META_TABLE); ++i) {
    const auto op = static_cast<IR::Opcode>(i);
    if (!IR::NameOf(op).starts_with("GlobalAtomic"))
      continue;
    const auto operand = Argument(f, IR::ArgTypeOf(op, 1));
    f.Add(op, {address, operand});
  }
  Profile profile{};
  profile.support_int64_atomics = true;
  const auto text = f.Emit(bindings, profile);
  Contains(text, "nx_global_load_subword(");
  Contains(text, "nx_global_find(address, bits / 8u, index, offset)");
  Contains(text, "nx_global_write_subword(");
  // One window search per access, not one block per window.
  Contains(text, "[loop] for (uint i = 0u; i < 1u; ++i)");
  Contains(text, "uint2(cbuf0[o >> 4].x & 4294967295u, cbuf0[o >> 4].y)");
  Contains(text, "ssbo[NonUniformResourceIndex(nx_global_index)].InterlockedAdd(");
  Require(text.find("nx_global_matched") == text.npos, "unrolled global windows");
  Contains(text, "nx_regs[0]");
  Contains(text, "nx_goto_17");
  std::ofstream("hlsl_global.hlsl") << text;
}
void GlobalWindowLayouts() {
  // Windows in two constant buffers, one at an 8-byte offset, read through nx_cbuf_load.
  Fixture mixed{Stage::Compute};
  mixed.program.info.uses_global_memory = true;
  mixed.program.info.uses_int64 = true;
  for (u32 index : {0u, 1u})
    mixed.program.info.constant_buffer_descriptors.push_back(
        {.index = index, .count = 1});
  mixed.program.info.storage_buffers_descriptors.push_back(
      {.cbuf_index = 0, .cbuf_offset = 0x110, .count = 1, .is_written = true});
  mixed.program.info.storage_buffers_descriptors.push_back(
      {.cbuf_index = 1, .cbuf_offset = 0x8, .count = 1, .is_written = true});
  const auto address = IR::Value{u64{0x1000}};
  mixed.Add(IR::Opcode::WriteGlobal32,
            {address, mixed.Add(IR::Opcode::LoadGlobal32, {address})});
  Bindings bindings;
  const auto text = mixed.Emit(bindings);
  Contains(text, "RWByteAddressBuffer ssbo[2] : register(u0, space0)");
  Contains(text, "static const uint nx_global_offsets[2] = {272u, 8u};");
  Contains(text, "static const uint nx_global_cbufs[2] = {0u, 1u};");
  Contains(text, "nx_cbuf_load(nx_global_cbufs[i], o + 12u)");
  std::ofstream("hlsl_global_windows.hlsl") << text;
  // Without a window, global memory reads zero and declares no buffer.
  Fixture none{Stage::Compute};
  none.program.info.uses_global_memory = true;
  none.program.info.uses_int64 = true;
  none.Add(IR::Opcode::WriteGlobal32,
           {address, none.Add(IR::Opcode::LoadGlobal32, {address})});
  none.Add(IR::Opcode::GlobalAtomicIAdd32, {address, IR::Value{1u}});
  Bindings empty;
  const auto stub = none.Emit(empty);
  Contains(stub, "uint nx_global_load32(uint64_t address) { return (uint)0; }");
  Require(stub.find("ssbo") == stub.npos, "a buffer declared without windows");
}
void SparseResidency() {
  Fixture f{Stage::Fragment};
  f.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .count = 1});
  const auto coords = Argument(f, IR::Type::F32x2);
  IR::TextureInstInfo flags{};
  flags.type.Assign(TextureType::Color2D);
  const auto sample = f.Add(IR::Opcode::ImageSampleImplicitLod,
                            {IR::Value{0u}, coords, {}, {}}, flags.raw);
  f.Add(IR::Opcode::GetSparseFromOp, {sample});
  Bindings bindings;
  Contains(f.Emit(bindings), "= true;");
}
void Geometry() {
  Fixture f{Stage::Geometry};
  f.program.output_topology = OutputTopology::TriangleStrip;
  f.program.output_vertices = 3;
  f.program.invocations = 1;
  f.program.info.loads.Set(IR::Attribute::PositionX);
  f.program.info.stores.Set(IR::Attribute::PositionX);
  const auto x = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::PositionX}, IR::Value{2u}});
  f.Add(IR::Opcode::SetAttribute,
        {IR::Value{IR::Attribute::PositionX}, x, IR::Value{0u}});
  f.Add(IR::Opcode::InvocationId);
  f.Add(IR::Opcode::EmitVertex, {IR::Value{0u}});
  f.Add(IR::Opcode::EndPrimitive, {IR::Value{0u}});
  RuntimeInfo runtime{};
  runtime.input_topology = InputTopology::Triangles;
  Bindings bindings;
  const auto text = f.Emit(bindings, {}, runtime);
  Contains(text, "inputs[2u].position.x");
  Contains(text, "[maxvertexcount(3)]");
  Contains(text, "inout TriangleStream<Output>");
  Contains(text, "nx_stream.Append(nx_vertex)");
  Contains(text, "nx_stream.RestartStrip()");
  std::ofstream("hlsl_geometry.hlsl") << text;
}
void NativeWideAtomics() {
  Fixture f{Stage::Compute};
  f.program.info.uses_int64 = true;
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  f.Add(IR::Opcode::StorageAtomicIAdd64,
        {IR::Value{0u}, IR::Value{0u}, IR::Value{u64{1}}});
  f.Add(IR::Opcode::StorageAtomicSMin64,
        {IR::Value{0u}, IR::Value{8u}, IR::Value{u64{1}}});
  Profile profile{};
  profile.support_int64_atomics = true;
  Bindings bindings;
  const auto text = f.Emit(bindings, profile);
  Contains(text, "Load<uint64_t>");
  Contains(text, "InterlockedCompareExchange");
  std::ofstream("hlsl_atomic64.hlsl") << text;
  Bindings fallback_bindings;
  const auto fallback = f.Emit(fallback_bindings);
  Contains(fallback, ".InterlockedAdd(");
  Contains(fallback, ".InterlockedCompareExchange(");
  Require(fallback.find("InterlockedCompareExchange64") == fallback.npos,
          "Fallback used native atomics");
  std::ofstream("hlsl_atomic64_fallback.hlsl") << fallback;
  Bindings failed{.storage_buffer = 7};
  Profile disabled{};
  disabled.hlsl_emulate_int64_atomics = false;
  try {
    f.Emit(failed, disabled);
    throw std::runtime_error("Disabled wide atomics accepted");
  } catch (const NotImplementedException &error) {
    Contains(error.what(), "SM 6.6");
  }
  Require(failed.storage_buffer == 7, "Failure advanced bindings");
  // Arceus fails in a fragment shader, not a compute shader.
  Fixture fragment{Stage::Fragment};
  fragment.program.info.uses_int64 = true;
  fragment.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  for (auto op :
       {IR::Opcode::StorageAtomicIAdd64, IR::Opcode::StorageAtomicSMin64,
        IR::Opcode::StorageAtomicUMin64, IR::Opcode::StorageAtomicSMax64,
        IR::Opcode::StorageAtomicUMax64, IR::Opcode::StorageAtomicAnd64,
        IR::Opcode::StorageAtomicOr64, IR::Opcode::StorageAtomicXor64,
        IR::Opcode::StorageAtomicExchange64}) {
    fragment.Add(op,
                 {IR::Value{0u}, IR::Value{0u}, IR::Value{u64{0x1ffffffff}}});
  }
  Bindings native_fragment, fallback_fragment;
  std::ofstream("hlsl_fragment_atomic64_native.hlsl")
      << fragment.Emit(native_fragment, profile);
  std::ofstream("hlsl_fragment_atomic64_fallback.hlsl")
      << fragment.Emit(fallback_fragment);
}
void MixedSharedMemory() {
  Fixture f{Stage::Compute};
  f.program.shared_memory_size = 128;
  f.program.info.uses_int64 = true;
  const auto pair = Argument(f, IR::Type::U32x2);
  f.Add(IR::Opcode::SharedAtomicExchange64, {IR::Value{0u}, IR::Value{u64{1}}});
  f.Add(IR::Opcode::SharedAtomicExchange32x2, {IR::Value{8u}, pair});
  for (size_t i = 0; i < std::size(IR::Detail::META_TABLE); ++i) {
    const auto op = static_cast<IR::Opcode>(i);
    const auto name = IR::NameOf(op);
    if (!name.starts_with("LoadShared") && !name.starts_with("WriteShared") &&
        !(name.starts_with("SharedAtomic") && name.ends_with("32")))
      continue;
    if (IR::NumArgsOf(op) == 1) {
      f.Add(op, {IR::Value{0u}});
    } else {
      const auto value = Argument(f, IR::ArgTypeOf(op, 1));
      f.Add(op, {IR::Value{0u}, value});
    }
  }
  Profile profile{};
  profile.support_shared_int64_atomics = true;
  Bindings bindings;
  const auto text = f.Emit(bindings, profile);
  Contains(text, "groupshared uint64_t smem64[16]");
  Contains(text, "nx_shared_write_subword(");
  Contains(text, "nx_shared_atomic32(");
  Require(text.find("smem[") == text.npos,
          "Mixed-width shared accesses use different backing arrays");
  Require(Backend::HLSL::RequiredShaderModel(f.program) == 66u,
          "Shared wide atomics need SM 6.6");
  std::ofstream("hlsl_shared64.hlsl") << text;
}
void PixelHelperAndAlphaTest() {
  Fixture f{Stage::Fragment};
  f.program.info.stores_frag_color[0] = true;
  f.Add(IR::Opcode::IsHelperInvocation);
  f.Add(IR::Opcode::SetFragColor,
        {IR::Value{0u}, IR::Value{3u}, IR::Value{0.75f}});
  RuntimeInfo runtime{};
  runtime.alpha_test_func = CompareFunction::Greater;
  runtime.alpha_test_reference = 0.5f;
  runtime.xfb_count = 1;
  runtime.dual_source_blend = true;
  Bindings bindings;
  const auto text = f.Emit(bindings, {}, runtime);
  Contains(text, "IsHelperLane()");
  Contains(text, "if (!(nx_alpha > asfloat(");
  Require(Backend::HLSL::RequiredShaderModel(f.program) == 66u,
          "Helper queries need SM 6.6");
  std::ofstream("hlsl_helper.hlsl") << text;
}
void UnsupportedRollback() {
  Fixture f{Stage::TessellationControl};
  f.program.info.constant_buffer_descriptors.push_back(
      {.index = 0, .count = 1});
  Bindings bindings{.uniform_buffer = 7};
  try {
    f.Emit(bindings);
    throw std::runtime_error("Unlinked hull stage silently accepted");
  } catch (const NotImplementedException &error) {
    Contains(error.what(), "stage linkage");
  }
  Require(bindings.uniform_buffer == 7, "Failure advanced bindings");
}
void PackedSignedAttributes() {
  // Maxwell A2B10G10R10 SNorm/SInt vertex formats have no D3D12 IA spelling; the
  // input assembler fetches R10G10B10A2_UINT and the shader decodes each component.
  Fixture f{Stage::VertexB};
  f.program.info.loads.Set(IR::Attribute::Generic0X);
  f.program.info.loads.Set(IR::Attribute::Generic0W);
  f.program.info.loads.Set(IR::Attribute::Generic1Y);
  f.program.info.stores.Set(IR::Attribute::PositionX);
  const auto x = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::Generic0X}, IR::Value{0u}});
  const auto w = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::Generic0W}, IR::Value{0u}});
  const auto y = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::Generic1Y}, IR::Value{0u}});
  const auto sum = f.Add(IR::Opcode::FPAdd32, {x, w});
  const auto all = f.Add(IR::Opcode::FPAdd32, {sum, y});
  f.Add(IR::Opcode::SetAttribute,
        {IR::Value{IR::Attribute::PositionX}, all, IR::Value{0u}});
  RuntimeInfo runtime{};
  runtime.generic_input_types[0] = AttributeType::SignedNorm10;
  runtime.generic_input_types[1] = AttributeType::SignedInt10;
  Bindings bindings;
  const auto text = f.Emit(bindings, {}, runtime);
  Contains(text, "uint4 attr0 : TEXCOORD0");
  Contains(text, "uint4 attr1 : TEXCOORD1");
  Contains(text, "max(float((int(input.attr0.x << 22u) >> 22u)) * (1.0f / 511.0f), -1.0f)");
  Contains(text, "max(float((int(input.attr0.w << 30u) >> 30u)) * 1.0f, -1.0f)");
  Contains(text, "asfloat((int(input.attr1.y << 22u) >> 22u))");
  std::ofstream("hlsl_packed10.hlsl") << text;
}
} // namespace
// A return nested in structured control flow must become a flag plus guards: DXC keeps it as an
// extra exit edge and the Xbox driver's compile of such a shader runs out of memory.
void NestedReturns() {
  using Node = IR::AbstractSyntaxNode;
  Fixture f{Stage::Compute};
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  IR::Block inner{f.pool};
  IR::Block after{f.pool};
  f.program.blocks.push_back(&inner);
  f.program.blocks.push_back(&after);
  const auto condition = f.Add(IR::Opcode::ConditionRef, {IR::Value{true}});
  inner.AppendNewInst(IR::Opcode::WriteStorage32,
                      {IR::Value{0u}, IR::Value{0u}, IR::Value{1u}});
  after.AppendNewInst(IR::Opcode::WriteStorage32,
                      {IR::Value{0u}, IR::Value{4u}, IR::Value{2u}});
  const auto push = [&](Node node) { f.program.syntax_list.push_back(node); };
  // { if (c) { { if (c) { inner; return; } } after; return; } }
  push({.data = {.if_node = {.cond = IR::U1{condition}}},
        .type = Node::Type::If});
  push({.data = {.if_node = {.cond = IR::U1{condition}}},
        .type = Node::Type::If});
  push({.data = {.block = &inner}, .type = Node::Type::Block});
  push({.type = Node::Type::Return});
  push({.type = Node::Type::EndIf});
  push({.data = {.block = &after}, .type = Node::Type::Block});
  push({.type = Node::Type::EndIf});
  push({.type = Node::Type::Return});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "bool nx_returned = false;");
  Contains(text, "nx_returned = true;");
  Contains(text, "if (!nx_returned) {");
  const auto count = [&](std::string_view token) {
    size_t n = 0;
    for (auto at = text.find(token); at != text.npos;
         at = text.find(token, at + 1))
      ++n;
    return n;
  };
  // Only the top-level return and the function epilogue may leave the function.
  Require(count("return;") == 2, "Nested returns must not leave the function");
  // The guard opened after the inner region closes before the outer region does.
  Require(count("if (!nx_returned) {") == 2 && count("{") == count("}"),
          "Return guards must be balanced");
  std::ofstream("hlsl_returns.hlsl") << text;
  Bindings repeated;
  Require(text == f.Emit(repeated), "Return lowering modified IR");
  // A program whose only returns are at the top level keeps the plain form.
  Fixture plain{Stage::Compute};
  plain.program.syntax_list.push_back({.type = Node::Type::Return});
  Bindings plain_bindings;
  Require(plain.Emit(plain_bindings).find("nx_returned") == std::string::npos,
          "Top-level returns must not use the flag");
}
void SingleUseInlining() {
    {
        Fixture f{Stage::Compute};
        f.program.local_memory_size = 16;
        const auto loaded = f.Add(IR::Opcode::LoadLocal, {IR::Value{0u}});
        f.Add(IR::Opcode::WriteLocal, {IR::Value{0u}, IR::Value{42u}});
        const auto sum = f.Add(IR::Opcode::IAdd32, {loaded, IR::Value{1u}});
        f.Add(IR::Opcode::WriteLocal, {IR::Value{4u}, sum});
        Bindings bindings;
        const auto text = f.Emit(bindings);
        Contains(text, "uint v0 = 0;");
        Contains(text, "v0 = lmem[0u];");
        Require(text.find("v1 =") == text.npos,
                "Single-use arithmetic should be emitted at its consumer");
        Require(text.find("uint v1") == text.npos, "Eliminated temporaries must not be declared");
    }
    {
        Fixture f{Stage::VertexB};
        f.program.info.loads.Set(IR::Attribute::Generic0X);
        f.program.info.stores.Set(IR::Attribute::PositionX);
        const auto input =
            f.Add(IR::Opcode::GetAttribute, {IR::Value{IR::Attribute::Generic0X}, IR::Value{0u}});
        const auto alias = f.Add(IR::Opcode::Identity, {input});
        const auto sum = f.Add(IR::Opcode::FPAdd32, {alias, IR::Value{2.0f}});
        sum.InstRecursive()->SetFlags(IR::FpControl{.no_contraction = true});
        f.Add(IR::Opcode::SetAttribute, {IR::Value{IR::Attribute::PositionX}, sum, IR::Value{0u}});
        f.Add(IR::Opcode::SetAttribute,
              {IR::Value{IR::Attribute::PositionY}, alias, IR::Value{0u}});
        Bindings bindings;
        const auto text = f.Emit(bindings);
        Contains(text, "float v0 = 0;");
        Contains(text, "precise float v1 = 0;");
        Contains(text, "v0 = input.attr0.x;");
        Require(text.find("v1=") != text.npos,
                "Precise producers and recursively shared aliases must remain named");
    }
}
void GroupedDeclarations() {
    Fixture f{Stage::Compute};
    f.program.local_memory_size = 16;
    const auto first = f.Add(IR::Opcode::LoadLocal, {IR::Value{0u}});
    const auto second = f.Add(IR::Opcode::LoadLocal, {IR::Value{4u}});
    f.Add(IR::Opcode::WriteLocal, {IR::Value{8u}, first});
    f.Add(IR::Opcode::WriteLocal, {IR::Value{12u}, second});
    Bindings bindings;
    Contains(f.Emit(bindings), "uint v0 = 0, v1 = 0;\n");
}
void PointSpriteCoordinate() {
  // D3D12 has no gl_PointCoord and never expands points, so the single fragment of a 1px point
  // reads the sprite centre. Fragment shaders reading PointSpriteS/T used to fail their whole
  // pipeline with "HLSL attribute PointSpriteT is not implemented".
  Fixture f{Stage::Fragment};
  f.program.info.stores_frag_color[0] = true;
  f.program.info.loads.Set(IR::Attribute::PointSpriteS);
  f.program.info.loads.Set(IR::Attribute::PointSpriteT);
  const auto s = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::PointSpriteS}, IR::Value{0u}});
  const auto t = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::PointSpriteT}, IR::Value{0u}});
  f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, s});
  f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{1u}, t});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "output.color0.x = ");
  Contains(text, "output.color0.y = ");
  Contains(text, "0.5f");
  Require(text.find("PointSprite") == text.npos && text.find("pointcoord") == text.npos,
          "A 1px point has no sprite input to declare");
  std::ofstream("hlsl_pointsprite.hlsl") << text;
}
void UndeclaredDescriptors() {
  // The IR names resources by index into the lists built from Info. A stale index surfaced as the
  // bare "invalid vector subscript" of std::vector::at, which hid what was looked up and where.
  const auto expect = [](Fixture &f, std::initializer_list<std::string_view> tokens) {
    Bindings bindings{.texture = 5};
    try {
      f.Emit(bindings);
    } catch (const LogicError &error) {
      for (const auto token : tokens)
        Contains(error.what(), token);
      Require(bindings.texture == 5, "Failure advanced bindings");
      return;
    }
    throw std::runtime_error("Undeclared descriptor silently accepted");
  };
  {
    Fixture f{Stage::Fragment};
    f.program.info.stores_frag_color[0] = true;
    f.program.info.texture_buffer_descriptors.push_back({.count = 1});
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Buffer);
    flags.descriptor_index.Assign(3);
    f.Add(IR::Opcode::ImageFetch,
          {IR::Value{0u}, IR::Value{1u}, {}, IR::Value{0u}, {}}, flags.raw);
    expect(f, {"texture buffer descriptor 3", "stage Fragment", "declares 1"});
  }
  {
    // Texture buffers are declared without a sampler; a sampler query for one must not wander
    // into (or past the end of) the sampled-texture list.
    Fixture f{Stage::VertexB};
    f.program.info.texture_buffer_descriptors.push_back({.count = 1});
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Buffer);
    f.Add(IR::Opcode::ImageQueryLod, {IR::Value{0u}, IR::Value{0.5f}}, flags.raw);
    expect(f, {"texture sampler descriptor 0", "stage VertexB", "declares 0"});
  }
}
void ResolutionMasks() {
    for (const auto stage : {Stage::VertexB, Stage::Geometry, Stage::Fragment, Stage::Compute}) {
        Fixture f{stage};
        f.program.output_vertices = 3;
        f.program.invocations = 1;
        f.program.info.texture_descriptors.push_back({.type = TextureType::Color2D, .count = 1});
        f.program.info.image_descriptors.push_back({.type = TextureType::Color2D,
                                                    .format = ImageFormat::R32_UINT,
                                                    .is_integer = true,
                                                    .count = 1});
        f.Add(IR::Opcode::IsTextureScaled, {IR::Value{0u}});
        f.Add(IR::Opcode::IsImageScaled, {IR::Value{0u}});
        Bindings bindings;
        bindings.storage_buffer = 3;
        const auto source = f.Emit(bindings);
        Contains(source, stage == Stage::Compute    ? "nx_texture_scaling_mask &"
                         : stage == Stage::VertexB  ? "nx_reserved0 &"
                         : stage == Stage::Geometry ? "nx_reserved1[2] &"
                                                    : "nx_reserved1[3] &");
        Contains(source, "nx_image_scaling_mask & (1u << 3)");
    }
}
int main(int argc, char **argv) {
  try {
      if (argc > 1 && std::string_view{argv[1]} == "seam-position") {
          SeamPositionOutput();
          return 0;
      }
    // `hlsl-ir-tests point-sprite-descriptors` runs only the point sprite and descriptor checks.
    if (argc > 1 && std::string_view{argv[1]} == "point-sprite-descriptors") {
      PointSpriteCoordinate();
      UndeclaredDescriptors();
      return 0;
    }
    ResolutionMasks();
    SeamPositionOutput();
    GroupedDeclarations();
    SingleUseInlining();
      Vertex();
      DirectClipDistances();
      IntegerTextureSample();
      PackedSignedAttributes();
      Fragment();
      MultisampleFetch();
      Compute();
      ControlFlowAndWaves();
      ResourceOrderAndCompare();
      StorageImageFormats();
      IntegerTypelessImage();
      ArithmeticAndPseudoResults();
      ChainedResourceBindings();
      NumericCoverage();
      GlobalAndRegisterState();
      GlobalWindowLayouts();
      SparseResidency();
      Geometry();
      NativeWideAtomics();
      MixedSharedMemory();
      PixelHelperAndAlphaTest();
      UnsupportedRollback();
      NestedReturns();
      PointSpriteCoordinate();
      UndeclaredDescriptors();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
