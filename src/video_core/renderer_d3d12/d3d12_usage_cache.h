// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace D3D12 {

// Usage bits only accumulate until ResetUsageTracking. Remember exact operations,
// not containing byte ranges: UsageTracker rounds its endpoints to usage bits.
class UsageCache {
public:
    bool Insert(std::uint64_t offset, std::uint64_t size) {
        for (std::size_t i = 0; i < count; ++i) {
            if (ranges[i].offset == offset && ranges[i].size == size) {
                return false;
            }
        }
        ranges[next] = {offset, size};
        next = (next + 1) % ranges.size();
        if (count < ranges.size()) {
            ++count;
        }
        return true;
    }

    void Clear() {
        count = next = 0;
    }

private:
    struct Range {
        std::uint64_t offset{}, size{};
    };
    std::array<Range, 8> ranges{};
    std::size_t count{}, next{};
};

} // namespace D3D12
