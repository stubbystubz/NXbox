#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a host replay from production method bodies, without emulator dependencies.

The existing sanitizer fixtures supply reduced register layouts and hardware stubs.
The DMA parser, Maxwell dispatch, dirty/shadow logic, and DrawManager are extracted
verbatim. --revision reads only production code from git; both runs share stubs.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tests/port"))
from test_gpu_frontend_hot_paths import method

root = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(
    description="Replay production Maxwell/DMA methods with a null rasterizer"
)
parser.add_argument("--revision", help="Read production methods from this git revision")
parser.add_argument("--frames", type=int, default=3000)
parser.add_argument("--repeat", type=int, default=5)
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--emit-source", type=Path)
args = parser.parse_args()
if args.frames < 1 or args.repeat < 1:
    parser.error("--frames and --repeat must be positive")


def production(path):
    if args.revision:
        return subprocess.check_output(
            ["git", "show", f"{args.revision}:{path}"], cwd=root, text=True
        )
    return (root / path).read_text()


f = (root / "tests/port/fixtures/gpu_frontend.cpp").read_text().split("int main()")[0]
draw = (root / "tests/port/fixtures/gpu_pipeline/maxwell_replay.cpp").read_text()
regs = draw.split("    struct Regs {", 1)[1].split("    // DRAW_MANAGER", 1)[0]
regs = regs.rsplit("    };", 1)[0]
# Add the remaining frontend register types and fields to the anonymous register struct.
regs = regs.replace(
    "        union {\n            struct {\n                u32 clear_surface;",
    """        static constexpr u32 NUM_REGS = MacroRegistersStart;
        enum class ShadowRamControl { Off, Track, TrackWithFilter, Replay };
        struct LaunchDMA { enum class Layout { Pitch }; struct { Layout Value() const { return Layout::Pitch; } } memory_layout; };
        union {
            struct {
                u32 puller_padding[64];
                struct {
                    u32 offset, size;
                    u32 buffer[16];
                    GPUVAddr Address() const { return 64; }
                } const_buffer;
                ShadowRamControl shadow_ram_control;
                struct { GPUVAddr Address() const { return 0; } } program_region;
                struct { u32 instruction_ptr; } load_mme;
                LaunchDMA launch_dma;
                u32 clear_surface;""",
)
a = f.index("    struct Regs {")
b = f.index("    struct {\n        std::bitset<256>", a)
f = f[:a] + "    struct Regs {" + regs + "    } regs{}, shadow_state{};\n" + f[b:]
f = re.sub(
    r"    void ProcessMethodCall\(u32 method, u32, u32, bool\) \{.*?\n    \}",
    "    void ProcessMethodCall(u32, u32, u32, bool);",
    f,
    flags=re.DOTALL,
)

