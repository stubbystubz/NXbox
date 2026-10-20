# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production command and memory helpers with host-only hardware stubs."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAXWELL = ROOT / "src/video_core/engines/maxwell_3d.cpp"
DEVICE_MEMORY = ROOT / "src/core/device_memory_manager.inc"
FIXTURES = ROOT / "tests/port/fixtures"


def method(source, name, occurrence=0):
    matches = list(
        re.finditer(
            rf"^[^\n]*\b{re.escape(name)}\([^;{{]*?\)[^\n]*\{{.*?^\}}",
            source,
            re.MULTILINE | re.DOTALL,
        )
    )
    match = matches[occurrence] if len(matches) > occurrence else None
    if match is None:
        raise AssertionError(f"Missing production method: {name}")
    return match.group()


class GpuFrontendHotPathTests(unittest.TestCase):
    def compile_and_run(self, source, environments=None):
        with tempfile.TemporaryDirectory(prefix="nxbox-frontend-") as directory:
            cpp = Path(directory) / "check.cpp"
            binary = Path(directory) / "check"
            cpp.write_text(source)
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    f"-I{ROOT / 'src'}",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            for environment in environments or [
                dict(os.environ, NXBOX_GPU_METHOD_PROFILE=value) for value in ("0", "1")
            ]:
                subprocess.run([str(binary)], check=True, timeout=30, env=environment)

    def test_registers_macros_and_assertions(self):
        source = MAXWELL.read_text()
        methods = "\n".join(
            method(source, f"Maxwell3D::{name}")
            for name in (
                "IsMethodExecutable",
                "ProcessShadowRam",
                "ProcessDirtyRegisters",
                "ProcessDirtyRegistersImpl",
                "ConsumeSinkImpl",
                "CallMethod",
                "CallMethodImpl",
                "CallMultiMethod",
                "CallCBDataIncrementing",
                "ProcessCBMultiData",
                "ProcessMacro",
                "RefreshParametersImpl",
                "GetRegisterValue",
            )
        )
        methods += "\n" + method(source, "Maxwell3D::CallMultiMethod", 1)
        # Synthetic register offsets keep the full production dispatch switches intact.
        # Leave room for const_buffer.buffer's sixteen adjacent method registers.
        fields = sorted(set(re.findall(r"MAXWELL3D_REG_INDEX\(([^)]+)\)", methods)))
        registers = "\n".join(
            f'    if (field == "{field}") return {32 * (index + 1)};'
            for index, field in enumerate(fields)
        )
        fixture = (FIXTURES / "gpu_frontend.cpp").read_text()
        self.compile_and_run(
            fixture.replace("// REGISTER_INDICES", registers).replace("// METHODS", methods)
        )

    def test_incrementing_cb_parser_boundaries(self):
        source = (ROOT / "src/video_core/dma_pusher.cpp").read_text()
        parser = method(source, "DmaPusher::ProcessCommands")
        start = parser.index("            constexpr u32 cb_begin")
        end = parser.index("            auto const command_header", start)
        reference = (parser[:start] + parser[end:]).replace(
            "DmaPusher::ProcessCommands", "ReferencePusher::ProcessCommands"
        )
        fixture = (FIXTURES / "gpu_frontend_dma.cpp").read_text()
        dispatch = "\n".join(
            method(source, f"DmaPusher::{name}") for name in ("CallMethod", "CallMultiMethod")
        )
        self.compile_and_run(
            fixture.replace("// METHODS", dispatch + "\n" + parser + "\n" + reference)
        )

    def test_execution_mask_covers_method_side_effects(self):
        source = MAXWELL.read_text()
        mask = method(source, "Maxwell3D::IsMethodExecutable")
        handlers = method(source, "Maxwell3D::ProcessMethodCall") + method(
            (MAXWELL.parent / "draw_manager.cpp").read_text(),
            "Maxwell3D::DrawManager::ProcessMethodCall",
        )
        cases = r"case (MAXWELL3D_REG_INDEX\([^)]+\)(?: \+ \d+)?):"
        self.assertLessEqual(set(re.findall(cases, handlers)), set(re.findall(cases, mask)))

    def test_fastpath_environment_is_latched(self):
        self.compile_and_run(
            r"""
#include <cassert>
#include <cstdlib>
#include "video_core/gpu_frontend.h"
int main() {
    const bool expected = std::getenv("NXBOX_GPU_FRONTEND_FASTPATH")[0] == '1';
    assert(Tegra::GpuFrontendFastPathEnabled() == expected);
    const bool batching = expected && std::getenv("NXBOX_MAXWELL_METHODS")[0] == '1';
    assert(Tegra::MaxwellMethodBatchingEnabled() == batching);
    setenv("NXBOX_MAXWELL_METHODS", batching ? "0" : "1", 1);
    assert(Tegra::MaxwellMethodBatchingEnabled() == batching);
    setenv("NXBOX_GPU_FRONTEND_FASTPATH", expected ? "0" : "1", 1);
    assert(Tegra::GpuFrontendFastPathEnabled() == expected);
}
""",
            [
                dict(os.environ, NXBOX_GPU_FRONTEND_FASTPATH=value, NXBOX_MAXWELL_METHODS=batching)
                for value in ("0", "1")
                for batching in ("0", "1")
            ],
        )

    def test_jit_zero_register_skip_preserves_result_and_live_carry(self):
        source = method(
            (ROOT / "src/video_core/macro.cpp").read_text(), "MacroJITx64Impl::Compile_ALU"
        )
        predicate = re.search(r"    const bool zero_reg_skip =.*?;", source, re.DOTALL).group()
        # Each ALU emitter must use the restricted decision, never the raw optimizer flag.
        self.assertEqual(source.count("optimizer.zero_reg_skip"), 1)
        enum = re.search(
            r"enum class ALUOperation : u32 \{.*?\};",
            (ROOT / "src/video_core/macro.h").read_text(),
            re.DOTALL,
        ).group()
        self.compile_and_run(
            (FIXTURES / "gpu_frontend_jit.cpp")
            .read_text()
            .replace("// ALU_ENUM", enum)
            .replace("// PREDICATE", predicate)
        )

    def test_sink_matches_independent_legacy_oracle(self):
        source = MAXWELL.read_text()
        methods = "\n".join(
            method(source, f"Maxwell3D::{name}").replace("Maxwell3D::", "NewM3D::")
            for name in (
                "ProcessShadowRam",
                "ConsumeSinkImpl",
                "ProcessDirtyRegisters",
                "ProcessDirtyRegistersImpl",
            )
        )
        self.compile_and_run(
            (FIXTURES / "gpu_frontend_sink_oracle.cpp").read_text().replace("// METHODS", methods)
        )

    def test_jit_alu_emission_matches_interpreter(self):
        source = (ROOT / "src/video_core/macro.cpp").read_text()
        production = "\n".join(
            method(source, name)
            for name in (
                "MacroJITx64Impl::Compile_GetRegister",
                "MacroJITx64Impl::Compile_ALU",
                "MacroInterpreterImpl::GetALUResult",
            )
        )
        self.compile_and_run(
            (FIXTURES / "gpu_frontend_jit_exec.cpp")
            .read_text()
            .replace("// PRODUCTION", production)
        )

    def test_jit_immediate_emission_preserves_two(self):
        source = (ROOT / "src/video_core/macro.cpp").read_text()
        production = "\n".join(
            method(source, f"MacroJITx64Impl::{name}")
            for name in (
                "Compile_GetRegister",
                "Compile_AddImmediate",
                "Compile_Read",
            )
        )
        self.compile_and_run(
            (FIXTURES / "gpu_frontend_jit_immediate.cpp")
            .read_text()
            .replace("// PRODUCTION", production)
        )

    def test_memory_spans_and_walks(self):
        source = DEVICE_MEMORY.read_text()
        spans = re.findall(
            r"^template <typename Traits>\n(?:const )?u8\* "
            r"DeviceMemoryManager<Traits>::GetSpan\(.*?^\}",
            source,
            re.MULTILINE | re.DOTALL,
        )
        self.assertEqual(len(spans), 2)
        walk = "template <typename Traits>\n" + method(
            source, "DeviceMemoryManager<Traits>::WalkBlock"
        )
        fixture = (FIXTURES / "gpu_frontend_memory.cpp").read_text()
        self.compile_and_run(fixture.replace("// METHODS", "\n".join([*spans, walk])))


if __name__ == "__main__":
    unittest.main()
