// SPDX-License-Identifier: GPL-3.0-or-later
// Host stubs surround production methods inserted by test_gpu_frontend_hot_paths.py.

#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <string_view>
#include <utility>
#include <vector>
#include "video_core/gpu_frontend.h"

#include "common/gpu_thread_profile.h"
#include "common/maxwell_method_profile.h"

using u32 = std::uint32_t;
using GPUVAddr = std::uint64_t;
constexpr u32 MacroRegistersStart = 0xE00;
static unsigned assertion_calls;
static unsigned assertion_failures;
#define ASSERT(condition)                                                                          \
    do {                                                                                           \
        ++assertion_calls;                                                                         \
        if (!(condition))                                                                          \
            ++assertion_failures;                                                                  \
    } while (false)

constexpr u32 RegisterIndex(std::string_view field) {
    // REGISTER_INDICES
    return 0;
}
#define MAXWELL3D_REG_INDEX(field) RegisterIndex(#field)

namespace Core {
struct System {};
} // namespace Core
namespace VideoCommon::ShaderDiscovery {
// Discovery tracing is off in these checks; the hook must still compile.
constexpr bool Enabled() {
    return false;
}
inline void Base(unsigned, std::uint64_t, bool) {}
} // namespace VideoCommon::ShaderDiscovery
namespace Settings {
static bool high = true;
bool IsGPULevelHigh() {
    return high;
}
} // namespace Settings

struct Maxwell3D {
    struct Regs {
        static constexpr u32 NUM_REGS = MacroRegistersStart;
        enum class ShadowRamControl { Off, Track, TrackWithFilter, Replay };
        struct {
            u32 offset{}, size{4096};
            GPUVAddr Address() const {
                return 64;
            }
        } const_buffer;
        ShadowRamControl shadow_ram_control{};
        std::array<u32, NUM_REGS> reg_array{};
        struct {
            GPUVAddr Address() const {
                return 0;
            }
        } program_region;
    } regs, shadow_state;
    struct {
        std::bitset<256> flags;
        std::array<std::array<unsigned char, Regs::NUM_REGS>, 2> tables{};
    } dirty;
    struct {
        unsigned calls{};
        void ProcessData(const u32*, u32) {
            ++calls;
        }
    } upload_state;
    struct Memory {
        std::array<u32, 1024> words{};
        std::vector<std::pair<GPUVAddr, size_t>> reads;
        unsigned writes{};
        unsigned GetID() const {
            return 0;
        }
        void WriteBlockCached(GPUVAddr address, const void* input, size_t size) {
            ++writes;
            assert(address + size <= sizeof(words));
            std::memcpy(reinterpret_cast<char*>(words.data()) + address, input, size);
        }
        void ReadBlock(GPUVAddr address, void* out, size_t size) {
            reads.emplace_back(address, size);
            assert(address + size <= sizeof(words));
            std::memcpy(out, reinterpret_cast<const char*>(words.data()) + address, size);
        }
    } memory_manager;
    std::bitset<65535> execution_mask;
    u32 executing_macro{};
    std::vector<u32> macro_params;
    std::vector<std::pair<GPUVAddr, size_t>> macro_segments;
    bool current_dirty{}, current_macro_dirty{};
    GPUVAddr current_dma_segment{};
    unsigned effects{}, bulk_calls{}, sink_calls{}, macro_calls{}, dispatch_calls{};
    bool fail_dispatch{};
    const bool method_batching{Tegra::MaxwellMethodBatchingEnabled()};
    bool captured_dirty{}, refresh{};
    std::vector<u32> captured;
    std::vector<std::pair<GPUVAddr, size_t>> captured_segments;

