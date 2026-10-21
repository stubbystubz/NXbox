# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host replay, ownership and ordering contracts for the optional draw pipeline."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class GpuPipelineTests(unittest.TestCase):
    def test_replay(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-pipeline-") as directory:
            binary = Path(directory) / "replay"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O1",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-pthread",
                    "-fsanitize=address,undefined",
                    f"-I{ROOT / 'src'}",
                    str(ROOT / "tests/port/gpu_pipeline_test.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run(
                [str(binary), str(ROOT / "tests/port/fixtures/gpu_pipeline/methods.txt")],
                check=True,
                timeout=60,
            )

    def test_syncpoint_pipeline_wait_cycle(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-pipeline-deadlock-") as directory:
            root = Path(directory)
            # Keep the real syncpoint manager and queue; stub only platform types
            # and profiling so no D3D12 or emulator runtime is needed on the host.
            common = root / "common"
            common.mkdir()
            (common / "common_types.h").write_text(
                "#pragma once\n#include <cstdint>\nusing u32 = std::uint32_t;\n"
            )
            (common / "gpu_thread_profile.h").write_text(
                "#pragma once\nnamespace Common::GpuProfile {\n"
                "using Tick = unsigned long long;\n"
                "enum class Wait { SyncpointHost, SyncpointGuest };\n"
                "struct WaitScope { explicit WaitScope(Wait) {} };\n}\n"
            )
            binary = root / "deadlock"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-pthread",
                    "-fsanitize=address,undefined",
                    f"-I{root}",
                    f"-I{ROOT / 'src'}",
                    str(ROOT / "tests/port/gpu_pipeline_deadlock_test.cpp"),
                    str(ROOT / "src/video_core/host1x/syncpoint_manager.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_production_maxwell_method_replay(self):
        header = (ROOT / "src/video_core/engines/maxwell_3d.h").read_text()
        declaration = header.split("    struct DrawManager {", 1)[1].split("    Regs regs{};", 1)[0]
        source = (ROOT / "src/video_core/engines/draw_manager.cpp").read_text()
        # Keep the production dispatch and all draw-state transitions. Hardware-only
        # DrawTexture is stubbed because textured blits stay synchronous.
        bodies = source[source.index("void Maxwell3D::DrawManager::ProcessMethodCall") :]
        bodies = bodies.rsplit("} // namespace Tegra::Engines", 1)[0]
        start = bodies.index("void Maxwell3D::DrawManager::DrawTexture(")
        end = bodies.index("void Maxwell3D::DrawManager::UpdateTopology(", start)
        bodies = bodies[:start] + bodies[end:]
        names = sorted(set(re.findall(r"MAXWELL3D_REG_INDEX\(([^)]+)\)", bodies)))
        names += [
            "global_base_instance_index",
            "global_base_vertex_index",
            "primitive_topology_control",
            "topology_override",
        ]
        fixture = (ROOT / "tests/port/fixtures/gpu_pipeline/maxwell_replay.cpp").read_text()
        fixture = fixture.replace("// DRAW_MANAGER", "struct DrawManager {" + declaration)
        fixture = fixture.replace("// DRAW_METHODS", bodies)
        fixture = fixture.replace(
            "// REGISTER_NAMES",
            "\n".join(f'offsets["{name}"] = MAXWELL3D_REG_INDEX({name});' for name in names),
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-maxwell-replay-") as directory:
            source_path = Path(directory) / "replay.cpp"
            binary = Path(directory) / "replay"
            source_path.write_text(fixture)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-O1",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-Wno-unused-parameter",
                    "-pthread",
                    "-fsanitize=address,undefined",
                    f"-I{ROOT / 'src'}",
                    str(source_path),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run(
                [str(binary), str(ROOT / "tests/port/fixtures/gpu_pipeline/maxwell_methods.txt")],
                check=True,
                timeout=60,
            )

    def test_production_synchronous_dispatch(self):
        header = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.h").read_text()
        start = header.index("    template <class Func>\n    auto RunGpuPipeline")
        end = header.index("\n    void Draw(bool", start)
        dispatch = header[start:end]
        native = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        tick = native.split("void RasterizerD3D12::TickWork() {", 1)[1].split("\n}", 1)[0]
        program = r"""
#include <array>
#include <bitset>
#include <cassert>
#include <map>
#include <chrono>
#include "common/cancellable_sync_queue.h"
#include "video_core/gpu_draw_pipeline.h"
struct Engine {
    unsigned engine_state{};
    std::array<unsigned, 4> regs{}, state{}, draw_manager{};
    struct { std::bitset<256> flags; } dirty;
    std::map<unsigned, unsigned> replace_table;
};
struct Rasterizer {
    struct GPU {
        Common::CancellableSyncQueue requests;
        void TickWork() { requests.Tick(); }
    } m_gpu;
    void TickWork() { // TICK_BODY
    }
    std::unique_ptr<VideoCommon::GpuDrawPipeline> gpu_pipeline;
    Engine frontend, consumer;
    Engine* frontend_maxwell = &frontend;
    Engine* maxwell3d = &consumer;
    inline static thread_local const Rasterizer* pipeline_frontend_owner{};
    bool OnGpuPipeline() const { return gpu_pipeline && !gpu_pipeline->IsConsumer(); }
    // DISPATCH
};
int main() {
    Rasterizer renderer;
    assert(renderer.RunGpuPipeline([] { return 3; }) == 3); // Disabled mode is inline.
    renderer.gpu_pipeline = std::make_unique<VideoCommon::GpuDrawPipeline>(1);
    renderer.pipeline_frontend_owner = &renderer;
    renderer.frontend.regs[0] = 17;
    renderer.frontend.dirty.flags.set(4);
    renderer.RunGpuPipeline([&] {
        assert(renderer.gpu_pipeline->IsConsumer());
        assert(renderer.consumer.regs[0] == 17 && renderer.consumer.dirty.flags[4]);
        renderer.consumer.dirty.flags.set(9);
        renderer.RunGpuPipeline([&] { assert(renderer.consumer.regs[0] == 17); });
    });
    assert(renderer.frontend.dirty.flags.none());
    renderer.frontend.regs[0] = 23;
    auto invalidation = std::async(std::launch::async, [&] {
        renderer.RunGpuPipeline([&] {
            // A CPU invalidation must not read the producer's live registers.
            assert(renderer.consumer.regs[0] == 17);
            assert(renderer.consumer.dirty.flags[9]);
        });
    });
    invalidation.get();
    unsigned object = 1;
    unsigned& reference = renderer.RunGpuPipeline([&]() -> unsigned& {
        assert(renderer.consumer.regs[0] == 23 && renderer.consumer.dirty.flags[9]);
        return object;
    });
    reference = 42;
    assert(object == 42);

    // The producer cannot retire this marker until Invoke returns. Servicing
    // this CPU sync request inside the invoked draw/compute would form a cycle.
    std::promise<void> producer_returned;
    auto progress = producer_returned.get_future();
    auto fence = renderer.m_gpu.requests.Request([&] {
        assert(progress.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
    });
    renderer.RunGpuPipeline([&] { renderer.TickWork(); });
    assert(renderer.m_gpu.requests.Completed() < fence);
    producer_returned.set_value();
    renderer.TickWork();
    assert(renderer.m_gpu.requests.Completed() == fence);
}
""".replace("// DISPATCH", dispatch).replace("// TICK_BODY", tick)
        with tempfile.TemporaryDirectory(prefix="nxbox-pipeline-dispatch-") as directory:
            source = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            source.write_text(program)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Werror",
                    "-pthread",
                    "-fsanitize=address,undefined",
                    f"-I{ROOT / 'src'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_integration(self):
        native = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.cpp").read_text()
        self.assertIn('BisectEnabled("NXBOX_GPU_PIPELINE")', native)
        self.assertIn("CaptureDrawSnapshot(*frontend_maxwell)", native)
        self.assertIn("snapshot.Apply(*maxwell3d)", native)
        draw = native.split("void RasterizerD3D12::Draw(bool", 1)[1].split(
            "void RasterizerD3D12::DrawSynchronous", 1
        )[0]
        self.assertIn("DrawSynchronous(is_indexed, instance_count)", draw)
        self.assertIn("if (gpu_pipeline)\n        TickWork()", draw)
        self.assertEqual(native.count("m_gpu.TickWork();"), 1)
        for name in (
            "DrawIndirect() try",
            "DrawTexture()",
            "Clear(u32 layer_count) try",
            "DispatchCompute() try",
        ):
            entry = native.split(f"void RasterizerD3D12::{name}", 1)[1].split("\n    }", 1)[0]
            self.assertLess(entry.index("RunGpuPipeline"), entry.index("TickWork();"))
        teardown = native.split("RasterizerD3D12::~RasterizerD3D12()", 1)[1]
        self.assertLess(
            teardown.index("gpu_pipeline->Close()"), teardown.index("gpu_pipeline.reset()")
        )
        memory = (ROOT / "src/video_core/memory_manager.cpp").read_text()
        for name in (
            "GetPointer",
            "GetSpan",
            "ReadBlockImpl",
            "WriteBlockImpl",
            "FlushCaching",
            "Map",
            "MapSparse",
            "Unmap",
        ):
            for body in memory.split(f"MemoryManager::{name}(")[1:]:
                self.assertIn("DrainGpuPipeline", body.split("\n}", 1)[0], name)
        self.assertIn(
            "pipeline_maxwell",
            (ROOT / "src/video_core/control/channel_state_cache.cpp").read_text(),
        )
        self.assertIn(
            "RunGpuPipeline",
            (ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp").read_text(),
        )
        header = (ROOT / "src/video_core/renderer_d3d12/d3d12_rasterizer.h").read_text()
        # Every override except introspection/drain must enter the serialized consumer.
        header = header.split("class RasterizerD3D12 final", 1)[1]
        overrides = re.findall(
            r"(?:void|bool|RasterizerDownloadArea|AccelerateDMAInterface&)\s+(\w+)\([^;{]*?\) override",
            header,
            re.DOTALL,
        )
        for name in overrides:
            if name == "DrainGpuPipeline":
                continue
            body = native.split(f"RasterizerD3D12::{name}(", 1)[1].split("\n}", 1)[0]
            self.assertIn("OnGpuPipeline()", body, name)
        dma = (ROOT / "src/video_core/dma_pusher.cpp").read_text()
        self.assertIn("SnapshotGpuMethods(commands, pipeline_commands)", dma)
        lifecycle = native.split("void RasterizerD3D12::ProfileGpuThread", 1)[1].split(
            "void RasterizerD3D12::SignalSyncPoint", 1
        )[0]
        self.assertIn("if (!active)", lifecycle)
        self.assertIn("gpu_pipeline->Close()", lifecycle)


if __name__ == "__main__":
    unittest.main()
