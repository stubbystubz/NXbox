# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay the production descriptor planner/finisher; also usable as a host benchmark."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_d3d12_draw_scratch as scratch
from test_d3d12_subresource_feedback import function

ROOT = scratch.ROOT


def source():
    header = (scratch.NATIVE / "d3d12_descriptor.h").read_text()
    cpp = (scratch.NATIVE / "d3d12_descriptor.cpp").read_text()
    structs = (
        "struct DescriptorKey {"
        + header.split("struct DescriptorKey {", 1)[1].split("struct DescriptorTable {", 1)[0]
    )
    mock = scratch.MOCK.replace(
        "using ResourcePtr = std::shared_ptr<int>;",
        "struct Resource { struct Handle { Resource* Get() const { return owner; } "
        "Resource* owner; } handle{this}; }; using ResourcePtr = std::shared_ptr<Resource>;",
    )
    allocator = (
        scratch.ALLOCATOR.replace(
            "__IS_NULL_DESCRIPTOR_HANDLE__", function(header, "bool IsNullDescriptorHandle(")
        )
        .replace(
            "u32 first = 0;", "u32 first = 0; DescriptorPlan* plan = nullptr; u32 count = 4096;"
        )
        .replace("u32, CopyBatch*) { ++writes; }", "u32, CopyBatch* = nullptr) { ++writes; }")
    )
    allocator = (
        allocator.replace(
            "return {key.kind + 10};",
            "auto [found, inserted] = source_handles.try_emplace(key, source_handles.size() * 2 + 1); "
            "const auto handle = found->second; cpu_keys[handle] = key; return {handle};",
        )
        .replace(
            "const D3D12_CPU_DESCRIPTOR_HANDLE*, const u32*, u32,\n"
            "                         const D3D12_CPU_DESCRIPTOR_HANDLE*, const u32*, int) { ++copies; }",
            "const D3D12_CPU_DESCRIPTOR_HANDLE* destination, const u32*, u32 ranges,\n"
            "const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const u32* sizes, int) {\n"
            "++copies; if (trace_api) { api_digest = api_digest * 131 + destination->ptr; "
            "api_digest = api_digest * 131 + ranges; for (u32 r = 0; r < ranges; ++r) { "
            "api_digest = api_digest * 131 + sources[r].ptr; "
            "api_digest = api_digest * 131 + (sizes ? sizes[r] : 1); }} "
            "auto out = destination->ptr; for (u32 r = 0; r < ranges; ++r) "
            "for (u32 i = 0; i < (sizes ? sizes[r] : 1); ++i) "
            "gpu_keys[out++] = cpu_keys.at(sources[r].ptr + i); }",
        )
        .replace(
            "return {++allocations};",
            "++allocations; DescriptorTable t; t.first = allocations; "
            "t.cpu.ptr = allocations * 4096; return t;",
        )
    )
    allocator = allocator.replace(
        "void ResolvePlanSources(DescriptorPlan&, bool = false);",
        "static DescriptorKey MakeDescriptorKey(const ResourcePtr&, u32, const void*, size_t);\n"
        "void WriteCached(int, const ResourcePtr&, u32, const void*, size_t, "
        "const DescriptorTable&, u32);\n"
        "void ResolvePlanSources(DescriptorPlan&, bool = false);",
    )
    methods = "\n".join(
        function(cpp, signature)
        for signature in (
            "DescriptorKey D3D12DescriptorAllocator::MakeDescriptorKey(",
            "void D3D12DescriptorAllocator::WriteCached(",
            "bool D3D12DescriptorAllocator::CanReuseDrawTable(",
            "bool D3D12DescriptorAllocator::CanReusePreviousDrawTable(",
            "const DescriptorTable* D3D12DescriptorAllocator::FindHandleDrawTable(",
            "const D3D12DescriptorAllocator::CachedDrawTable* D3D12DescriptorAllocator::FindCachedDrawTable(",
            "void D3D12DescriptorAllocator::RememberDrawTable(",
            "DescriptorTable D3D12DescriptorAllocator::FinishDrawTable(",
        )
    ).replace("D3D12_DESCRIPTOR_HEAP_TYPE type", "int type")
    banks = r"""
struct SourceHash {
    size_t operator()(const DescriptorKey& key) const {
        return key.resource ^ (key.kind << 8) ^ (key.description[0] << 16) ^ key.description[4];
    }
};
std::unordered_map<DescriptorKey, std::uintptr_t, SourceHash> source_handles;
std::unordered_map<std::uintptr_t, DescriptorKey> cpu_keys, gpu_keys;
bool trace_api = true;
std::uint64_t api_digest = 0;
"""
    return (
        mock
        + structs
        + banks
        + allocator
        + methods
        + (ROOT / "tests/port/d3d12_descriptor_setup_test.cpp").read_text()
    )


def run(benchmark=False, profile=False, repeats=1):
    with tempfile.TemporaryDirectory(prefix="nxbox-descriptor-setup-") as directory:
        cpp = Path(directory) / "setup.cpp"
        binary = Path(directory) / "setup"
        cpp.write_text(source())
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "src"),
                *(["-O3"] if benchmark else ["-fsanitize=address,undefined"]),
                str(cpp),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        results = []
        for sample in range(repeats):
            values = (
                (("0", "1") if sample % 2 == 0 else ("1", "0")) if benchmark else ("0", "1", None)
            )
            for value in values:
                environment = {**os.environ}
                if value is None:
                    environment.pop("NXBOX_D3D12_DESCRIPTOR_KEY_PATCH", None)
                else:
                    environment["NXBOX_D3D12_DESCRIPTOR_KEY_PATCH"] = value
                result = subprocess.run(
                    [str(binary), "profile" if profile else "bench" if benchmark else "check"],
                    env=environment,
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=60,
                )
                results.append(result.stdout)
        return results


class DescriptorSetupTests(unittest.TestCase):
    def test_production_replay(self):
        before, after, default = run()
        self.assertEqual(after, default)
        self.assertEqual(before, after)  # Same table handles, API counts and semantic keys.


if __name__ == "__main__":
    import sys

    if "--bench" in sys.argv or "--profile" in sys.argv:
        print("".join(run(benchmark=True, profile="--profile" in sys.argv, repeats=5)), end="")
    else:
        unittest.main()
