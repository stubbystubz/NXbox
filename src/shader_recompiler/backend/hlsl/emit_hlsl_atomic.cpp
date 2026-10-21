// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/runtime_info.h"
namespace Shader::Backend::HLSL {
namespace {
// The window that GlobalMemoryAccess found for the access it opened.
constexpr std::string_view GLOBAL_INDEX = "NonUniformResourceIndex(nx_global_index)";
void Atomic64(EmitContext& ctx, IR::Inst& inst, std::string_view resource, std::string_view offset,
              std::string_view value) {
    if (!ctx.profile.support_int64_atomics && !ctx.profile.hlsl_emulate_int64_atomics)
        throw NotImplementedException(
            "HLSL {} requires SM 6.6 and D3D12 native buffer int64 atomics", inst.GetOpcode());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    std::string operation;
    switch (inst.GetOpcode()) {
    case IR::Opcode::StorageAtomicIAdd64:
    case IR::Opcode::GlobalAtomicIAdd64:
        operation = "old_value + value";
        break;
    case IR::Opcode::StorageAtomicSMin64:
    case IR::Opcode::GlobalAtomicSMin64:
        operation = "uint64_t(min(int64_t(old_value), int64_t(value)))";
        break;
    case IR::Opcode::StorageAtomicUMin64:
    case IR::Opcode::GlobalAtomicUMin64:
        operation = "min(old_value, value)";
        break;
    case IR::Opcode::StorageAtomicSMax64:
    case IR::Opcode::GlobalAtomicSMax64:
        operation = "uint64_t(max(int64_t(old_value), int64_t(value)))";
        break;
    case IR::Opcode::StorageAtomicUMax64:
    case IR::Opcode::GlobalAtomicUMax64:
        operation = "max(old_value, value)";
        break;
    case IR::Opcode::StorageAtomicAnd64:
    case IR::Opcode::GlobalAtomicAnd64:
        operation = "old_value & value";
        break;
    case IR::Opcode::StorageAtomicOr64:
    case IR::Opcode::GlobalAtomicOr64:
        operation = "old_value | value";
        break;
    case IR::Opcode::StorageAtomicXor64:
    case IR::Opcode::GlobalAtomicXor64:
        operation = "old_value ^ value";
        break;
    case IR::Opcode::StorageAtomicExchange64:
    case IR::Opcode::GlobalAtomicExchange64:
        operation = "value";
        break;
    default:
        throw LogicError("Invalid HLSL int64 atomic {}", inst.GetOpcode());
    }
    if (!ctx.profile.support_int64_atomics) {
        // Each word is atomic, but the pair is not a linearizable 64-bit transaction.
        // In particular, readers can observe the low word before the carry reaches high.
        if (inst.GetOpcode() == IR::Opcode::StorageAtomicIAdd64 ||
            inst.GetOpcode() == IR::Opcode::GlobalAtomicIAdd64) {
            ctx.Add("{{ uint64_t value = {2}; uint lo, hi; "
                    "{0}.InterlockedAdd({1}, uint(value), lo); "
                    "uint carry = uint(lo + uint(value) < lo); "
                    "{0}.InterlockedAdd(({1}) + 4u, uint(value >> 32) + carry, hi); "
                    "{3} = (uint64_t(hi) << 32) | uint64_t(lo); }}",
                    resource, offset, value, result);
        } else {
            // Retry each word separately, evaluating min/max in 64 bits. Keep the
            // original low word for the high comparison; concurrent updates can still tear.
            ctx.Add("{{ uint64_t value = {2}; uint2 original = uint2(0u, 0u); "
                    "for (uint word = 0u; word < 2u; ++word) {{ "
                    "uint address = ({1}) + word * 4u; uint expected; uint previous; "
                    "{0}.InterlockedAdd(address, 0u, expected); "
                    "do {{ uint other; {0}.InterlockedAdd(({1}) + (1u - word) * 4u, 0u, other); "
                    "uint64_t old_value = word == 0u ? "
                    "((uint64_t(other) << 32) | expected) : "
                    "((uint64_t(expected) << 32) | original.x); "
                    "uint64_t new_value = {3}; "
                    "uint replacement = uint(new_value >> (word * 32u)); "
                    "{0}.InterlockedCompareExchange(address, expected, replacement, previous); "
                    "if (previous == expected) break; expected = previous; }} while (true); "
                    "original[word] = expected; }} "
                    "{4} = (uint64_t(original.y) << 32) | original.x; }}",
                    resource, offset, value, operation, result);
        }
        return;
    }
    ctx.Add("{{ uint64_t old_value = {0}.Load<uint64_t>({1}); uint64_t previous; "
            "uint64_t value = {2}; do {{ uint64_t new_value = {3}; "
            "{0}.InterlockedCompareExchange64({1}, old_value, new_value, previous); "
            "if (previous == old_value) break; old_value = previous; }} while (true); {4} = "
            "old_value; }}",
            resource, offset, value, operation, result);
}
} // namespace

void EmitSharedAtomicIAdd32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 0u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedAdd(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicSMin32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 1u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = "
            "uint(min(int(old_value), int({}))); InterlockedCompareExchange(smem[{} >> 2], "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            o, value, o, result);
}

