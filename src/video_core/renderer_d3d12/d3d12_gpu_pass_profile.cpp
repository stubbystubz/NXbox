// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_gpu_pass_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <utility>
#include <vector>
#include <fmt/format.h>
#include "video_core/renderer_d3d12/d3d12_context.h"

namespace D3D12 {
namespace {
constexpr u32 SlotCount = 32;
constexpr u32 MaxPasses = 4096;
constexpr u32 QueriesPerSlot = 2 + MaxPasses * 2;
constexpr std::array Names{"draw", "compute", "copy", "clear", "blit"};
struct Pass {
    GpuPassKind kind;
    GpuPassTarget target;
    u64 draws{}, calls{1};
};
} // namespace
struct GpuPassProfile::Impl {
    explicit Impl(Context& owner) : context{owner} {
        D3D12_QUERY_HEAP_DESC desc{};
        desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        desc.Count = SlotCount * QueriesPerSlot;
        Check(context.Device()->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)),
              "GPU pass timestamp heap", context.Device());
        readback = context.CreateBuffer(u64{desc.Count} * sizeof(u64), D3D12_HEAP_TYPE_READBACK);
        u64 frequency{};
        Check(context.Queue()->GetTimestampFrequency(&frequency), "GPU pass frequency",
              context.Device());
        if (!frequency)
            throw std::runtime_error("GPU pass timestamp frequency is zero");
        ticks_to_ms = 1000.0 / static_cast<double>(frequency);
    }
    struct Slot {
        std::atomic<bool> busy{};
    };
    struct Sum {
        double ms{};
        u64 draws{}, calls{};
    };
    using Key = std::pair<GpuPassKind, GpuPassTarget>;
    Context& context;
    ComPtr<ID3D12QueryHeap> heap;
    ResourcePtr readback;
    std::array<Slot, SlotCount> slots;
    CommandFacade* commands{};
    u32 slot_index{}, next_slot{};
    std::vector<Pass> passes;
    GpuPassTarget target;
    GpuPassKind draw_kind{GpuPassKind::Draw};
    bool open{}, frame_end_pending{};
    u64 overflow{};
    // Only completion callbacks mutate these fields (serialized by Context).
    std::map<Key, Sum> sums;
    double ticks_to_ms{}, frame_gpu_ms{}, idle_ms{};
    u64 last_end{}, frames{}, dropped{}, truncated{};
    std::chrono::steady_clock::time_point last_log{std::chrono::steady_clock::now()};

    u32 Base() const {
        return slot_index * QueriesPerSlot;
    }
    void Stamp(u32 index) {
        commands->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Base() + index);
    }
    void Close() {
        if (!open)
            return;
        Stamp(3 + static_cast<u32>(passes.size() - 1) * 2);
        open = false;
    }
    void Read(u32 slot, const std::vector<Pass>& samples, u64 overflow_count) {
        const auto* bytes = readback->mapped.data() + u64{slot} * QueriesPerSlot * sizeof(u64);
        auto stamp = [bytes](u32 index) {
            u64 value{};
            std::memcpy(&value, bytes + u64{index} * sizeof(u64), sizeof(value));
            return value;
        };
        const auto begin = stamp(0), end = stamp(1);
        if (end >= begin) {
            frame_gpu_ms += static_cast<double>(end - begin) * ticks_to_ms;
            if (last_end && begin > last_end)
                idle_ms += static_cast<double>(begin - last_end) * ticks_to_ms;
            last_end = end;
            for (u32 i = 0; i < samples.size(); ++i) {
                const auto first = stamp(2 + i * 2), last = stamp(3 + i * 2);
                if (last < first)
                    continue;
                const auto& pass = samples[i];
                auto& sum = sums[{pass.kind, pass.target}];
                sum.ms += static_cast<double>(last - first) * ticks_to_ms;
                sum.draws += pass.draws;
                sum.calls += pass.calls;
            }
        } else {
            last_end = 0;
            ++dropped;
        }
        truncated += overflow_count;
        // Release only after copying every mapped byte, never just on fence completion.
        slots[slot].busy.store(false, std::memory_order_release);
    }
    void Frame() {
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log < std::chrono::seconds{5})
            return;
        std::vector<std::pair<Key, Sum>> ranked(sums.begin(), sums.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
        const double count = static_cast<double>(frames);
        double total{};
        for (const auto& [key, sum] : ranked)
            total += sum.ms;
        for (size_t i = 0; i < std::min<size_t>(15, ranked.size()); ++i) {
            const auto& [key, sum] = ranked[i];
            const auto& [kind, rt] = key;
            Diagnostic(fmt::format(
                "NXBOX GPU_PASS rank={} ms={:.4f} kind={} draws={:.3f} rt={}x{} fmt={} "
                "depth={} shader_hash={:016x} calls/frame={:.3f}",
                i + 1, sum.ms / count, Names[static_cast<size_t>(kind)], sum.draws / count,
                rt.width, rt.height, rt.format, rt.depth, rt.shader_hash, sum.calls / count));
        }
        Diagnostic(fmt::format("NXBOX GPU_PASS total_ms={:.4f} frame_gpu_ms={:.4f} idle_ms={:.4f} "
                               "frames={} dropped_submissions={} truncated_passes={}",
                               total / count, frame_gpu_ms / count, idle_ms / count, frames,
                               dropped, truncated));
        sums.clear();
        frames = dropped = truncated = 0;
        frame_gpu_ms = idle_ms = 0;
        last_log = now;
    }
};
bool GpuPassProfile::Enabled() {
    const char* value = std::getenv("NXBOX_D3D12_GPU_PASS_PROFILE");
    return value && value[0] == '1' && value[1] == '\0';
}
GpuPassProfile::GpuPassProfile(Context& context) : impl{std::make_unique<Impl>(context)} {}
GpuPassProfile::~GpuPassProfile() = default;
void GpuPassProfile::BeginSubmission(CommandFacade& commands) {
    auto& p = *impl;
    p.draw_kind = GpuPassKind::Draw;
    p.target = {};
    for (u32 i = 0; i < SlotCount; ++i) {
        const u32 slot = (p.next_slot + i) % SlotCount;
        if (p.slots[slot].busy.exchange(true, std::memory_order_acq_rel))
            continue;
        p.slot_index = slot;
        p.next_slot = (slot + 1) % SlotCount;
        p.commands = &commands;
        p.passes.clear();
        p.open = false;
        p.overflow = 0;
        p.Stamp(0);
        commands.pass_profile = this;
        return;
    }
    // Bounded memory and no extra fence wait when the completion thread falls behind.
    p.context.AfterCompletion([&p] {
        ++p.dropped;
        p.last_end = 0;
    });
}
void GpuPassProfile::EndSubmission() {
    auto& p = *impl;
    if (!p.commands) {
        if (std::exchange(p.frame_end_pending, false))
            p.context.AfterCompletion([&p] { p.Frame(); });
        return;
    }
    p.Close();
    p.Stamp(1);
    p.commands->pass_profile = nullptr; // Do not profile our own query resolve.
    const u32 count = 2 + static_cast<u32>(p.passes.size()) * 2;
    p.commands->ResolveQueryData(p.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, p.Base(), count,
                                 p.readback->handle.Get(), u64{p.Base()} * sizeof(u64));
    p.context.AfterCompletion([&p, slot = p.slot_index, samples = std::move(p.passes),
                               overflow = p.overflow] { p.Read(slot, samples, overflow); });
    p.commands = nullptr;
    if (std::exchange(p.frame_end_pending, false))
        p.context.AfterCompletion([&p] { p.Frame(); });
}
void GpuPassProfile::EndFrame() {
    impl->frame_end_pending = true;
}
void GpuPassProfile::Work(GpuPassKind kind) {
    auto& p = *impl;
    if (!p.commands)
        return;
    const bool recorded_draw = kind == GpuPassKind::Draw;
    if (recorded_draw)
        kind = p.draw_kind;
    if (p.open && p.passes.back().kind == kind &&
        (kind == GpuPassKind::Draw || kind == GpuPassKind::Compute)) {
        if (recorded_draw)
            ++p.passes.back().draws;
        return;
    }
    p.Close();
    if (p.passes.size() == MaxPasses) {
        ++p.overflow;
        return;
    }
    p.passes.push_back({kind, kind == GpuPassKind::Compute ? GpuPassTarget{} : p.target,
                        recorded_draw ? 1ULL : 0ULL, 1});
    p.Stamp(2 + static_cast<u32>(p.passes.size() - 1) * 2);
    p.open = true;
}
void GpuPassProfile::Boundary() {
    impl->Close();
}
void GpuPassProfile::Target(GpuPassTarget target) {
    const auto& old = impl->target;
    if (old.width != target.width || old.height != target.height || old.format != target.format ||
        old.depth != target.depth)
        Boundary();
    impl->target = target;
}
void GpuPassProfile::Label(std::string_view label) {
    const auto kind = label.find("clear") != std::string_view::npos &&
                              label.find("present") == std::string_view::npos
                          ? GpuPassKind::Clear
                      : label.find("blit") != std::string_view::npos ||
                              label.find("present") != std::string_view::npos
                          ? GpuPassKind::Blit
                          : GpuPassKind::Draw;
    if (impl->draw_kind != kind) {
        Boundary();
        impl->target = {};
        impl->draw_kind = kind;
    }
}
} // namespace D3D12