    bool IsMethodExecutable(u32);
    u32 ProcessShadowRam(u32, u32);
    const bool frontend_fastpath;
    std::vector<std::pair<u32, u32>> method_sink;
    void ConsumeSinkImpl(Core::System&);
    void ProcessDirtyRegisters(u32, u32);
    void ProcessDirtyRegistersImpl(u32, u32);
    void CallMethod(Core::System&, u32, u32, bool);
    u32 MethodProfileKey(u32) const;
    template <bool Profile = false>
    void CallMethodImpl(Core::System&, u32, u32, bool);
    void CallMultiMethod(Core::System&, u32, const u32*, u32, u32);
    void CallMultiMethod(Core::System&, u32, const u32*, u32);
    void ProcessMacro(Core::System&, u32, const u32*, u32, bool);
    void RefreshParametersImpl();
    u32 GetRegisterValue(u32) const;
    void ConsumeSink(Core::System&) {
        ++sink_calls;
    }
    void CallMacroMethod(Core::System&, u32, const std::vector<u32>& parameters) {
        executing_macro = 0;
        if (refresh && current_macro_dirty)
            RefreshParametersImpl();
        ++macro_calls;
        captured = parameters;
        captured_segments = macro_segments;
        captured_dirty = current_macro_dirty;
    }
    void ProcessMethodCall(u32 method, u32, u32, bool) {
        if (fail_dispatch)
            throw 7;
        ++dispatch_calls;
        effects += execution_mask[method];
    }
    void ProcessCBMultiData(const u32*, u32);
    void CallCBDataIncrementing(u32, const u32*, u32);
    void ProcessInlineIndexMultiData(u32, const u32*, u32) {
        ++bulk_calls;
    }
    explicit Maxwell3D(bool fast = Tegra::GpuFrontendFastPathEnabled()) : frontend_fastpath{fast} {
        for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
            execution_mask[i] = IsMethodExecutable(i);
            regs.reg_array[i] = i;
            shadow_state.reg_array[i] = i + 100;
            dirty.tables[0][i] = static_cast<unsigned char>(i);
            dirty.tables[1][i] = static_cast<unsigned char>(i + 1);
        }
    }
};

// METHODS

struct TestSink : Common::GpuProfile::Sink {
    unsigned enters{}, leaves{};
    std::size_t Enter(Common::GpuProfile::Phase) override {
        return enters++;
    }
    void Leave(std::size_t) override {
        ++leaves;
    }
};

struct TestMethodCollector : Common::MaxwellMethodProfile::Collector {
    Common::GpuProfile::Tick clock{};
    Common::GpuProfile::Tick ReadTicks() override {
        return clock++;
    }
};