h = production("src/video_core/engines/maxwell_3d.h")
decl = (
    "    struct DrawManager {"
    + h.split("    struct DrawManager {", 1)[1].split("    Regs regs{};", 1)[0]
)
f = f.replace(
    "    bool IsMethodExecutable(u32);",
    decl
    + """    DrawManager draw_manager;
    Rasterizer rasterizer_storage;
    Rasterizer* rasterizer{&rasterizer_storage};
    bool ShouldExecute() const { return true; }
    bool IsMethodExecutable(u32);""",
)
f = f.replace(
    "constexpr u32 RegisterIndex(std::string_view field) {",
    "constexpr u32 RegisterIndex(std::string_view field);\n/*",
)
f = f.replace(
    "#define MAXWELL3D_REG_INDEX(field) RegisterIndex(#field)",
    "*/\n#define MAXWELL3D_REG_INDEX(field) RegisterIndex(#field)",
)
f = f.replace(
    "#include <array>",
    """#include <algorithm>
#include <map>
#include <span>
#include <optional>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <array>""",
    1,
)
prefix = draw[draw.index("using u8 =") : draw.index("namespace Tegra::Engines {")]
prefix = prefix.replace("using GPUVAddr = std::uint64_t;", "").replace(
    "#define MAXWELL3D_REG_INDEX(field) static_cast<u32>(offsetof(Maxwell3D::Regs, field) / 4)", ""
)
prefix = (
    prefix[: prefix.index("struct Rasterizer {")]
    + """struct Rasterizer {
    std::uint64_t draws{}, checksum{};
    __attribute__((noinline)) void Draw(bool indexed, u32 instances) { ++draws; checksum += indexed + instances; }
    void Clear(u32) {} void DrawIndirect() {} void WaitForIdle() {}
    void FragmentBarrier() {} void InvalidateGPUCache() {} void TiledCacheBarrier() {}
};
"""
)
f = f.replace("struct Maxwell3D {", prefix + "struct Maxwell3D {", 1)
f = f.replace(
    "    bool IsMethodExecutable(u32);",
    """
    struct MacroStub { void ClearCode(u32) {} void AddCode(u32, u32) {} } macro_engine;
    void ProcessMacroBind(u32) {} void ProcessFirmwareCall4() {}
    void ProcessCBData(u32 value) { ProcessCBMultiData(&value, 1); }
    void ProcessCBBind(size_t) {} void ProcessQueryGet() {} void ProcessQueryCondition() {}
    void ProcessCounterReset() {} void ProcessSyncPoint() {}
    bool IsMethodExecutable(u32);""",
)
f = f.replace(
    "        unsigned calls{};",
    """        void ProcessExec(bool) {} void ProcessData(u32, bool) {}
        unsigned calls{};""",
)
# all real draw fields get their real offsets; other frontend-only side effects get unused offsets.
s = production("src/video_core/engines/maxwell_3d.cpp")
methods = (
    "\n".join(
        method(s, "Maxwell3D::" + name)
        for name in (
            "IsMethodExecutable",
            "ProcessShadowRam",
            "ProcessDirtyRegisters",
            "ProcessDirtyRegistersImpl",
            "ProcessMethodCall",
            "ConsumeSinkImpl",
            "CallMethod",
            "CallMultiMethod",
            "CallCBDataIncrementing",
            "ProcessCBMultiData",
            "ProcessMacro",
            "RefreshParametersImpl",
            "GetRegisterValue",
        )
    )
    + "\n"
    + method(s, "Maxwell3D::CallMultiMethod", 1)
)
if "void Maxwell3D::CallMethodImpl" in s:
    methods += "\n" + method(s, "Maxwell3D::CallMethodImpl")
bodies = (
    production("src/video_core/engines/draw_manager.cpp")
    .split("void Maxwell3D::DrawManager::ProcessMethodCall", 1)[1]
    .rsplit("} // namespace Tegra::Engines", 1)[0]
)
bodies = "void Maxwell3D::DrawManager::ProcessMethodCall" + bodies
a = bodies.index("void Maxwell3D::DrawManager::DrawTexture(")
b = bodies.index("void Maxwell3D::DrawManager::UpdateTopology(", a)
bodies = bodies[:a] + "void Maxwell3D::DrawManager::DrawTexture(Maxwell3D&) {}\n" + bodies[b:]
fields = sorted(set(re.findall(r"MAXWELL3D_REG_INDEX\(([^)]+)\)", methods)))
real = set(re.findall(r"MAXWELL3D_REG_INDEX\(([^)]+)\)", bodies)) | {
    "const_buffer.buffer",
    "program_region.address_high",
    "program_region.address_low",
    "shadow_ram_control",
}
real -= {"program_region.address_high", "program_region.address_low"}
indices = "constexpr u32 RegisterIndex(std::string_view field) {\n"
for i, field in enumerate(fields):
    value = f"offsetof(Maxwell3D::Regs, {field}) / 4" if field in real else str(512 + 32 * i)
    indices += f'    if (field == "{field}") return {value};\n'
