// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
// Included after the extracted production methods and hardware stubs.
static Maxwell3D* trace_engine;
static std::uint64_t trace = 14695981039346656037ull;
void TraceState() {
    const auto mix = [](u32 value) { trace = (trace ^ value) * 1099511628211ull; };
    for (auto value : trace_engine->regs.reg_array)
        mix(value);
    for (auto value : trace_engine->shadow_state.reg_array)
        mix(value);
    for (size_t i = 0; i < trace_engine->dirty.flags.size(); ++i)
        mix(trace_engine->dirty.flags[i]);
}
void Rasterizer::Draw(bool indexed, u32 instances) {
    ++draws;
    checksum += indexed + instances;
    if (trace_engine) {
        TraceState();
        trace = (trace ^ indexed) * 1099511628211ull;
        trace = (trace ^ instances) * 1099511628211ull;
    }
}
struct Profile : Common::GpuProfile::Sink, Common::MaxwellMethodProfile::Collector {
    static constexpr size_t idle = 99, nested = 100;
    std::size_t phase = idle;
    Common::GpuProfile::Tick last{};
    std::array<Common::GpuProfile::Tick, 16> elapsed{};
    void Switch(size_t next) {
        const auto now = Common::GpuProfile::Now();
        if (phase < elapsed.size())
            elapsed[phase] += now - last;
        phase = next;
        last = now;
    }
    std::size_t Enter(Common::GpuProfile::Phase next) override {
        const auto target = static_cast<std::size_t>(next);
        if (target == phase)
            return nested;
        const auto old = phase;
        Switch(target);
        return old;
    }
    void Leave(std::size_t previous) override {
        if (previous != nested)
            Switch(previous);
    }
    Common::GpuProfile::Tick ReadTicks() override {
        const auto maxwell = static_cast<size_t>(Common::GpuProfile::Phase::Maxwell3DMethods);
        return elapsed[maxwell] + (phase == maxwell ? Common::GpuProfile::Now() - last : 0);
    }
};
int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 2000;
    const bool profiling = argc > 2 && argv[2][0] == '1';
    constexpr unsigned draws_per_frame = 3000;
    DmaPusher pusher;
    Profile profile;
    Common::GpuProfile::sink = &profile;
    Common::GpuProfile::recording_enabled = profiling;
    if (argc > 3 && std::string_view(argv[3]) == "breakdown")
        Common::MaxwellMethodProfile::collector = &profile;
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
    for (unsigned draw = 0; draw < draws_per_frame; ++draw) {
        // State changes, redundant state, repeated non-incrementing writes, and
        // traditional begin/count/end draws. No renderer or guest memory work.
        packet(SubmissionMode::Increasing, 256, {draw, 2, 3, 4, 5, 6, 7, 8});
        packet(SubmissionMode::Increasing, 288, {1, 2, 3, 4});
        packet(SubmissionMode::NonIncreasing, 320, {draw, draw + 1, draw + 2, draw + 3});
        packet(SubmissionMode::Increasing, MAXWELL3D_REG_INDEX(vertex_buffer.first), {draw * 3, 3});
        packet(SubmissionMode::Increasing, MAXWELL3D_REG_INDEX(draw.begin), {4});
        packet(SubmissionMode::Increasing, MAXWELL3D_REG_INDEX(draw.end), {0});
    }
    const auto replay = [&] {
        const Common::GpuProfile::RecordingScope scope{Common::GpuProfile::Phase::DmaPusher};
        pusher.ProcessCommands(stream);
    };
    for (unsigned frame = 0; frame < 50; ++frame)
        replay();
    if (argc > 3 && std::string_view(argv[3]) == "verify")
        trace_engine = &engine;
    profile.elapsed.fill(0);
    profile.samples.fill({});
    engine.rasterizer->draws = 0;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned frame = 0; frame < frames; ++frame)
        replay();
    const double ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    assert(engine.rasterizer->draws == std::uint64_t(frames) * draws_per_frame);
    assert(assertion_failures == 0 && pusher.dma_state.method_count == 0);
    if (trace_engine) {
        TraceState();
        std::printf("trace=%llu\n", static_cast<unsigned long long>(trace));
        return 0;
    }
    std::printf("profile=%u methods/frame=%u draws/frame=%u ns/method=%.3f "
                "ns/draw=%.3f checksum=%llu\n",
                profiling, methods, draws_per_frame, ns / (frames * double(methods)),
                ns / (frames * double(draws_per_frame)),
                static_cast<unsigned long long>(engine.rasterizer->checksum));
    if (profiling) {
        const double draws = frames * double(draws_per_frame);
        std::printf("exclusive dma=%.3f maxwell=%.3f ns/draw\n", profile.elapsed[0] / draws,
                    profile.elapsed[1] / draws);
        for (size_t i = 0; i < profile.samples.size(); ++i) {
            const auto& sample = profile.samples[i];
            if (sample.count)
                std::printf("method_class=%zu ns/draw=%.3f units/draw=%.3f\n", i,
                            sample.ticks / draws, sample.count / draws);
        }
    }
}
