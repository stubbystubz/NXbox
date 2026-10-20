// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
// Included after the extracted production methods and hardware stubs.
struct Profile : Common::GpuProfile::Sink {
    std::size_t phase = 99;
    std::uint64_t transitions{};
    std::size_t Enter(Common::GpuProfile::Phase next) override {
        const auto target = static_cast<std::size_t>(next);
        if (target == phase)
            return 100;
        const auto old = phase;
        phase = target;
        transitions += Common::GpuProfile::Now();
        return old;
    }
    void Leave(std::size_t previous) override {
        if (previous == 100)
            return;
        phase = previous;
        transitions += Common::GpuProfile::Now();
    }
};
int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 2000;
    const bool profiling = argc > 2 && argv[2][0] == '1';
    DmaPusher pusher;
    Profile profile;
    Common::GpuProfile::sink = &profile;
    Common::GpuProfile::recording_enabled = profiling;
    auto& engine = pusher.engine;
    engine.regs = {};
    engine.shadow_state = {};
    engine.dirty.flags.reset();
    std::vector<CommandHeader> stream;
    unsigned methods = 0;
    auto packet = [&](SubmissionMode mode, u32 first, std::initializer_list<u32> words) {
        CommandHeader h{};
        h.mode = mode;
        h.method = first;
        h.method_count = words.size();
        stream.push_back(h);
        for (auto word : words) {
            CommandHeader v{};
            v.argument = word;
            stream.push_back(v);
        }
        methods += words.size();
    };
    for (unsigned draw = 0; draw < 1000; ++draw) {
        // State changes, redundant state, repeated non-incrementing writes, and
        // traditional begin/count/end draws. No renderer or guest memory work.
        packet(SubmissionMode::Increasing, 256, {draw, 2, 3, 4, 5, 6, 7, 8});
        packet(SubmissionMode::Increasing, 288, {1, 2, 3, 4});
        packet(SubmissionMode::NonIncreasing, 320, {draw, draw + 1, draw + 2, draw + 3});
        packet(SubmissionMode::Increasing, MAXWELL3D_REG_INDEX(vertex_buffer.first), {draw * 3, 3});
        packet(SubmissionMode::Increasing, MAXWELL3D_REG_INDEX(draw.begin), {4});
        packet(SubmissionMode::Increasing, MAXWELL3D_REG_INDEX(draw.end), {0});
    }
    for (unsigned frame = 0; frame < 50; ++frame)
        pusher.ProcessCommands(stream);
    engine.rasterizer->draws = 0;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned frame = 0; frame < frames; ++frame)
        pusher.ProcessCommands(stream);
    const double ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    assert(engine.rasterizer->draws == std::uint64_t(frames) * 1000);
    assert(assertion_failures == 0 && pusher.dma_state.method_count == 0);
    std::printf("profile=%u methods/frame=%u draws/frame=1000 ns/method=%.3f "
                "ns/draw=%.3f checksum=%llu\n",
                profiling, methods, ns / (frames * double(methods)), ns / (frames * 1000.0),
                static_cast<unsigned long long>(engine.rasterizer->checksum));
}
