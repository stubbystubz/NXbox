// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
#include "video_core/renderer_d3d12/d3d12_recorded_state.h"

#include <cassert>
#include <iostream>
#include <random>

using namespace D3D12;

// A tiny cache forces collisions/eviction. GPU tables snapshot descriptor contents;
// CPU handles are recycled with different contents while old GPU tables stay live.
int main() {
    std::mt19937 random{0xCB012};
    BoundHandleTableCache<std::array<unsigned, 16>, 2, 8> tables;
    GraphicsRootCbvCache roots;
    std::array<unsigned, 16> cpu{};
    std::array<std::uint64_t, 64> reference_cbvs{}, fast_cbvs{};
    std::array<bool, 64> reference_valid{}, fast_valid{};
    std::array<std::uintptr_t, 16> handles{};
    std::array<std::uint32_t, 3> sizes{4, 0, 0};
    std::uint64_t tick = 1, generation = 1;
    std::uintptr_t signature = 1;
    std::size_t hits = 0, misses = 0, skipped = 0, binds = 0;
    for (unsigned draw = 0; draw < 100000; ++draw) {
        if (random() % 19 == 0) {
            ++tick;
            roots.Reset(); // Same native command-list pointer, new recording.
            reference_valid.fill(false);
            fast_valid.fill(false);
        }
        if (random() % 31 == 0) {
            ++generation;
            for (auto& content : cpu)
                content = random();
        }
        if (random() % 37 == 0)
            tables.Clear(); // TickFrame may clear in the middle of a recording.
        if (random() % 7 == 0)
            sizes = {random() % 6, random() % 6, random() % 5};
        const auto count = sizes[0] + sizes[1] + sizes[2];
        if (random() % 3 == 0) {
            for (auto& handle : handles)
                handle = random() % cpu.size();
        }
        std::array<unsigned, 16> reference_table{};
        for (unsigned slot = 0; slot < count; ++slot)
            reference_table[slot] = cpu[handles[slot]];
        const auto sources = std::span{handles.data(), count};
        if (const auto* cached = tables.Find(tick, generation, sizes, sources)) {
            ++hits;
            assert(*cached == reference_table);
        } else {
            ++misses;
            tables.Remember(tick, generation, sizes, sources, reference_table);
        }
        // Both layouts and high address bits participate in equality.
        if (count <= 8) {
            assert(tables.Find(tick, generation, sizes, sources));
            assert(!tables.Find(tick + 1, generation, sizes, sources));
            assert(!tables.Find(tick, generation + 1, sizes, sources));
            auto other_layout = sizes;
            ++other_layout[0];
            assert(!tables.Find(tick, generation, other_layout, sources));
            if (count) {
                handles[0] ^= std::uintptr_t{1} << 48;
                assert(!tables.Find(tick, generation, sizes, sources));
                handles[0] ^= std::uintptr_t{1} << 48;
            }
        } else {
            assert(!tables.Find(tick, generation, sizes, sources));
        }
        if (random() % 43 == 0) {
          // An indirect signature may overwrite root arguments and resets them
          // afterwards. General draw-state invalidation alone does not cover
          // it.
          roots.Reset();
          reference_valid.fill(false);
          fast_valid.fill(false);
        }
        const auto next_signature = random() % 9 == 0 ? random() % 4 : signature;
        if (next_signature != signature) {
            reference_valid.fill(false);
            fast_valid.fill(false);
        }
        signature = next_signature;
        // Reissuing an identical signature must preserve the CBVs.
        roots.SetSignature(signature);
        roots.SetSignature(signature);
        if (random() % 5 == 0) {
            const auto slot = random() % 64;
            // Table/constants/SRV/UAV overwrite a root parameter of another type.
            roots.Invalidate(slot);
            reference_valid[slot] = fast_valid[slot] = false;
        }
        for (unsigned slot = 4; slot < 10; ++slot) {
            const auto address =
                random() % 5 == 0 ? std::uint64_t{random()} << 24 : reference_cbvs[slot];
            // Old path binds unconditionally, including valid address zero.
            reference_cbvs[slot] = address;
            reference_valid[slot] = true;
            ++binds;
            if (roots.cbvs.Update(slot, address)) {
                fast_cbvs[slot] = address;
                fast_valid[slot] = true;
            } else {
                ++skipped;
            }
            // Compare effective GPU state at each draw, not command counts.
            assert(fast_valid[slot] && fast_cbvs[slot] == reference_cbvs[slot]);
        }
        assert(fast_valid == reference_valid);
    }
    // Replacing a collision cannot change an already recorded table snapshot.
    BoundHandleTableCache<unsigned, 1, 1> collision;
    std::array<std::uintptr_t, 1> a{1}, b{2};
    collision.Remember(1, 1, {1, 0, 0}, a, 111);
    const auto snapshot = *collision.Find(1, 1, {1, 0, 0}, a);
    collision.Remember(1, 1, {1, 0, 0}, b, 222);
    assert(!collision.Find(1, 1, {1, 0, 0}, a));
    assert(snapshot == 111);
    assert(*collision.Find(1, 1, {1, 0, 0}, b) == 222);
    assert(hits && misses && skipped);
    std::cout << "100000 random draws: table hits=" << hits << " misses=" << misses
              << "; CBVs skipped=" << skipped << '/' << binds
              << "; effective state matches unconditional binding\n";
}
