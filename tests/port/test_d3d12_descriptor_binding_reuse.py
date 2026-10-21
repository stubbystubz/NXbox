# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare cached GPU descriptor/root state to unconditional binding on random draws."""

import subprocess
import tempfile
import unittest
from pathlib import Path

import test_d3d12_deferred_recording as recorder_tests

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class DescriptorBindingReuseTests(unittest.TestCase):
    def test_random_old_path_equivalence(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-descriptor-binding-") as directory:
            binary = Path(directory) / "test"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests/port/d3d12_descriptor_binding_reuse_test.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_production_facade_indirect_reset(self):
        def source():
            return (
                recorder_tests.source().split("int main() {", 1)[0]
                + r"""
int main() {
    ID3D12GraphicsCommandList list;
    ID3D12RootSignature signature;
    ID3D12CommandSignature indirect;
    ID3D12Resource arguments;
    for (const char* value : std::array<const char*, 3>{nullptr, "1", "0"}) {
        if (value) setenv("NXBOX_D3D12_ROOT_CBV_REUSE", value, 1);
        else unsetenv("NXBOX_D3D12_ROOT_CBV_REUSE");
        CommandFacade policy;
        assert(policy.reuse_graphics_root_cbvs == (!value || std::strcmp(value, "0") != 0));
    }
    unsetenv("NXBOX_D3D12_ROOT_CBV_REUSE");
    for (bool deferred : {false, true}) {
        CommandFacade facade;
        assert(facade.reuse_graphics_root_cbvs); // Default-on production initializer.
        RecordStream<> stream;
        facade.immediate = &list;
        if (deferred) facade.stream = &stream;
        facade.SetGraphicsRootSignature(&signature);
        facade.SetGraphicsRootConstantBufferView(4, 0x123400);
        assert(!facade.graphics_root_cbvs.cbvs.Update(4, 0x123400));
        facade.ExecuteIndirect(&indirect, 1, &arguments, 0, nullptr, 0);
        assert(facade.graphics_root_cbvs.cbvs.Update(4, 0x123400));
        // Reasserting the same signature after indirect also requires a full rebind.
        facade.SetGraphicsRootSignature(&signature);
        assert(facade.graphics_root_cbvs.cbvs.Update(4, 0x123400));
        stream.Close();
        while (stream.Consume(nullptr, false)) {}
    }
    std::cout << "production facade: immediate/deferred indirect reset forces identical CBV rebind\n";
}
"""
            )

        recorder_tests.DeferredRecording.run_binary(self, "address,undefined", source)

    def test_root_cache_tracks_actual_writes_and_list_reset(self):
        commands = (NATIVE / "d3d12_commands.h").read_text()
        for name in (
            "32BitConstants",
            "ShaderResourceView",
            "UnorderedAccessView",
            "DescriptorTable",
        ):
            method = commands.split(f"void SetGraphicsRoot{name}(", 1)[1].split("\n    }", 1)[0]
            self.assertIn("graphics_root_cbvs.Invalidate(index);", method)
        self.assertIn("graphics_root_cbvs.SetSignature(", commands)
        self.assertIn("graphics_root_cbvs.cbvs.Update(index, address);", commands)
        context = (NATIVE / "d3d12_context.cpp").read_text()
        start = context.split("CommandFacade* Context::Commands()", 1)[1].split(
            "void Context::MarkWork", 1
        )[0]
        self.assertIn("facade.graphics_root_cbvs.Reset();", start)
        draw = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        self.assertIn('std::getenv("NXBOX_D3D12_ROOT_CBV_REUSE")', commands)
        indirect = commands.split("void ExecuteIndirect(", 1)[1].split("\n    }", 1)[0]
        self.assertLess(
            indirect.index("graphics_root_cbvs.Reset();"), indirect.index("if (!stream)")
        )
        self.assertIn("root_cbv_reuse ? root_changed : changed", draw)
        allocator = (NATIVE / "d3d12_descriptor.cpp").read_text()
        self.assertIn('std::getenv("NXBOX_D3D12_HANDLE_TABLE_CACHE")', allocator)
        self.assertIn("plan.sources.Current(cache.generation)", allocator)
        self.assertIn("FindHandleDrawTable(plan, sampler)", allocator)


if __name__ == "__main__":
    unittest.main()