int main() {
    TestMethodCollector methods;
    if (Common::MaxwellMethodProfile::Enabled())
        Common::MaxwellMethodProfile::collector = &methods;
    Core::System system;
    std::mt19937 random{0x12345678};
    TestSink owner;
    Common::GpuProfile::sink = &owner;
    for (bool profiling : {false, true}) {
        Common::GpuProfile::recording_enabled = profiling;
        for (u32 mode = 0; mode < 4; ++mode) {
            Maxwell3D scalar{false}, batch{true};
            scalar.shadow_state.shadow_ram_control = batch.shadow_state.shadow_ram_control =
                static_cast<Maxwell3D::Regs::ShadowRamControl>(mode);
            for (u32 step = 0; step < 20000; ++step) {
                const u32 first = random() % (MacroRegistersStart - 32);
                const u32 amount = 1 + random() % 32;
                // Exercise shared dirty flags, repeated values and changed-then-restored writes.
                std::array<u32, 32> values{};
                for (u32 i = 0; i < amount; ++i) {
                    const u32 reg = first + i;
                    const u32 value = random() % 4 ? scalar.regs.reg_array[reg] : random();
                    scalar.dirty.tables[1][reg] = batch.dirty.tables[1][reg] =
                        static_cast<unsigned char>(random() % 2 ? reg : reg + 1);
                    scalar.method_sink.emplace_back(reg, value);
                    values[i] = value;
                }
                batch.CallMultiMethod(system, first, values.data(), amount);
                scalar.ConsumeSinkImpl(system);
                batch.ConsumeSinkImpl(system);
                const u32 reg = random() % MacroRegistersStart;
                const u32 value = random() % 2 ? scalar.regs.reg_array[reg] : random();
                // Macro sends use CallMethod: effects must execute even for repeated values.
                scalar.CallMethod(system, reg, value, true);
                batch.CallMethod(system, reg, value, true);
                assert(scalar.regs.reg_array == batch.regs.reg_array);
                assert(scalar.shadow_state.reg_array == batch.shadow_state.reg_array);
                assert(scalar.dirty.flags == batch.dirty.flags);
                assert(scalar.effects == batch.effects);
                if (random() % 8 == 0) {
                    scalar.dirty.flags.reset();
                    batch.dirty.flags.reset();
                }
            }
        }

        if (!profiling) {
            assert(owner.enters == 0 && owner.leaves == 0);
            assert(Common::GpuProfile::TakeMethodWindow()[0].count == 0);
        } else {
            assert(owner.enters > 0 && owner.enters == owner.leaves);
            const auto dirty = Common::GpuProfile::TakeMethodWindow()[0];
            if (Common::GpuProfile::MethodProfilingEnabled())
                assert(dirty.count > 0 && dirty.skipped > 0 && dirty.timed_calls > 0);
            else
                assert(dirty.count == 0);
        }
    }
    Common::GpuProfile::recording_enabled = false;
    Common::GpuProfile::sink = nullptr;
    // Dirty state survives a changed-then-restored batch and can be cleared
    // between batches. Empty batches must work before vector storage exists.
    {
        Maxwell3D batch;
        batch.CallMultiMethod(system, 0, nullptr, 0);
        assert(batch.method_sink.empty());
        u32 method = 0;
        while (batch.execution_mask[method])
            ++method;
        const u32 original = batch.regs.reg_array[method];
        batch.dirty.flags.reset();
        batch.method_sink = {{method, original + 1}, {method, original}};
        batch.ConsumeSinkImpl(system);
        assert(batch.regs.reg_array[method] == original);
        assert(batch.dirty.flags[batch.dirty.tables[0][method]]);
        assert(batch.dirty.flags[batch.dirty.tables[1][method]]);
        batch.dirty.flags.reset();
        batch.method_sink = {{method, original}, {method, original}};
        batch.ConsumeSinkImpl(system);
        assert(batch.dirty.flags.none());
    }
    const u32 values[]{0x1234, 0x5678, 0x9ABC, 0xDEF0};
    // Compare batched writes against the scalar path for every non-executable register,
    // all shadow modes, empty/single/multiple writes, and partial DMA submissions.
    for (u32 mode = 0; mode < 4; ++mode) {
        for (u32 index = 0; index < Maxwell3D::Regs::NUM_REGS; ++index) {
            for (u32 count : {0U, 1U, 4U}) {
                Maxwell3D scalar, batch;
                if (batch.execution_mask[index])
                    continue;
                scalar.shadow_state.shadow_ram_control = batch.shadow_state.shadow_ram_control =
                    static_cast<Maxwell3D::Regs::ShadowRamControl>(mode);
                for (u32 i = 0; i < count; ++i)
                    scalar.CallMethod(system, index, values[i], count - i <= 1);
                batch.CallMultiMethod(system, index, values, count, count + 2);
                assert(scalar.regs.reg_array == batch.regs.reg_array);
                assert(scalar.shadow_state.reg_array == batch.shadow_state.reg_array);
                assert(scalar.dirty.flags == batch.dirty.flags);
                assert(batch.effects == 0);
            }
        }
    }
    // Plain macro register sends retain shadow/dirty state without entering the
    // method-side-effect dispatcher. The escape hatch keeps the legacy path.
    for (bool profiling : {false, true}) {
        Common::GpuProfile::recording_enabled = profiling;
        for (u32 mode = 0; mode < 4; ++mode) {
            Maxwell3D legacy{false}, fast{true};
            legacy.shadow_state.shadow_ram_control = fast.shadow_state.shadow_ram_control =
                static_cast<Maxwell3D::Regs::ShadowRamControl>(mode);
            for (u32 index = 0; index < Maxwell3D::Regs::NUM_REGS; ++index) {
                if (fast.execution_mask[index])
                    continue;
                for (const u32 value : {index, index + 100, index + 200, index + 200}) {
                    legacy.CallMethod(system, index, value, true);
                    fast.CallMethod(system, index, value, true);
                    assert(legacy.regs.reg_array == fast.regs.reg_array);
                    assert(legacy.shadow_state.reg_array == fast.shadow_state.reg_array);
                    assert(legacy.dirty.flags == fast.dirty.flags);
                }
            }
            assert(legacy.dispatch_calls > 0);
            assert(fast.dispatch_calls == 0);
            assert(legacy.effects == 0 && fast.effects == 0);
        }
    }
    Common::GpuProfile::recording_enabled = false;
    Maxwell3D engine;
    const auto draw = MAXWELL3D_REG_INDEX(draw.begin);
    engine.CallMultiMethod(system, draw, values, 4, 4);
    assert(engine.effects == 4);
    engine.CallMultiMethod(system, MAXWELL3D_REG_INDEX(const_buffer.buffer), values, 4, 4);
    assert(engine.memory_manager.writes == 1);
    engine.CallMultiMethod(system, MAXWELL3D_REG_INDEX(inline_data), values, 4, 4);
    assert(engine.upload_state.calls == 1);
    engine.CallMultiMethod(system, MAXWELL3D_REG_INDEX(draw_inline_index), values, 4, 4);
    assert(engine.bulk_calls == 1);
    engine.shadow_state.shadow_ram_control = Maxwell3D::Regs::ShadowRamControl::Replay;
    engine.CallMultiMethod(system, MAXWELL3D_REG_INDEX(draw_inline_index), values, 4, 4);
    assert(engine.bulk_calls == 1 && engine.effects == 8);

    // One bulk boundary, even with executable words and exception unwinding.
    {
        Maxwell3D bulk;
        Common::GpuProfile::sink = &owner;
        Common::GpuProfile::recording_enabled = true;
        auto before = owner.enters;
        bulk.CallMultiMethod(system, draw, values, 4, 4);
        assert(owner.enters == before + (bulk.method_batching ? 1 : 5) &&
               owner.enters == owner.leaves);
        bulk.fail_dispatch = true;
        before = owner.enters;
        try {
            bulk.CallMultiMethod(system, draw, values, 4, 4);
            assert(false);
        } catch (int error) {
            assert(error == 7);
        }
        assert(owner.enters == before + (bulk.method_batching ? 1 : 2) &&
               owner.enters == owner.leaves);
        Common::GpuProfile::recording_enabled = false;
        Common::GpuProfile::sink = nullptr;
    }
    // Non-incrementing ordinary state must preserve intermediate dirty changes,
    // not just the final register value. Compare independent scalar execution
    // in every shadow mode and with both dirty-table entries sharing the same
    // flag.
    for (u32 mode = 0; mode < 4; ++mode) {
        for (bool shared : {false, true}) {
            Maxwell3D scalar, batch;
            u32 reg = 0;
            while (scalar.execution_mask[reg])
                ++reg;
            scalar.shadow_state.shadow_ram_control = batch.shadow_state.shadow_ram_control =
                static_cast<Maxwell3D::Regs::ShadowRamControl>(mode);
            scalar.dirty.tables[1][reg] = batch.dirty.tables[1][reg] =
                shared ? scalar.dirty.tables[0][reg] : 93;
            const u32 original = scalar.regs.reg_array[reg];
            const u32 repeated[]{original, original + 1, original + 2, original};
            for (u32 value : repeated)
                scalar.CallMethod(system, reg, value, true);
            batch.CallMultiMethod(system, reg, repeated, 4, 4);
            assert(scalar.regs.reg_array == batch.regs.reg_array);
            assert(scalar.shadow_state.reg_array == batch.shadow_state.reg_array);
            assert(scalar.dirty.flags == batch.dirty.flags && batch.dirty.flags.any());
        }
    }

    // Repeated state writes skip dirty bits but still execute draw side effects.
    engine.shadow_state.shadow_ram_control = Maxwell3D::Regs::ShadowRamControl::Off;
    engine.CallMethod(system, draw, 55, true);
    engine.dirty.flags.reset();
    const auto effects = engine.effects;
    engine.CallMethod(system, draw, 55, true);
    assert(engine.dirty.flags.none() && engine.effects == effects + 1);
    engine.CallMethod(system, draw, 56, true);
    assert(engine.dirty.flags.any());

    // Incrementing CB packets preserve all register/shadow values while doing one
    // memory-manager write instead of sixteen. Include partial packets and Replay.
    for (u32 mode = 0; mode < 4; ++mode) {
        for (u32 start = 0; start < 16; ++start) {
            Maxwell3D scalar, batch;
            scalar.shadow_state.shadow_ram_control = batch.shadow_state.shadow_ram_control =
                static_cast<Maxwell3D::Regs::ShadowRamControl>(mode);
            std::array<u32, 16> data;
            std::iota(data.begin(), data.end(), 900);
            const u32 first = MAXWELL3D_REG_INDEX(const_buffer.buffer) + start;
            for (u32 i = 0; i < 16 - start; ++i) {
                auto value = scalar.ProcessShadowRam(first + i, data[i]);
                scalar.ProcessDirtyRegisters(first + i, value);
                scalar.ProcessCBMultiData(&value, 1);
            }
            batch.CallCBDataIncrementing(first, data.data(), 16 - start);
            assert(scalar.regs.reg_array == batch.regs.reg_array);
            assert(scalar.shadow_state.reg_array == batch.shadow_state.reg_array);
            assert(scalar.dirty.flags == batch.dirty.flags);
            assert(scalar.memory_manager.words == batch.memory_manager.words);
            assert(scalar.regs.const_buffer.offset == batch.regs.const_buffer.offset);
            assert(batch.memory_manager.writes == 1 && scalar.memory_manager.writes == 16 - start);
        }
    }
    engine.shadow_state.shadow_ram_control = Maxwell3D::Regs::ShadowRamControl::Replay;

    assertion_calls = assertion_failures = 0;
    engine.CallMethod(system, 0, 123, true);
    assert(engine.GetRegisterValue(0) == 100); // Replay still applies.
    assert(assertion_calls == 0);

    // Scalar, bulk, and zero-sized macro segments retain their addresses and dirty state.
    engine.current_dma_segment = 64;
    engine.current_dirty = true;
    engine.CallMethod(system, MacroRegistersStart, values[0], false);
    engine.current_dma_segment = 0;
    engine.CallMultiMethod(system, MacroRegistersStart + 1, values + 1, 2, 3);
    engine.CallMultiMethod(system, MacroRegistersStart + 1, values, 0, 1);
    engine.current_dma_segment = 128;
    engine.CallMethod(system, MacroRegistersStart + 1, values[3], true);
    assert(engine.captured == std::vector<u32>(values, values + 4));
    const std::vector<std::pair<GPUVAddr, size_t>> segments{{64, 1}, {0, 2}, {0, 0}, {128, 1}};
    assert(engine.captured_segments == segments && engine.captured_dirty);
    assert(!engine.current_dirty && !engine.current_macro_dirty && engine.executing_macro == 0);
    assert(engine.macro_params.empty() && engine.macro_segments.empty());
    assert(assertion_calls == 0);

    // High-accuracy refresh reads all nonzero segments into the same parameter storage.
    engine.refresh = true;
    engine.memory_manager.words[16] = 77;
    engine.memory_manager.words[32] = 88;
    engine.current_dma_segment = 64;
    engine.current_dirty = true;
    engine.CallMethod(system, MacroRegistersStart, 1, false);
    engine.current_dma_segment = 0;
    engine.CallMethod(system, MacroRegistersStart + 1, 2, false);
    engine.current_dma_segment = 128;
    engine.CallMethod(system, MacroRegistersStart + 1, 3, true);
    assert((engine.captured == std::vector<u32>{77, 2, 88}));
    assert(engine.memory_manager.reads.size() == 2);
    Settings::high = false;
    engine.current_dirty = true;
    engine.CallMethod(system, MacroRegistersStart, 5, true);
    assert((engine.captured == std::vector<u32>{5}));
    assert(engine.memory_manager.reads.size() == 2);
    Settings::high = true;

    // Exercise vector growth and capacity reuse, including bulk macro parameter append.
    engine.refresh = false;
    std::vector<u32> large(4096);
    std::iota(large.begin(), large.end(), 0);
    engine.CallMultiMethod(system, MacroRegistersStart, large.data(), 4096, 4096);
    assert(engine.captured == large);
    const auto* storage = engine.macro_params.data();
    engine.CallMultiMethod(system, MacroRegistersStart, large.data(), 4096, 4096);
    assert(engine.captured == large && engine.macro_params.data() == storage);

    // Failure diagnostics still run; an unfinished macro must use its argument register.
    engine.CallMethod(system, MacroRegistersStart + 1, 1, true);
    assert(assertion_failures == 1);
    engine.CallMethod(system, MacroRegistersStart, 1, false);
    engine.CallMethod(system, draw, 2, true);
    assert(assertion_failures == 2);
    // Batching must retain that diagnostic rather than silently using the plain-write path.
    engine.CallMultiMethod(system, 0, values, 4, 4);
    assert(assertion_failures == 6);
}
