// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace EdenXbox::Recording {
inline constexpr unsigned Slots = 3;
inline constexpr unsigned MaxSeconds = 120;
inline constexpr std::uintmax_t ArchiveBytes = 512ull * 1024 * 1024;
inline constexpr std::uintmax_t ClipReserve = 192ull * 1024 * 1024;

inline unsigned Seconds(const char* text, unsigned fallback = 0) {
    if (!text || !*text)
        return fallback;
    unsigned value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9')
            return fallback;
        value = std::min(3600u, value * 10 + unsigned(*p - '0'));
    }
    return std::min(value, MaxSeconds);
}
inline std::string FileName(std::string title, const std::tm& time) {
    if (title.size() != 16 ||
        title.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        title = "0000000000000000";
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &time);
    return title + "-" + stamp + ".mp4";
}
// Producer claims a free slot; the consumer releases it only after the GPU and encoder finish.
// No waiting or overwrite of an in-flight slot, including during stop/finalize.
struct Ring {
    std::array<std::atomic<bool>, Slots> busy{};
    unsigned next{};
    std::atomic<std::uint64_t> drops{};
    int Acquire() {
        for (unsigned n = 0; n < Slots; ++n) {
            const auto i = (next + n) % Slots;
            if (!busy[i].exchange(true, std::memory_order_acq_rel)) {
                next = (i + 1) % Slots;
                return static_cast<int>(i);
            }
        }
        ++drops;
        return -1;
    }
    void Release(unsigned i) {
        busy[i].store(false, std::memory_order_release);
    }
};
struct Timeline {
    double first{-1}, start{-1}, next{};
    unsigned duration{MaxSeconds}, delay{}, fps{30};
    bool automatic{}, fired{};
    bool AutoStart(double now) {
        if (first < 0)
            first = now;
        if (!automatic || fired || now - first < delay)
            return false;
        fired = true;
        return true;
    }
    void Start(double now) {
        start = now;
        next = now;
    }
    bool Expired(double now) const {
        return start >= 0 && now - start >= duration;
    }
    bool Due(double now) {
        if (now + 0.000001 < next)
            return false;
        next += (std::floor(std::max(0.0, now - next) * fps + 0.000001) + 1) / fps;
        // Preserve cadence across jitter; never accumulate a catch-up burst after a stall.
        return true;
    }
    std::int64_t Timestamp(double now) const {
        return static_cast<std::int64_t>(std::max(0.0, now - start) * 10000000.0);
    }
};
struct Options {
    unsigned mode{}; // 0: manual 720p30, 1..4: auto 720p30/60, 1080p30/60.
    unsigned Width() const {
        return mode >= 3 ? 1920 : 1280;
    }
    unsigned Height() const {
        return mode >= 3 ? 1080 : 720;
    }
    unsigned Fps() const {
        return mode == 2 || mode == 4 ? 60 : 30;
    }
};
inline Options ReadOptions(const std::filesystem::path& local) {
    Options options;
    std::ifstream in(local / "recording.txt");
    unsigned mode{};
    if (in >> mode && mode <= 4)
        options.mode = mode;
    return options;
}
inline bool SaveOptions(const std::filesystem::path& local, Options options) {
    std::ofstream out(local / "recording.txt", std::ios::trunc);
    out << options.mode << '\n';
    out.close();
    return !out.fail();
}
// Configured before renderer construction; only the toggle/indicator cross threads afterwards.
inline std::filesystem::path local_state;
inline std::string title_id;
inline Options options;
inline std::atomic<unsigned> toggles{};
inline std::atomic<bool> active{}, pending{}, stop_requested{};
inline void Configure(const std::filesystem::path& local, std::string title) {
    local_state = local;
    title_id = std::move(title);
    options = ReadOptions(local);
    toggles.store(0);
    active.store(false);
    pending.store(false);
    stop_requested.store(false);
}
struct Shortcut {
    bool stick_held{}, consume_stick{};
    bool Update(bool view, bool stick, bool blocked) {
        if (!stick)
            consume_stick = false;
        const bool edge = view && stick && !stick_held && !blocked;
        stick_held = stick;
        if (edge)
            consume_stick = true;
        return edge;
    }
};
} // namespace EdenXbox::Recording
