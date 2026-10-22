# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay the production CPU descriptor resolver against its uncached path."""

import subprocess
import tempfile
import unittest
from pathlib import Path

import test_d3d12_descriptor_ranges as ranges
import test_d3d12_descriptor_setup as setup
from test_d3d12_subresource_feedback import function

DRIVER = r"""
int main() {
    for (bool enabled : {false, true}) {
        D3D12DescriptorAllocator allocator;
        allocator.descriptor_lookup_cache = enabled;
        allocator.view_cache.cpu = {1024};
        allocator.view_cache.capacity = 4;
        allocator.sampler_cache.cpu = {2048};
        allocator.sampler_cache.capacity = 3;
        allocator.null_view_cache.cpu = {4096};
        allocator.null_view_cache.capacity = 2;
        D3D12::DescriptorLookupCache<DescriptorKey, D3D12_CPU_DESCRIPTOR_HANDLE, 1> memo;
        auto original = Key(7);
        memo.Store(1, original, {42});
        assert(memo.Find(1, original)->ptr == 42);
        assert(!memo.Find(2, original));
        for (unsigned byte = 0; byte < original.description.size(); ++byte) {
            auto changed = original;
            changed.description[byte] ^= 1;
            assert(!memo.Find(1, changed));
        }
        for (unsigned field = 0; field < 3; ++field) {
            auto changed = original;
            if (field == 0) ++changed.resource;
            if (field == 1) ++changed.kind;
            if (field == 2) ++changed.size;
            assert(!memo.Find(1, changed));
            memo.Store(1, changed, {43});
            assert(!memo.Find(1, original));
            memo.Store(1, original, {42});
        }
        auto owner = std::make_shared<Resource>();
        std::weak_ptr<Resource> lifetime = owner;
        std::uint64_t digest = 0;
        unsigned state = 119;
        for (unsigned step = 0; step < 30000; ++step) {
            if (step % 71 == 0) {
                allocator.new_frame = true;
                ++allocator.context.tick;
            }
            allocator.RefreshCaches();
            state = state * 1664525u + 1013904223u;
            auto key = Key((state >> 20) % 9);
            const bool sampler = step % 5 == 0;
            const auto type = sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                                      : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            key.kind = sampler ? 3 : (step % 7 == 0 ? 2 : 1);
            // Deliberately collide the quick index, varying every byte outside it.
            key.description[63] = static_cast<u8>(step / 31);
            key.resource = step % 3 ? 0x1000 : 0;
            const auto resource = key.resource ? owner : ResourcePtr{};
            const auto first = allocator.ResolveDescriptor(type, resource, key, nullptr);
            const auto writes = allocator.context.device.writes;
            for (unsigned repeat = 0; repeat < 8; ++repeat) {
                const auto same = allocator.ResolveDescriptor(type, resource, key, nullptr);
                assert(same.ptr == first.ptr && allocator.context.device.writes == writes);
            }
            digest = digest * 131 + first.ptr;
        }
        // Generation invalidation must release the map's pin; the front cache
        // contains only numeric keys/handles and must not extend resource lifetime.
        owner.reset();
        allocator.view_cache.entries.clear();
        ++allocator.view_cache.generation;
        allocator.sampler_cache.entries.clear();
        ++allocator.sampler_cache.generation;
        assert(lifetime.expired());
        std::cout << digest << ':' << allocator.context.device.writes << '\n';
    }
}
"""

SCRATCH_DRIVER = r"""
int main() {
    D3D12DescriptorAllocator allocator;
    allocator.descriptor_fastpath = true;
    DescriptorPlan plan{{0, 0, 0}};
    for (unsigned draw = 0; draw < 48; ++draw) {
        allocator.descriptor_ranges = draw % 2 != 0;
        // Exercise the entire persistent scratch, then a smaller prefix. Old
        // range counts and sizes must never leak across heap domains or calls.
        const u32 count = draw % 3 == 0 ? 4096 : draw % 3 == 1 ? 1 : 127;
        plan.Reset({0, count, 0});
        plan.sources.generation = 1;
        std::vector<DescriptorKey> expected;
        for (u32 slot = 0; slot < count; ++slot) {
            auto key = DescriptorKey{};
            key.kind = slot % 4;
            key.description[0] = static_cast<u8>(slot);
            key.description[4] = static_cast<u8>(draw);
            auto [entry, inserted] = source_handles.try_emplace(key, source_handles.size() * 2 + 1);
            (void)inserted;
            cpu_keys[entry->second] = key;
            plan.sources.handles[slot] = entry->second;
            plan.keys[slot] = key;
            expected.push_back(key);
        }
        const auto table = allocator.FinishDrawTable(plan, draw % 2 != 0);
        for (u32 slot = 0; slot < count; ++slot)
            assert(gpu_keys.at(table.cpu.ptr + slot) == expected[slot]);
        ++allocator.context.tick;
    }
}
"""


