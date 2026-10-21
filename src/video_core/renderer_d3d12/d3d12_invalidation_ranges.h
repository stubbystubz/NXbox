// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace D3D12 {

// Merge only consecutive overlapping/adjacent intervals. No sort, allocation or
// expansion across unwritten gaps. Query report publication precedes this pass.
template <typename F>
void ForEachInvalidationRange(std::span<const std::pair<std::uint64_t, std::size_t>> ranges,
                              bool coalesce, F&& emit) {
    std::uint64_t begin{}, end{};
    bool pending = false;
    const auto flush = [&] {
        if (pending) {
            emit(begin, end - begin);
            pending = false;
        }
    };
    for (const auto& [address, size] : ranges) {
        if (!coalesce || size == 0 || size > std::numeric_limits<std::uint64_t>::max() - address) {
            flush();
            emit(address, size);
            continue;
        }
        const auto next_end = address + size;
        if (pending && address <= end && begin <= next_end) {
            begin = (std::min)(begin, address);
            end = (std::max)(end, next_end);
        } else {
            flush();
            begin = address;
            end = next_end;
            pending = true;
        }
    }
    flush();
}

} // namespace D3D12
