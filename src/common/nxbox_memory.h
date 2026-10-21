// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>

// Cross-thread allocation gauges. No allocation, locks or formatting on the hot path.
// Counters describe owned capacity, not resident pages or opaque driver allocations.
namespace NxboxMemory {
enum class Kind {
    GpuBuffer,
    GpuTexture,
    TextureRgba8,
    TextureBc,
    TextureDepth,
    TextureOther,
    GpuUpload,
    GpuReadback,
    DecodeInput,
    DecodeOutput,
    DecodeReserved,
    TextureScratch,
    BufferScratch,
    ShaderIr,
    ShaderSource,
    ShaderDxil,
    PipelineObjects,
    TextureExpected,
    TextureCritical,
    DecodeBudget,
    TextureEvictions,
    Count,
};
inline std::array<std::atomic<std::uint64_t>, static_cast<size_t>(Kind::Count)> bytes{};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXBOX_MEMORY_BREAKDOWN");
        return !value || std::string_view{value} != "0";
    }();
    return enabled;
}
inline std::uint64_t Read(Kind kind) {
    return bytes[static_cast<size_t>(kind)].load(std::memory_order_relaxed);
}
class Gauge {
public:
    explicit Gauge(Kind kind_) : kind{kind_} {}
    Gauge(Kind kind_, std::uint64_t value) : Gauge{kind_} {
        Set(value);
    }
    Gauge(const Gauge&) = delete;
    Gauge& operator=(const Gauge&) = delete;
    Gauge(Gauge&& other) noexcept : kind{other.kind}, owned{std::exchange(other.owned, 0)} {}
    Gauge& operator=(Gauge&& other) noexcept {
        Set(0);
        kind = other.kind;
        owned = std::exchange(other.owned, 0);
        return *this;
    }
    ~Gauge() {
        Set(0);
    }
    void Set(std::uint64_t value) {
        if (!Enabled() || value == owned)
            return;
        auto& counter = bytes[static_cast<size_t>(kind)];
        if (value >= owned)
            counter.fetch_add(value - owned, std::memory_order_relaxed);
        else
            counter.fetch_sub(owned - value, std::memory_order_relaxed);
        owned = value;
    }
    void Add(std::uint64_t value) {
        Set(owned + value);
    }

private:
    Kind kind;
    std::uint64_t owned{};
};
} // namespace NxboxMemory
