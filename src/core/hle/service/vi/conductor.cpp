// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/adpf.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/core_timing_trace.h"
#include "core/hle/service/vi/conductor.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/display_list.h"
#include "core/hle/service/vi/vblank_timing.h"
#include "core/hle/service/vi/vsync_manager.h"

constexpr auto FrameNs = std::chrono::nanoseconds{1000000000 / 60};

constexpr s64 UNLOCKED_TARGET_DIVISOR = 4;

namespace Service::VI {

Conductor::Conductor(Core::System& system, Container& container, DisplayList& displays)
    : m_system(system), m_container(container),
      m_independent_vsync(system.IsMulticore() && IndependentVsyncEnabled()) {
    if (VblankProfileEnabled()) {
        vblank_timing.Reset();
    }
    displays.ForEachDisplay([&](Display& display) {
        m_vsync_managers.insert({display.GetId(), VsyncManager{}});
    });

    if (system.IsMulticore()) {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                if (VblankProfileEnabled()) {
                    vblank_timing.RecordTick(ns_late);
                }
                // A busy compositor must not defer the guest's vblank notification. Keep
                // composition on its worker so WaitForComposite never blocks CoreTiming.
                if (m_independent_vsync) {
                    this->SignalVsync();
                }
                m_signal.Set();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
        m_thread = std::jthread([this](std::stop_token token) { this->VsyncThread(token); });
    } else {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                if (VblankProfileEnabled()) {
                    vblank_timing.RecordTick(ns_late);
                }
                this->ProcessVsync();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
    }
}

Conductor::~Conductor() {
    m_system.CoreTiming().UnscheduleEvent(m_event);

    if (m_system.IsMulticore()) {
        m_thread.request_stop();
        m_signal.Set();
        // Join before the synchronization and pacing members are destroyed.
        m_thread.join();
    }
}

void Conductor::LinkVsyncEvent(u64 display_id, Event* event) {
    std::scoped_lock lock{m_vsync_lock};
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end()) {
        it->second.LinkVsyncEvent(event);
    }
}

void Conductor::UnlinkVsyncEvent(u64 display_id, Event* event) {
    std::scoped_lock lock{m_vsync_lock};
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end()) {
        it->second.UnlinkVsyncEvent(event);
    }
}

void Conductor::SignalVsync() {
    std::scoped_lock lock{m_vsync_lock};
    for (auto& entry : m_vsync_managers) {
        Core::Timing::Trace::Record(Core::Timing::Trace::Source::VsyncSignal, 0, 0, entry.first);
        entry.second.SignalVsync(m_system.Kernel());
    }
    if (VblankProfileEnabled()) {
        vblank_timing.RecordSignal(VblankTiming::Clock::now());
    }
}

void Conductor::ProcessVsync() {
    Common::PollThreadPolicies();
    Common::ADPF::SetTargetWorkDuration(std::chrono::nanoseconds{this->GetFramePeriodNs()});

    const bool profile = VblankProfileEnabled();
    const auto compose_start =
        profile ? VblankTiming::Clock::now() : VblankTiming::Clock::time_point{};
    for (auto& [display_id, manager] : m_vsync_managers) {
        auto swap_interval = m_swap_interval.load(std::memory_order_relaxed);
        auto speed_scale = m_compose_speed_scale.load(std::memory_order_relaxed);
        m_container.ComposeOnDisplay(&swap_interval, &speed_scale, display_id);
        m_swap_interval.store(swap_interval, std::memory_order_relaxed);
        m_compose_speed_scale.store(speed_scale, std::memory_order_relaxed);
        if (!m_independent_vsync) {
            std::scoped_lock lock{m_vsync_lock};
            Core::Timing::Trace::Record(Core::Timing::Trace::Source::VsyncSignal, 0, 0, display_id);
            manager.SignalVsync(m_system.Kernel());
        }
    }
    if (profile) {
        const auto compose_end = VblankTiming::Clock::now();
        vblank_timing.RecordComposition(
            std::chrono::duration_cast<std::chrono::nanoseconds>(compose_end - compose_start));
        if (!m_independent_vsync) {
            vblank_timing.RecordSignal(compose_end);
        }
    }
}

void Conductor::VsyncThread(std::stop_token token) {
    Common::SetCurrentThreadName("VSyncThread");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
    Common::SetCurrentThreadToPerformanceCores();

    while (!token.stop_requested()) {
        m_signal.Wait();

        if (token.stop_requested() || m_system.IsShuttingDown()) {
            return;
        }

        this->ProcessVsync();
    }
}

s64 Conductor::GetNextTicks() const {
    const auto& settings = Settings::values;
    auto speed_scale = 1.f;
    if (settings.use_multi_core.GetValue()) {
        if (settings.use_speed_limit.GetValue()) {
            // Scales the speed based on speed_limit setting on MC. SC is handled by
            // SpeedLimiter::DoSpeedLimiting.
            speed_scale = 100.f / Settings::SpeedLimit();
        } else {
            // Run at unlocked framerate.
            speed_scale = 0.01f;
        }
    }

    // Adjust by speed limit determined during composition.
    speed_scale /= m_compose_speed_scale.load(std::memory_order_relaxed);

    if (m_system.GetNVDECActive() && settings.use_video_framerate.GetValue()) {
        // Run at intended presentation rate during video playback.
        speed_scale = 1.f;
    }

    const auto interval = m_independent_vsync ? 1 : m_swap_interval.load(std::memory_order_relaxed);
    const f32 effective_fps = 60.f / static_cast<f32>(interval);
    return static_cast<s64>(speed_scale * (1000000000.f / effective_fps));
}

s64 Conductor::GetFramePeriodNs() const {
    const auto& settings = Settings::values;
    f32 speed_scale = 1.f;
    bool unlocked = false;
    if (settings.use_multi_core.GetValue()) {
        if (settings.use_speed_limit.GetValue()) {
            speed_scale = 100.f / Settings::SpeedLimit();
        } else {
            unlocked = true;
        }
    }
    speed_scale /= m_compose_speed_scale.load(std::memory_order_relaxed);

    const f32 effective_fps =
        60.f / static_cast<f32>(m_swap_interval.load(std::memory_order_relaxed));
    s64 period = static_cast<s64>(speed_scale * (1000000000.f / effective_fps));
    if (unlocked) {
        period /= UNLOCKED_TARGET_DIVISOR;
    }
    return std::clamp<s64>(period, 1'000'000, 100'000'000);
}

} // namespace Service::VI
