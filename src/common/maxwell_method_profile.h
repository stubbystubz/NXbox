// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <utility>
#include "common/gpu_thread_profile.h"

namespace Common::MaxwellMethodProfile {
enum Class : unsigned {
    ConstantBuffer,
    InlineUpload,
    DrawBegin,
    DrawEnd,
    DrawInline,
    DrawIndexed,
    Draw,
    Clear,
    QuerySync,
    Macro,
    PlainRegisters,
    ShadowDirty,
    Other,
    CommitAsyncFlushes,
    CreateFence,
    QueueFence,
    FlushCommands,
    InvalidateGpuCache,
    FenceLock,
    AccelerateConditional,
    FallbackCondition,
    GuestSyncpointAction,
    CompositeRest,
    PresentLookup,
    PresentPrepare,
    PresentDownload,
    PresentUpload,
    PresentDraw,
    PresentEasu,
    PresentRcas,
    PresentCapture,
    PresentCopy,
    PresentSwapchain,
    PresentSubmit,
    PresentWait,
    PresentFinish,
    PresentFrameEnd,
    PresentMap,
    ClassCount
};
inline constexpr std::array Names{"cb_data",
                                  "inline_upload",
                                  "draw_begin",
                                  "draw_end",
                                  "draw_inline",
                                  "draw_indexed",
                                  "draw",
                                  "clear",
                                  "query_sync",
                                  "macro",
                                  "plain_registers",
                                  "shadow_dirty",
                                  "other",
                                  "commit_async_flushes",
                                  "create_fence",
                                  "queue_fence",
                                  "flush_commands",
                                  "invalidate_gpu_cache",
                                  "fence_lock",
                                  "accelerate_conditional",
                                  "fallback_condition",
                                  "guest_syncpoint_action",
                                  "composite_rest",
                                  "present_lookup",
                                  "present_prepare",
                                  "present_download",
                                  "present_upload",
                                  "present_draw",
                                  "present_easu",
                                  "present_rcas",
                                  "present_capture",
                                  "present_copy",
                                  "present_swapchain",
                                  "present_submit",
                                  "present_wait",
                                  "present_finish",
                                  "present_frame_end",
                                  "present_map"};
static_assert(Names.size() == ClassCount);
inline constexpr unsigned RegisterCount = 0xE00;
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_D3D12_METHOD_PROFILE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
struct Sample {
    GpuProfile::Tick ticks{}, count{};
};
// One collector per recording thread. ReadTicks returns a monotonic *exclusive*
// Maxwell phase clock, in FrameProfile's 100 ns units, including across windows.
struct Collector {
    virtual ~Collector() = default;
    virtual GpuProfile::Tick ReadTicks() = 0;
    std::array<Sample, ClassCount + RegisterCount> samples{};
    struct Scope;
    Scope* active{};
};
inline thread_local Collector* collector{};
struct Collector::Scope {
    Collector& owner;
    Scope* parent;
    unsigned key;
    GpuProfile::Tick count, start, children{};
    Scope(Collector& owner_, unsigned key_, GpuProfile::Tick count_)
        : owner{owner_}, parent{owner.active}, key{key_}, count{count_}, start{owner.ReadTicks()} {
        owner.active = this;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        const auto elapsed = owner.ReadTicks() - start;
        auto& sample = owner.samples[key];
        sample.ticks += elapsed - children;
        sample.count += count;
        if (parent)
            parent->children += elapsed;
        owner.active = parent;
    }
};
// Optional named scope: the disabled path reads only the recording-thread TLS pointer.
struct NamedScope {
    std::optional<Collector::Scope> scope;
    explicit NamedScope(Class key, bool enabled = true) {
        if (auto* owner = collector; owner && enabled) [[unlikely]]
            scope.emplace(*owner, key, 1);
    }
};
inline bool InPresentation() {
    if (auto* owner = collector) [[unlikely]] {
        for (auto* scope = owner->active; scope; scope = scope->parent) {
            if (scope->key == CompositeRest || scope->key == PresentLookup ||
                scope->key == PresentDraw || scope->key == PresentCapture)
                return true;
        }
    }
    return false;
}
// Classify and construct the scope only in the enabled instantiation. The normal
// instantiation has no scope cleanup, clock, classification, or per-word gate.
template <typename Key, typename Function>
#ifdef _MSC_VER
__forceinline
#else
__attribute__((always_inline)) inline
#endif
    void Dispatch(Key&& key, GpuProfile::Tick count, Function&& function) {
    if (auto* owner = collector) [[unlikely]] {
        Collector::Scope scope{*owner, key(), count};
        function(std::true_type{});
    } else {
        function(std::false_type{});
    }
}
// Backend subdivisions share the exclusive phase clock and subtract from the
// enclosing method. The TLS pointer is installed only by the method-profile gate.
template <typename Function>
decltype(auto) Measure(Class key, Function&& function) {
    if (auto* owner = collector) [[unlikely]] {
        Collector::Scope scope{*owner, key, 1};
        return function();
    }
    return function();
}
template <bool Profile, typename Function>
decltype(auto) Shadow(GpuProfile::Tick count, Function&& function) {
    if constexpr (Profile) {
        Collector::Scope scope{*collector, ShadowDirty, count};
        return function();
    } else {
        return function();
    }
}
} // namespace Common::MaxwellMethodProfile