class DescriptorLookupCacheTests(unittest.TestCase):
    def compile_run(self, source, conversions=True):
        with tempfile.TemporaryDirectory(prefix="nxbox-p74-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            cpp.write_text(source)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    *(["-Wconversion"] if conversions else []),
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ranges.ROOT / "src"),
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            return subprocess.run(
                [str(binary)],
                check=True,
                capture_output=True,
                text=True,
                timeout=30,
            ).stdout

    def test_production_resolver_matches_legacy(self):
        cpp = (ranges.NATIVE / "d3d12_descriptor.cpp").read_text()
        methods = "\n".join(
            function(cpp, name)
            for name in (
                "void D3D12DescriptorAllocator::RefreshCaches()",
                "D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::ResolveDescriptor(",
            )
        )
        fixture = ranges.NULL_DRIVER.split("int main() {", 1)[0]
        fixture = fixture.replace("assert(!resource);", "(void)resource;")
        output = self.compile_run(fixture.replace("__PRODUCTION__", methods) + DRIVER)
        before, after = output.splitlines()
        self.assertEqual(before, after)

    def test_full_scratch_reused_across_domains_and_sizes(self):
        source = setup.source().split("int main(", 1)[0] + SCRATCH_DRIVER
        # Existing planner fixture/header have unrelated signed-size conversions.
        self.compile_run(source, conversions=False)


BENCH_DRIVER = r"""
#include <chrono>
int main() {
    for (unsigned sample = 0; sample < 10; ++sample) {
        D3D12DescriptorAllocator allocator;
        allocator.descriptor_lookup_cache = sample % 2 != 0;
        allocator.persistent_null_descriptors = false;
        allocator.view_cache.cpu = {1024};
        allocator.view_cache.capacity = 64;
        const auto resource = std::make_shared<Resource>();
        std::array<DescriptorKey, 32> keys;
        for (unsigned index = 0; index < keys.size(); ++index) {
            keys[index] = Key(index);
            keys[index].resource = 0x1000;
            keys[index].size = 40;
            allocator.ResolveDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                        resource, keys[index], nullptr);
        }
        hash_calls = 0;
        std::uint64_t digest = 0;
        const auto begin = std::chrono::steady_clock::now();
        constexpr unsigned iterations = 2000000;
        for (unsigned iteration = 0; iteration < iterations; ++iteration) {
            digest += allocator.ResolveDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                        resource, keys[iteration % keys.size()], nullptr).ptr;
        }
        const auto ns = std::chrono::duration<double, std::nano>(
                            std::chrono::steady_clock::now() - begin).count() / iterations;
        std::cout << "cache=" << allocator.descriptor_lookup_cache << " ns/resolve=" << ns
                  << " map_hashes=" << hash_calls << " digest=" << digest << '\n';
    }
}
"""


def benchmark():
    cpp = (ranges.NATIVE / "d3d12_descriptor.cpp").read_text()
    methods = function(
        cpp, "D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::ResolveDescriptor("
    )
    fixture = ranges.NULL_DRIVER.split("int main() {", 1)[0]
    fixture = fixture.replace("assert(!resource);", "(void)resource;")
    fixture = fixture.replace("struct KeyHash {", "std::uint64_t hash_calls = 0;\nstruct KeyHash {")
    fixture = fixture.replace(
        "return std::hash<std::string_view>{}(",
        "++hash_calls; return std::hash<std::string_view>{}(",
    )
    with tempfile.TemporaryDirectory(prefix="nxbox-p74-bench-") as directory:
        source = Path(directory) / "bench.cpp"
        binary = Path(directory) / "bench"
        source.write_text(fixture.replace("__PRODUCTION__", methods) + BENCH_DRIVER)
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-O3",
                "-I",
                str(ranges.ROOT / "src"),
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    import sys

    if "--bench" in sys.argv:
        benchmark()
    else:
        unittest.main()
