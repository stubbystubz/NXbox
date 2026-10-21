// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
// Appended to the production structs/methods with a counting D3D12 device fixture.
#include <chrono>
#include <iostream>

int main(int argc, char** argv) {
    const bool benchmark = argc > 1 && std::strcmp(argv[1], "check") != 0;
    const bool profile = argc > 1 && std::strcmp(argv[1], "profile") == 0;
    double plan_ns = 0, table_ns = 0;
    trace_api = !benchmark;
    D3D12DescriptorAllocator allocator;
    allocator.descriptor_fastpath = true;
    allocator.handle_table_reuse = true;
    DescriptorPlan plan{{0, 0, 0}};
    const char* setting = std::getenv("NXBOX_D3D12_DESCRIPTOR_KEY_PATCH");
    assert(plan.patch_keys == (!setting || std::strcmp(setting, "0") != 0));
    std::array<ResourcePtr, 32> resources;
    for (auto& resource : resources)
        resource = std::make_shared<Resource>();
    std::uint64_t digest = 0;
    const unsigned frames = benchmark ? 600 : 8;
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned frame = 0; frame < frames; ++frame) {
        for (unsigned draw = 0; draw < 2800; ++draw) {
            // 4-16 SRVs, 1-4 UAVs, root CBVs (zero CBV table slots),
            // material runs of eight draws and recurring materials. Synthetic,
            // not a captured game trace. Each run changes one material binding.
            const unsigned material = (draw / 8) % 32;
            const u32 srv = 4 + ((draw / 256) % 4) * 4;
            const u32 uav = 1 + (draw / 128) % 4;
            const auto plan_begin = profile ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
            plan.Reset({0, srv, uav});
            DescriptorTable table;
            table.plan = &plan;
            table.count = srv + uav;
            for (u32 slot = 0; slot < table.count; ++slot) {
                std::array<u8, 64> desc{};
                const u32 kind = slot < srv ? 1 : 2;
                const u32 size = kind == 1 ? 40 : 32;
                desc[0] = static_cast<u8>(slot);
                desc[4] = static_cast<u8>(slot == 0 ? material : slot % 3);
                auto resource = resources[slot == 0 ? material : slot % 3];
                allocator.WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, resource, kind,
                                      desc.data(), size, table, slot);
                if (!benchmark) {
                    const auto& key = plan.keys[slot];
                    assert(key.kind == kind && key.size == size);
                    assert(key.resource == reinterpret_cast<std::uintptr_t>(resource.get()));
                    assert(std::memcmp(key.description.data(), desc.data(), 64) == 0);
                    assert(plan.resources[slot] == resource);
                }
            }
            // Same production reservation lookup then finisher lookup as graphics.
            const auto table_begin = profile ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
            const auto expected = benchmark ? std::vector<DescriptorKey>{} : plan.keys;
            const bool reuse = allocator.CanReuseDrawTable(plan);
            const auto gpu = allocator.FinishDrawTable(plan);
            if (profile) {
                const auto end = std::chrono::steady_clock::now();
                plan_ns +=
                    std::chrono::duration<double, std::nano>(table_begin - plan_begin).count();
                table_ns += std::chrono::duration<double, std::nano>(end - table_begin).count();
            }
            if (!benchmark) {
                for (u32 slot = 0; slot < expected.size(); ++slot)
                    assert(gpu_keys.at(gpu.cpu.ptr + slot) == expected[slot]);
                digest = digest * 131 + gpu.first + reuse;
                for (const auto& key : allocator.previous_draw_tables[0].plan.keys)
                    digest = digest * 131 + key.kind + key.description[4];
            }
            plan.resources.clear(); // Actual Draw cleanup, including scratch after swaps.
            // Bound fixture memory just as retired GPU tables are bounded by rings.
            if (draw % 177 == 0) {
                gpu_keys.clear();
                ++allocator.context.tick;          // Reserve/feedback submission.
                ++allocator.view_cache.generation; // Independently invalid CPU sources.
            }
        }
    }
    if (!benchmark) {
        // Same slot: resource-only changes, null bindings, kind changes, shorter
        // descriptions (the old tail must become zero), and the last valid byte.
        for (unsigned change = 0; change < 30; ++change) {
            plan.Reset({1, 0, 0});
            DescriptorTable table;
            table.plan = &plan;
            table.count = 1;
            std::array<u8, 64> desc{};
            desc[0] = 7;
            const u32 size = change % 3 == 0 ? 64 : 16;
            if (size == 64)
                desc[63] = static_cast<u8>(change);
            const auto resource = change % 5 ? resources[change % 32] : ResourcePtr{};
            allocator.WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, resource, change % 4,
                                  desc.data(), size, table, 0);
            const auto expected = plan.keys;
            const auto gpu = allocator.FinishDrawTable(plan);
            assert(gpu_keys.at(gpu.cpu.ptr) == expected[0]);
            assert(expected[0].description == desc);
            plan.resources.clear();
        }
        bool rejected = false;
        try {
            DescriptorTable table;
            table.plan = &plan;
            std::array<u8, 65> oversized{};
            allocator.WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {}, 0, oversized.data(),
                                  oversized.size(), table, 0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }
    const auto ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() /
        (frames * 2800);
    if (profile)
        std::cout << "planning_ns/draw=" << plan_ns / (frames * 2800)
                  << " table_lookup_copy_ns/draw=" << table_ns / (frames * 2800) << '\n';
    if (benchmark)
        std::cout << "patch=" << plan.patch_keys << " ns/draw=" << ns
                  << " allocations=" << allocator.allocations
                  << " copies=" << allocator.context.copies
                  << " resolutions=" << allocator.resolutions << '\n';
    else
        std::cout << "digest=" << digest << " api_digest=" << api_digest
                  << " allocations=" << allocator.allocations
                  << " copies=" << allocator.context.copies << " writes=" << allocator.writes
                  << " resolutions=" << allocator.resolutions
                  << " reuses=" << allocator.context.reuses << '\n';
}