void EmitSharedAtomicUMin32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 2u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedMin(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicSMax32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 3u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = "
            "uint(max(int(old_value), int({}))); InterlockedCompareExchange(smem[{} >> 2], "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            o, value, o, result);
}

void EmitSharedAtomicUMax32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                            std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 4u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedMax(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicInc32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                           std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 5u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = old_value >= "
            "{} ? 0u : old_value + 1u; InterlockedCompareExchange(smem[{} >> 2], old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = old_value; }}",
            o, value, o, result);
}

void EmitSharedAtomicDec32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                           std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 6u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = smem[{} >> 2]; uint previous; do {{ uint new_value = old_value == "
            "0u || old_value > {} ? {} : old_value - 1u; InterlockedCompareExchange(smem[{} >> 2], "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            o, value, value, o, result);
}

void EmitSharedAtomicAnd32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                           std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 7u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedAnd(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicOr32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                          std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 8u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedOr(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicXor32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                           std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 9u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedXor(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitSharedAtomicExchange32(EmitContext& ctx, IR::Inst& inst, std::string_view pointer_offset,
                                std::string_view value) {
    if (ctx.uses_shared_wide) {
        ctx.AddU32("{} = nx_shared_atomic32({}, {}, 10u);", inst, pointer_offset, value);
        return;
    }

    const auto o = std::string(pointer_offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedExchange(smem[{} >> 2], {}, {});", o, value, result);
}

void EmitStorageAtomicIAdd32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedAdd({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicSMin32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = "
            "uint(min(int(old_value), int({}))); {}.InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = old_value; }}",
            resource, o, value, resource, o, result);
}

void EmitStorageAtomicUMin32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedMin({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicSMax32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = "
            "uint(max(int(old_value), int({}))); {}.InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = old_value; }}",
            resource, o, value, resource, o, result);
}

void EmitStorageAtomicUMax32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedMax({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicInc32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = old_value >= "
            "{} ? 0u : old_value + 1u; {}.InterlockedCompareExchange({}, old_value, new_value, "
            "previous); if (previous == old_value) break; old_value = previous; }} while (true); "
            "{} = old_value; }}",
            resource, o, value, resource, o, result);
}

void EmitStorageAtomicDec32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = old_value == "
            "0u || old_value > {} ? {} : old_value - 1u; {}.InterlockedCompareExchange({}, "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            resource, o, value, value, resource, o, result);
}

void EmitStorageAtomicAnd32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedAnd({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicOr32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                           const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedOr({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicXor32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedXor({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicExchange32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                                 const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto resource = fmt::format("ssbo[{}]", binding.U32());
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{}.InterlockedExchange({}, {}, {});", resource, o, value, result);
}

void EmitStorageAtomicAddF32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::F32);
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "asuint(asfloat(old_value) + {}); ssbo[{}].InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = asfloat(old_value); }}",
            binding.U32(), o, value, binding.U32(), o, result);
}
void EmitStorageAtomicAddF16x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(unpackHalf2x16(old_value) + {}); ssbo[{}].InterlockedCompareExchange({}, "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicMinF16x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(min(unpackHalf2x16(old_value), {})); "
            "ssbo[{}].InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicMaxF16x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(max(unpackHalf2x16(old_value), {})); "
            "ssbo[{}].InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicAddF32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(unpackHalf2x16(old_value) + {}); ssbo[{}].InterlockedCompareExchange({}, "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicMinF32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(min(unpackHalf2x16(old_value), {})); "
            "ssbo[{}].InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicMaxF32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(max(unpackHalf2x16(old_value), {})); "
            "ssbo[{}].InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            binding.U32(), o, value, binding.U32(), o, result);
}

void EmitStorageAtomicIAdd32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add(
        "ssbo[{}].InterlockedAdd({}, {}.x, {}.x); ssbo[{}].InterlockedAdd({} + 4u, {}.y, {}.y);",
        binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicSMin32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add(
        "for (uint component = 0u; component < 2u; ++component) {{ uint byte_offset = {} + "
        "component * 4u; uint old_value = ssbo[{}].Load(byte_offset); uint previous; do {{ uint "
        "new_value = uint(min(int(old_value), int({}[component]))); "
        "ssbo[{}].InterlockedCompareExchange(byte_offset, old_value, new_value, previous); if "
        "(previous == old_value) break; old_value = previous; }} while (true); {}[component] = "
        "old_value; }}",
        o, binding.U32(), value, binding.U32(), result);
}

void EmitStorageAtomicUMin32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add(
        "ssbo[{}].InterlockedMin({}, {}.x, {}.x); ssbo[{}].InterlockedMin({} + 4u, {}.y, {}.y);",
        binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicSMax32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add(
        "for (uint component = 0u; component < 2u; ++component) {{ uint byte_offset = {} + "
        "component * 4u; uint old_value = ssbo[{}].Load(byte_offset); uint previous; do {{ uint "
        "new_value = uint(max(int(old_value), int({}[component]))); "
        "ssbo[{}].InterlockedCompareExchange(byte_offset, old_value, new_value, previous); if "
        "(previous == old_value) break; old_value = previous; }} while (true); {}[component] = "
        "old_value; }}",
        o, binding.U32(), value, binding.U32(), result);
}

void EmitStorageAtomicUMax32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                               const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add(
        "ssbo[{}].InterlockedMax({}, {}.x, {}.x); ssbo[{}].InterlockedMax({} + 4u, {}.y, {}.y);",
        binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicAnd32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                              const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add(
        "ssbo[{}].InterlockedAnd({}, {}.x, {}.x); ssbo[{}].InterlockedAnd({} + 4u, {}.y, {}.y);",
        binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicOr32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add("ssbo[{}].InterlockedOr({}, {}.x, {}.x); ssbo[{}].InterlockedOr({} + 4u, {}.y, {}.y);",
            binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicXor32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                              const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add(
        "ssbo[{}].InterlockedXor({}, {}.x, {}.x); ssbo[{}].InterlockedXor({} + 4u, {}.y, {}.y);",
        binding.U32(), o, value, result, binding.U32(), o, value, result);
}

void EmitStorageAtomicExchange32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                                   const IR::Value& offset, std::string_view value) {
    const auto o = ctx.var_alloc.Consume(offset);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x2);
    ctx.Add(
        "ssbo[{}].InterlockedExchange({}, {}.x, {}.x); ssbo[{}].InterlockedExchange({} + 4u, {}.y, "
        "{}.y);",
        binding.U32(), o, value, result, binding.U32(), o, value, result);
}
void EmitGlobalAtomicIAdd32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{}.InterlockedAdd({}, {}, {});", resource, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicSMin32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = "
            "uint(min(int(old_value), int({}))); {}.InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = old_value; }}",
            resource, o, value, resource, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicUMin32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{}.InterlockedMin({}, {}, {});", resource, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicSMax32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = "
            "uint(max(int(old_value), int({}))); {}.InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = old_value; }}",
            resource, o, value, resource, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicUMax32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{}.InterlockedMax({}, {}, {});", resource, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicInc32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                           std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = old_value >= "
            "{} ? 0u : old_value + 1u; {}.InterlockedCompareExchange({}, old_value, new_value, "
            "previous); if (previous == old_value) break; old_value = previous; }} while (true); "
            "{} = old_value; }}",
            resource, o, value, resource, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicDec32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                           std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{{ uint old_value = {}.Load({}); uint previous; do {{ uint new_value = old_value == "
            "0u || old_value > {} ? {} : old_value - 1u; {}.InterlockedCompareExchange({}, "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            resource, o, value, value, resource, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicAnd32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                           std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{}.InterlockedAnd({}, {}, {});", resource, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicOr32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                          std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{}.InterlockedOr({}, {}, {});", resource, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicXor32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                           std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{}.InterlockedXor({}, {}, {});", resource, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicExchange32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                                std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    const auto resource = fmt::format("ssbo[{}]", GLOBAL_INDEX);
    ctx.Add("{}.InterlockedExchange({}, {}, {});", resource, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicIAdd32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add(
        "ssbo[{}].InterlockedAdd({}, {}.x, {}.x); ssbo[{}].InterlockedAdd({} + 4u, {}.y, {}.y);",
        GLOBAL_INDEX, o, value, result, GLOBAL_INDEX, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicSMin32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add(
        "for (uint component = 0u; component < 2u; ++component) {{ uint byte_offset = {} + "
        "component * 4u; uint old_value = ssbo[{}].Load(byte_offset); uint previous; do {{ uint "
        "new_value = uint(min(int(old_value), int({}[component]))); "
        "ssbo[{}].InterlockedCompareExchange(byte_offset, old_value, new_value, previous); if "
        "(previous == old_value) break; old_value = previous; }} while (true); {}[component] = "
        "old_value; }}",
        o, GLOBAL_INDEX, value, GLOBAL_INDEX, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicUMin32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add(
        "ssbo[{}].InterlockedMin({}, {}.x, {}.x); ssbo[{}].InterlockedMin({} + 4u, {}.y, {}.y);",
        GLOBAL_INDEX, o, value, result, GLOBAL_INDEX, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicSMax32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add(
        "for (uint component = 0u; component < 2u; ++component) {{ uint byte_offset = {} + "
        "component * 4u; uint old_value = ssbo[{}].Load(byte_offset); uint previous; do {{ uint "
        "new_value = uint(max(int(old_value), int({}[component]))); "
        "ssbo[{}].InterlockedCompareExchange(byte_offset, old_value, new_value, previous); if "
        "(previous == old_value) break; old_value = previous; }} while (true); {}[component] = "
        "old_value; }}",
        o, GLOBAL_INDEX, value, GLOBAL_INDEX, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicUMax32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add(
        "ssbo[{}].InterlockedMax({}, {}.x, {}.x); ssbo[{}].InterlockedMax({} + 4u, {}.y, {}.y);",
        GLOBAL_INDEX, o, value, result, GLOBAL_INDEX, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicAnd32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                             std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add(
        "ssbo[{}].InterlockedAnd({}, {}.x, {}.x); ssbo[{}].InterlockedAnd({} + 4u, {}.y, {}.y);",
        GLOBAL_INDEX, o, value, result, GLOBAL_INDEX, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicOr32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add("ssbo[{}].InterlockedOr({}, {}.x, {}.x); ssbo[{}].InterlockedOr({} + 4u, {}.y, {}.y);",
            GLOBAL_INDEX, o, value, result, GLOBAL_INDEX, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicXor32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                             std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add(
        "ssbo[{}].InterlockedXor({}, {}.x, {}.x); ssbo[{}].InterlockedXor({} + 4u, {}.y, {}.y);",
        GLOBAL_INDEX, o, value, result, GLOBAL_INDEX, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicExchange32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                                  std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add(
        "ssbo[{}].InterlockedExchange({}, {}.x, {}.x); ssbo[{}].InterlockedExchange({} + 4u, {}.y, "
        "{}.y);",
        GLOBAL_INDEX, o, value, result, GLOBAL_INDEX, o, value, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicAddF32(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 4))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "asuint(asfloat(old_value) + {}); ssbo[{}].InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = asfloat(old_value); }}",
            GLOBAL_INDEX, o, value, GLOBAL_INDEX, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicAddF16x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(unpackHalf2x16(old_value) + {}); ssbo[{}].InterlockedCompareExchange({}, "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            GLOBAL_INDEX, o, value, GLOBAL_INDEX, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicAddF32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(unpackHalf2x16(old_value) + {}); ssbo[{}].InterlockedCompareExchange({}, "
            "old_value, new_value, previous); if (previous == old_value) break; old_value = "
            "previous; }} while (true); {} = old_value; }}",
            GLOBAL_INDEX, o, value, GLOBAL_INDEX, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicMinF16x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(min(unpackHalf2x16(old_value), {})); "
            "ssbo[{}].InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            GLOBAL_INDEX, o, value, GLOBAL_INDEX, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicMinF32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(min(unpackHalf2x16(old_value), {})); "
            "ssbo[{}].InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            GLOBAL_INDEX, o, value, GLOBAL_INDEX, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicMaxF16x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(max(unpackHalf2x16(old_value), {})); "
            "ssbo[{}].InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            GLOBAL_INDEX, o, value, GLOBAL_INDEX, o, result);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicMaxF32x2(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                              std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add("{} = 0;", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    constexpr std::string_view o = "nx_global_offset";
    ctx.Add("{{ uint old_value = ssbo[{}].Load({}); uint previous; do {{ uint new_value = "
            "packHalf2x16(max(unpackHalf2x16(old_value), {})); "
            "ssbo[{}].InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
            "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
            GLOBAL_INDEX, o, value, GLOBAL_INDEX, o, result);
    ctx.Add("}} }}");
}
void EmitStorageAtomicIAdd64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", binding.U32()), ctx.var_alloc.Consume(offset),
             value);
}
void EmitStorageAtomicSMin64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", binding.U32()), ctx.var_alloc.Consume(offset),
             value);
}
void EmitStorageAtomicUMin64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", binding.U32()), ctx.var_alloc.Consume(offset),
             value);
}
void EmitStorageAtomicSMax64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", binding.U32()), ctx.var_alloc.Consume(offset),
             value);
}
void EmitStorageAtomicUMax64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                             const IR::Value& offset, std::string_view value) {
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", binding.U32()), ctx.var_alloc.Consume(offset),
             value);
}
void EmitStorageAtomicAnd64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", binding.U32()), ctx.var_alloc.Consume(offset),
             value);
}
void EmitStorageAtomicOr64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                           const IR::Value& offset, std::string_view value) {
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", binding.U32()), ctx.var_alloc.Consume(offset),
             value);
}
void EmitStorageAtomicXor64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                            const IR::Value& offset, std::string_view value) {
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", binding.U32()), ctx.var_alloc.Consume(offset),
             value);
}
void EmitStorageAtomicExchange64(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                                 const IR::Value& offset, std::string_view value) {
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", binding.U32()), ctx.var_alloc.Consume(offset),
             value);
}
void EmitGlobalAtomicIAdd64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicSMin64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicUMin64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicSMax64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicUMax64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                            std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicInc64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                           std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicDec64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                           std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicAnd64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                           std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicOr64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                          std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicXor64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                           std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitGlobalAtomicExchange64(EmitContext& ctx, IR::Inst& inst, std::string_view address,
                                std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U64);
    ctx.Add("{} = uint64_t(0u);", result);
    if (!ctx.GlobalMemoryAccess(address, 8))
        return;
    Atomic64(ctx, inst, fmt::format("ssbo[{}]", GLOBAL_INDEX), "nx_global_offset", value);
    ctx.Add("}} }}");
}
void EmitSharedAtomicExchange64(EmitContext& ctx, IR::Inst& inst, std::string_view offset,
                                std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add(
        "{{ uint64_t old_value; InterlockedExchange(smem64[{} >> 3], {}, old_value); {} = {}; }}",
        offset, value, result, "old_value");
}
void EmitSharedAtomicExchange32x2(EmitContext& ctx, IR::Inst& inst, std::string_view offset,
                                  std::string_view value) {
    const auto result = ctx.var_alloc.Define(inst, inst.Type());
    ctx.Add(
        "{{ uint64_t old_value; InterlockedExchange(smem64[{} >> 3], {}, old_value); {} = {}; }}",
        offset, fmt::format("packUint2x32({})", value), result, "unpackUint2x32(old_value)");
}
} // namespace Shader::Backend::HLSL
