// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <utility>
#include <vector>
#include "common/gpu_thread_profile.h"
#include "common/maxwell_method_profile.h"
#include "video_core/gpu_frontend.h"
using u32 = std::uint32_t;
using u8 = std::uint8_t;
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
// Out of range, so a method never matches a register this fixture does not model.
#define MAXWELL3D_REG_INDEX(field) static_cast<u32>(sizeof(#field) + 0x10000)
template <class Self>
struct Common3D {
    struct Regs {
        static constexpr u32 NUM_REGS = 0xE00;
        enum class ShadowRamControl { Off, Track, TrackWithFilter, Replay };
        ShadowRamControl shadow_ram_control{};
        std::array<u32, NUM_REGS> reg_array{};
        struct {
            std::uint64_t Address() const {
                return 0;
            }
        } program_region;
    } regs, shadow_state;
    struct {
        unsigned GetID() const {
            return 0;
        }
    } memory_manager;
    struct {
        std::bitset<255> flags;
        std::array<std::array<u8, Regs::NUM_REGS>, 2> tables{};
    } dirty;
    std::vector<std::pair<u32, u32>> method_sink;
};
struct BaseM3D : Common3D<BaseM3D> {
    u32 ProcessShadowRam(u32, u32);
    void ConsumeSinkImpl(Core::System&);
    void ProcessDirtyRegisters(u32, u32);
};
struct NewM3D : Common3D<NewM3D> {
    const bool frontend_fastpath;
    explicit NewM3D(bool f) : frontend_fastpath(f) {}
    u32 ProcessShadowRam(u32, u32);
    void ConsumeSinkImpl(Core::System&);
    void ProcessDirtyRegisters(u32, u32);
    void ProcessDirtyRegistersImpl(u32, u32);
};
u32 BaseM3D::ProcessShadowRam(u32 method, u32 argument) {
    // Keep track of the register value in shadow_state when requested.
    auto const c = shadow_state.shadow_ram_control;
    if (c == Regs::ShadowRamControl::Track || c == Regs::ShadowRamControl::TrackWithFilter)
        return shadow_state.reg_array[method] = argument;
    else if (c == Regs::ShadowRamControl::Replay)
        return shadow_state.reg_array[method];
    return argument;
}
void BaseM3D::ProcessDirtyRegisters(u32 method, u32 argument) {
    Common::GpuProfile::MethodScope method_profile{Common::GpuProfile::MethodClass::DirtyFlags};
    // Method side effects still run in ProcessMethodCall. Unchanged register
    // state needs no table lookups or dirty-bit writes.
    if (regs.reg_array[method] == argument) {
        method_profile.Skip();
        return;
    }
    regs.reg_array[method] = argument;
    for (auto const& table : dirty.tables)
        dirty.flags[table[method]] = true;
}
// METHODS
template <class A, class B>
bool same(const A& a, const B& b) {
    return a.regs.reg_array == b.regs.reg_array &&
           a.shadow_state.reg_array == b.shadow_state.reg_array && a.dirty.flags == b.dirty.flags;
}
int main() {
    unsigned checks = 0;
    for (unsigned seed = 1; seed <= 300; ++seed) {
        std::mt19937 rng{seed * 2246822519u};
        for (bool recording : {false, true})
            for (bool fast : {false, true}) {
                Common::GpuProfile::recording_enabled =
                    false; // base predates the gate; compare architectural state only
                struct TestSink : Common::GpuProfile::Sink {
                    std::size_t Enter(Common::GpuProfile::Phase) override {
                        return 0;
                    }
                    void Leave(std::size_t) override {}
                } sink;
                Common::GpuProfile::sink = recording ? &sink : nullptr;
                BaseM3D base;
                NewM3D cur{fast};
                for (u32 i = 0; i < 0xE00; ++i) {
                    const u32 v = rng();
                    base.regs.reg_array[i] = cur.regs.reg_array[i] = v % 5;
                    base.shadow_state.reg_array[i] = cur.shadow_state.reg_array[i] = (v >> 8) % 5;
                    const u8 t0 = rng() % 255,
                             t1 = (rng() % 3) ? t0 : rng() % 255; // many shared flags
                    base.dirty.tables[0][i] = cur.dirty.tables[0][i] = t0;
                    base.dirty.tables[1][i] = cur.dirty.tables[1][i] = t1;
                }
                for (unsigned step = 0; step < 400; ++step) {
                    const auto mode = static_cast<BaseM3D::Regs::ShadowRamControl>(rng() % 4);
                    base.shadow_state.shadow_ram_control =
                        static_cast<BaseM3D::Regs::ShadowRamControl>(mode);
                    cur.shadow_state.shadow_ram_control =
                        static_cast<NewM3D::Regs::ShadowRamControl>(mode);
                    const unsigned n = rng() % 40;
                    for (unsigned i = 0; i < n; ++i) {
                        const u32 method = rng() % 0xE00, value = rng() % 5;
                        base.method_sink.emplace_back(method, value);
                        cur.method_sink.emplace_back(method, value);
                    }
                    Core::System system;
                    for (auto [method, value] : base.method_sink)
                        base.ProcessDirtyRegisters(method, base.ProcessShadowRam(method, value));
                    base.method_sink.clear();
                    Common::GpuProfile::TakeMethodWindow();
                    Common::GpuProfile::recording_enabled = recording;
                    cur.ConsumeSinkImpl(system);
                    const auto window = Common::GpuProfile::TakeMethodWindow();
                    assert(window[static_cast<std::size_t>(
                                      Common::GpuProfile::MethodClass::DirtyFlags)]
                               .count ==
                           (recording && Common::GpuProfile::MethodProfilingEnabled() ? n : 0));
                    Common::GpuProfile::recording_enabled = false;
                    assert(base.method_sink.empty() && cur.method_sink.empty());
                    assert(same(base, cur));
                    // CallMethod's register prefix: shadow RAM then dirty tracking.
                    const u32 method = rng() % 0xE00, value = rng() % 5;
                    const u32 a = base.ProcessShadowRam(method, value);
                    base.ProcessDirtyRegisters(method, a);
                    const u32 b = cur.ProcessShadowRam(method, value);
                    cur.ProcessDirtyRegisters(method, b);
                    assert(a == b && same(base, cur));
                    if (rng() % 6 == 0) {
                        base.dirty.flags.reset();
                        cur.dirty.flags.reset();
                    }
                    ++checks;
                }
            }
    }
    Common::GpuProfile::sink = nullptr;
    Common::GpuProfile::recording_enabled = true;
    {
        Common::GpuProfile::RecordingScope scope{Common::GpuProfile::Phase::Maxwell3DMethods};
    }
    Common::GpuProfile::recording_enabled = false;
    std::printf("sink-oracle checks=%u OK\n", checks);
}
