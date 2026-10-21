// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Kernel::NxboxThreadProfile {
inline bool Enabled() noexcept {
    // LocalState overrides are loaded after early process initialization.
    const char* value = std::getenv("NXBOX_THREAD_PROFILE");
    return value && std::strcmp(value, "1") == 0;
}

// Owned by a guest KThread, not a host thread: a blocking SVC can migrate between
// cores. Updated only by that guest thread. No shared locks or per-call logging.
class Profile {
public:
    using Clock = std::chrono::steady_clock;
    struct Counter {
        std::uint64_t calls{}, total_us{}, max_us{};
        void Add(std::uint64_t us) {
            ++calls;
            total_us += us;
            max_us = std::max(max_us, us);
        }
    };
    struct SyncCounter {
        std::uintptr_t object{};
        Counter time{};
    };

    explicit Profile(Clock::time_point now = Clock::now()) : start{now} {}

    static std::uint64_t Elapsed(Clock::time_point begin, Clock::time_point end) {
        return end > begin
                   ? std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()
                   : 0;
    }
    void Run(Clock::time_point begin, Clock::time_point end) {
        run.Add(Elapsed(begin, end));
    }
    void Svc(std::uint32_t number, Clock::time_point begin, Clock::time_point end) {
        // The last bucket explicitly represents out-of-range SVC numbers.
        svc[std::min<std::size_t>(number, svc.size() - 1)].Add(Elapsed(begin, end));
    }
    void Sync(std::uintptr_t object, Clock::time_point begin, Clock::time_point end) {
        const auto us = Elapsed(begin, end);
        // Object zero represents timeout/cancellation/no selected object. Keep
        // a bounded set of selected objects; overflow is reported separately.
        for (auto& entry : sync) {
            if (!entry.time.calls || entry.object == object) {
                entry.object = object;
                entry.time.Add(us);
                return;
            }
        }
        sync_overflow.Add(us);
    }

    template <typename Report>
    void Poll(Report&& report, Clock::time_point now) noexcept {
        if (now - start < std::chrono::seconds(5)) {
            return;
        }
        const auto interval =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        try {
            // SVC wall time includes blocked AND runnable-but-unscheduled time.
            // Sync is a subset of SVC, never additive to it. Run includes runtime
            // compilation/callbacks, but excludes SVC dispatch and scheduler idle.
            report("run", std::uintptr_t{0}, run, interval);
            for (std::size_t i = 0; i < svc.size(); ++i) {
                if (svc[i].calls) {
                    report("svc", i, svc[i], interval);
                }
            }
            for (const auto& entry : sync) {
                if (entry.time.calls) {
                    report("sync_object", entry.object, entry.time, interval);
                }
            }
            if (sync_overflow.calls) {
                report("sync_overflow", std::uintptr_t{0}, sync_overflow, interval);
            }
        } catch (...) {
            // Diagnostics must not interrupt guest execution.
        }
        run = {};
        svc = {};
        sync = {};
        sync_overflow = {};
        start = now;
    }

private:
    Clock::time_point start;
    Counter run{}, sync_overflow{};
    std::array<Counter, 129> svc{};
    std::array<SyncCounter, 16> sync{};
};
} // namespace Kernel::NxboxThreadProfile
