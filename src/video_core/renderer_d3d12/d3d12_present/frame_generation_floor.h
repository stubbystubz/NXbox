// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <deque>

namespace D3D12::Present {
// Wall timestamps of real guest presents only. Do not subtract presentation waits:
// that would estimate producer capacity rather than delivered native frame rate.
class NativeFrameGenerationGate {
public:
    void ObservePresent(double timestamp, bool real = true) {
        if (!real)
            return;
        if (!std::isfinite(timestamp) || (have_previous && timestamp <= previous)) {
            Reset();
            return;
        }
        if (have_previous && timestamp - previous > 1.0)
            Reset(); // Loading/suspension must requalify from a fresh window.
        previous = timestamp;
        have_previous = true;
        presents.push_back(timestamp);
        // Retain the sample just before the one-second boundary. Each interval
        // has two real endpoints; generated presents can never enter this deque.
        while (presents.size() > 2 && timestamp - presents[1] >= 1.0)
            presents.pop_front();
        const double duration = timestamp - presents.front();
        native_fps = duration > 0 ? static_cast<double>(presents.size() - 1) / duration : 0;
        if (duration < 1.0 - Epsilon || native_fps < 28.0 - Epsilon) {
            qualified = false;
            recovering = false;
        } else if (!qualified) {
            if (native_fps < 29.0 - Epsilon) {
                recovering = false;
            } else if (!recovering) {
                recovering = true;
                recovery_since = timestamp;
            } else if (timestamp - recovery_since >= 1.0 - Epsilon) {
                qualified = true;
                recovering = false;
            }
        }
    }
    double NativeFps() const {
        return native_fps;
    }
    unsigned Multiplier(double refresh) const {
        // Permission for at most one synthetic frame, not an unconditional 2x rate.
        // The vblank schedule gives ready real frames priority and caps output at 60.
        return qualified && native_fps < 50.0 - Epsilon && Interval(refresh) ? 2u : 0u;
    }
    unsigned Interval(double refresh) const {
        // Use the lower tolerance edge for 59.94/119.88, but never exceed 60 Hz.
        return refresh >= 119 && refresh <= 120 ? 2u : refresh >= 59 && refresh <= 60 ? 1u : 0u;
    }

private:
    void Reset() {
        presents.clear();
        native_fps = 0;
        have_previous = qualified = recovering = false;
    }
    static constexpr double Epsilon = 1e-8;
    std::deque<double> presents;
    double previous{}, native_fps{}, recovery_since{};
    bool have_previous{}, qualified{}, recovering{};
};
// Reserve positive-interval DXGI presents, one output slot ahead of scanout.
// Preparing/submitting at the scanout deadline is too late: guest work may already
// occupy the GPU. DXGI owns the vblank wait; do not add a second CPU period.
// Synthetic submission has one slot of lookahead and no catch-up credit.
class FrameGenerationVblankSchedule {
public:
    void RealReady() {
        generated_available = false;
    }
    void Presented(double timestamp, double output_hz, bool real, bool candidate = false) {
        period = 1.0 / output_hz;
        next_slot = std::fmax(timestamp, next_slot) + period;
        generated_available = real && candidate;
    }
    bool Generate(double timestamp, bool real_ready) const {
        return generated_available && !real_ready && timestamp >= Deadline();
    }
    double Deadline() const {
        return generated_available ? next_slot - period : 0;
    }

private:
    double next_slot{}, period{};
    bool generated_available{};
};
} // namespace D3D12::Present
