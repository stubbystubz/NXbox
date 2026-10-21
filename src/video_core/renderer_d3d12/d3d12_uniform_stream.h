// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace D3D12 {
// Adapted from Jadyx's submission-local uniform stream. Allocation owns a resource
// lease as well as a mapped span; no contents are cached or read from upload memory.
template <typename Allocation>
class UniformStream {
public:
    static constexpr std::size_t BlockSize = 4096;
    void Clear() {
        block = {};
        used = 0;
    }
    template <typename Allocate, typename CurrentTick>
    Allocation Request(std::size_t size, Allocate&& allocate, CurrentTick&& current_tick) {
        if (!size || size > BlockSize)
            return allocate(size);
        const auto aligned = (size + 255) & ~std::size_t{255};
        if (tick != current_tick())
            Clear();
        if (block.mapped_span.empty() || aligned > block.mapped_span.size() - used) {
            Clear(); // Release our lease before the ring attempts pressure reclamation.
            block = allocate(BlockSize);
            // The allocator can submit/wait. Stamp the resulting tick, not its input tick.
            tick = current_tick();
        }
        Allocation result{block.buffer, block.offset + used, block.mapped_span.subspan(used, size)};
        used += aligned;
        return result;
    }

private:
    Allocation block{};
    std::uint64_t tick{};
    std::size_t used{};
};
} // namespace D3D12
