# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay production draw packets against the old facade with random bit patterns."""

from pathlib import Path
import unittest

import test_d3d12_deferred_recording as recorder_tests

ROOT = Path(__file__).resolve().parents[2]


def random_source():
    context = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
    start = context.index('const char* value = std::getenv("NXBOX_D3D12_RECORD_STATE")')
    end = context.index("\n    }", start)
    policy = context[start:end].replace("facade.", "fast.")
    return (
        recorder_tests.source().split("int main() {", 1)[0]
        + r"""
#include <random>
// Model the API's effective dynamic state, including partial IA updates and
// the alias between standard and independent stencil references.
Log observable(const Log& input) {
    std::map<std::string, std::string> state;
    Log result;
    for (const auto& call : input) {
        bool setter = false;
        for (const char* method : {"IASetPrimitiveTopology", "IASetIndexBuffer",
                                  "OMSetBlendFactor", "RSSetViewports", "RSSetScissorRects"}) {
            if (call.starts_with(method)) { state[method] = call; setter = true; }
        }
        if (call.starts_with("IASetVertexBuffers")) {
            const char* p = call.data() + strlen("IASetVertexBuffers");
            UINT start, count; bool present; size_t size;
            memcpy(&start, p, sizeof(start)); p += sizeof(start);
            memcpy(&count, p, sizeof(count)); p += sizeof(count);
            memcpy(&present, p, sizeof(present)); p += sizeof(present);
            memcpy(&size, p, sizeof(size)); p += sizeof(size);
            assert(count == size);
            for (UINT i = 0; i < count; ++i) {
                D3D12_VERTEX_BUFFER_VIEW value{};
                if (present) { memcpy(&value, p, sizeof(value)); p += sizeof(value); }
                state["vertex" + std::to_string(start+i)] =
                    std::string(reinterpret_cast<const char*>(&value), sizeof(value));
            }
            setter = true;
        }
        if (call.starts_with("OMSetStencilRef") || call.starts_with("OMSetFrontAndBackStencilRef")) {
            const bool independent = call.starts_with("OMSetFront");
            const char* p = call.data() + strlen(independent ? "OMSetFrontAndBackStencilRef" : "OMSetStencilRef");
            UINT front, back;
            memcpy(&front, p, sizeof(front)); back = front;
            if (independent) memcpy(&back, p + sizeof(front), sizeof(back));
            state["front"] = std::string(reinterpret_cast<const char*>(&front), sizeof(front));
            state["back"] = std::string(reinterpret_cast<const char*>(&back), sizeof(back));
            setter = true;
        }
        if (!setter) {
            std::string event = call;
            for (const auto& [key, value] : state) { event += key; event += value; }
            result.push_back(event);
            if (call.starts_with("ExecuteIndirect")) {
                // Model a signature which modifies IA bindings and clears them
                // after execution; the following draw must explicitly rebind.
                state.erase("IASetIndexBuffer");
                for (auto it = state.begin(); it != state.end();) {
                    if (it->first.starts_with("vertex")) it = state.erase(it);
                    else ++it;
                }
            }
        }
    }
    return result;
}
int main() {
    std::mt19937 random(0x4e58424f);
    ID3D12CommandSignature signature;
    ID3D12Resource arguments;
    size_t old_calls = 0, new_calls = 0, batches = 0;
    CommandFacade old, fast;
    for (unsigned epoch = 0; epoch < 12; ++epoch) {
        RecordStream<> old_stream, stream;
        old.stream = &old_stream; old.compact_draws = false;
        fast.stream = &stream;
        // Execute the actual Context policy with unset, explicit-on and opt-out values.
        if (epoch % 3 == 0) unsetenv("NXBOX_D3D12_RECORD_STATE");
        else setenv("NXBOX_D3D12_RECORD_STATE", epoch % 3 == 1 ? "1" : "0", 1);
        __TRANSPORT_POLICY__
        assert(fast.reuse_draw_state == (epoch % 3 != 2));
        fast.compact_draws = epoch % 3 != 1;
        fast.ResetDrawState(); // Same producer hook used after command-list Reset.
        ID3D12GraphicsCommandList expected, actual;
        RecordingTarget old_target, target;
        old_target.list = &expected; old_target.list8 = &expected;
        target.list = &actual; target.list8 = &actual;
        size_t epoch_batches = 0;
        std::thread old_worker([&] { while (old_stream.Consume(&old_target)) {} });
        std::thread worker([&] { while (stream.Consume(&target)) ++epoch_batches; });
        D3D12_VIEWPORT viewports[16]{};
        D3D12_RECT scissors[16]{};
        D3D12_VERTEX_BUFFER_VIEW vertices[32]{};
        D3D12_INDEX_BUFFER_VIEW index{};
        FLOAT blend[4]{};
        UINT topology = 1, stencil = 0, start = 0, count = 2, viewport_count = 1;
        for (unsigned i = 0; i < 3000; ++i) {
            if (random() % 7 == 0) {
                // Raw float bits exercise NaNs, signed zero and infinities.
                for (auto& value : blend) { auto bits = uint32_t(random()); memcpy(&value, &bits, 4); }
                for (auto& view : viewports)
                    for (unsigned j = 0; j < sizeof(view); ++j)
                        reinterpret_cast<unsigned char*>(&view)[j] = random();
                for (auto& rect : scissors) rect = {int(random()), int(random()), int(random()), int(random())};
                for (auto& vertex : vertices) vertex = {random(), random(), random()};
                index = {random(), random(), int(random())};
                topology = random(); stencil = random();
                start = random() % 16; count = random() % (33-start);
                viewport_count = random() % 17;
            }
            auto record = [&](CommandFacade& f) {
                f.IASetPrimitiveTopology(topology);
                f.IASetVertexBuffers(start, count, vertices);
                f.IASetIndexBuffer(i % 89 == 0 ? nullptr : &index);
                f.RSSetViewports(viewport_count, viewports);
                f.RSSetScissorRects(viewport_count, scissors);
                f.OMSetBlendFactor(blend);
                if (i % 2) f.OMSetFrontAndBackStencilRef(stencil, stencil);
                else f.OMSetStencilRef(stencil);
                // Both draw kinds share the compact packet; parameters remain exact.
                f.DrawIndexedInstanced(i, i+1, i+2, -INT(i), i+3);
                f.DrawInstanced(i+7, i+8, i+9, i+10);
                if (i % 53 == 0) f.ExecuteIndirect(&signature, 1, &arguments, 0, nullptr, 0);
                if (i % 17 == 0) f.Dispatch(i, 2, 3); // No batching across other commands.
            };
            record(old);
            track_allocations = true;
            record(fast);
            track_allocations = false;
            if (i % 31 == 0) { old_stream.Flush(); stream.Flush(); }
        }
        old_stream.Close(); stream.Close(); old_worker.join(); worker.join();
        assert(observable(actual.calls) == observable(expected.calls));
        old_calls += expected.calls.size(); new_calls += actual.calls.size(); batches += epoch_batches;
        if (fast.compact_draws) assert(epoch_batches < actual.calls.size());
    }
    assert(producer_allocations == 0 && new_calls < old_calls);
    std::cout << "72000 randomized draws: old=" << old_calls << " compact=" << new_calls
              << " replay batches=" << batches << "; zero producer allocations\n";
    // A tiny ring forces repeated packets through every split/recycling position.
    struct Packet {
        uint32_t value;
        void Replay(void* p) const { auto& next = *static_cast<uint32_t*>(p); assert(value == next++); }
    };
    RecordStream<37, 3> tiny;
    uint32_t next = 0;
    std::thread consumer([&] { while (tiny.Consume(&next)) {} });
    for (uint32_t i = 0; i < 10000; ++i) {
        tiny.Repeat(Packet{i});
        if (i % 13 == 0) tiny.Flush();
    }
    tiny.Close(); consumer.join(); assert(next == 10000);
    // Abandoned packets must not execute; flush must close a batch immediately.
    RecordStream<> abandoned;
    abandoned.Repeat(Packet{999}); abandoned.Flush(); abandoned.Repeat(Packet{999});
    abandoned.Close();
    assert(abandoned.Consume(nullptr, false)); assert(abandoned.Consume(nullptr, false));
    assert(!abandoned.Consume(nullptr, false));
}
"""
    ).replace("__TRANSPORT_POLICY__", policy)


class DrawPacketTests(unittest.TestCase):
    def test_random_replay_address_undefined(self):
        recorder_tests.DeferredRecording.run_binary(self, "address,undefined", random_source)

    def test_random_replay_thread(self):
        recorder_tests.DeferredRecording.run_binary(self, "thread", random_source)

    def test_reset_hook_and_switches(self):
        context = (ROOT / "src/video_core/renderer_d3d12/d3d12_context.cpp").read_text()
        begin = context.split("CommandFacade* Context::Commands()", 1)[1].split(
            "void Context::MarkWork", 1
        )[0]
        self.assertIn("facade.ResetDrawState();", begin)
        self.assertIn('std::getenv("NXBOX_D3D12_RECORD_STATE")', context)
        self.assertIn('GetEnvironmentVariableA("NXBOX_D3D12_COMPACT_DRAWS"', context)


if __name__ == "__main__":
    unittest.main()