indices += "    return 0;\n}\n"
f = f.replace("// METHODS", indices + "\n" + methods + "\n" + bodies)
dma = (root / "tests/port/fixtures/gpu_frontend_dma.cpp").read_text()
a = dma.index("enum class SubmissionMode")
b = dma.index("struct ReferencePusher")
dma = dma[a:b]
dma = (
    dma.replace("Engines::Engine*", "Maxwell3D*")
    .replace("Engines::Maxwell3D", "Maxwell3D")
    .replace("Engines::Engine engine2", "Maxwell3D engine2")
)
# stub puller declarations require forward DmaPusher, retained in copied block.
dispatch = production("src/video_core/dma_pusher.cpp")
dma += (
    "\n"
    + """void Engines::Puller::CallPullerMethod(DmaPusher&, MethodCall) { std::abort(); }
void Engines::Puller::CallMultiMethod(DmaPusher&, u32, u32, const u32*, u32, u32) { std::abort(); }
"""
)
dma += "\n".join(
    method(dispatch, "DmaPusher::" + n)
    for n in ("CallMethod", "CallMultiMethod", "ProcessCommands")
)
f = f.replace(
    "namespace Core {",
    "namespace Engines { enum class EngineTypes { Maxwell3D, Other }; }\nnamespace Core {",
    1,
)
f = f.replace(
    "    bool IsMethodExecutable(u32);",
    """    std::array<std::uint16_t, MacroRegistersStart> register_write_runs{};
    void InitializeRegisterWriteRuns();
    u32 GetRegisterWriteRunLength(u32 method) const {
        return register_write_runs[method];
    }
    bool IsMethodExecutable(u32);""",
)
f = f.replace(
    "        ++sink_calls;", "        if (!method_sink.empty()) ConsumeSinkImpl(system);"
).replace("    void ConsumeSink(Core::System&) {", "    void ConsumeSink(Core::System& system) {")
f = f.replace(
    "    }\n};\n\n// METHODS", "        InitializeRegisterWriteRuns();\n    }\n};\n\n// METHODS", 1
)
f += "\n" + method(s, "Maxwell3D::InitializeRegisterWriteRuns") + "\n" + dma
# Use the DMA bitfield type for both command headers and draw registers.
a = f.index("template <unsigned Shift, unsigned Bits, class T = u32>")
b = f.index("struct Rasterizer {", a)
f = f[:a] + f[b:]
a = f.index("template <unsigned Shift, unsigned Bits, typename T = u32>")
b = f.index("union CommandHeader", a)
field = f[a:b].replace(
    "    Field& operator=", "    T Value() const { return *this; }\n    Field& operator="
)
f = f[:a] + f[b:]
f = f.replace("struct Rasterizer {", field + "struct Rasterizer {", 1)
f = f.replace(
    "using u32 = std::uint32_t;",
    "using u64 = std::uint64_t;\nusing s64 = std::int64_t;\nusing u32 = std::uint32_t;",
    1,
)
f = f.replace(
    "namespace Engines { enum class EngineTypes",
    "struct Maxwell3D;\nnamespace Engines { using Maxwell3D = ::Maxwell3D; enum class EngineTypes",
    1,
)
f = f.replace(
    '#include "video_core/gpu_frontend.h"',
    '#include "video_core/gpu_frontend.h"\n#include "video_core/gpu_draw_pipeline.h"',
)

f = f.replace("using u64 =", "using u16 = std::uint16_t;\nusing u64 =")
f += "\n" + (Path(__file__).parent / "maxwell_main.inc").read_text()
if args.emit_source:
    args.emit_source.write_text(f)
with tempfile.TemporaryDirectory(prefix="nxbox-maxwell-bench-") as directory:
    cpp = Path(directory) / "replay.cpp"
    binary = Path(directory) / "replay"
    cpp.write_text(f)
    flags = ["-O1", "-g", "-fsanitize=address,undefined"] if args.sanitize else ["-O3"]
    subprocess.run(
        [
            *shlex.split(os.environ.get("CXX", "c++")),
            "-std=c++20",
            *flags,
            f"-I{root / 'src'}",
            str(cpp),
            "-o",
            str(binary),
        ],
        check=True,
    )
    for _ in range(args.repeat):
        for profile in ("0", "1"):
            subprocess.run(
                [str(binary), str(args.frames), profile],
                check=True,
                env=dict(os.environ, NXBOX_GPU_METHOD_PROFILE="0"),
            )
