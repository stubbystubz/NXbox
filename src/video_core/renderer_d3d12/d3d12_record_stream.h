// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace D3D12 {
// One producer and one consumer. Only chunk boundaries synchronize; neither a command
// nor its variable payload needs to fit in one chunk. Published bytes are immutable.
template <size_t ChunkSize = 256 * 1024, size_t ChunkCount = 32>
class RecordStream {
public:
    static_assert(ChunkCount >= 2 && ChunkSize > 0);
    static constexpr size_t chunk_size = ChunkSize, chunk_count = ChunkCount;
    using Replay = void (*)(void*, const std::byte*, bool);
    struct Header {
        Replay replay;
        size_t size;
    };
    // More room absorbs bursts of short submissions while the native recorder catches up.
    // The bound still applies to payloads and COM holds; a slow consumer backpressures safely.
    explicit RecordStream(size_t capacity = ChunkCount, bool direct = true)
        : active_chunks{capacity}, direct_replay{direct} {
        if (capacity < 2 || capacity > ChunkCount)
            throw std::invalid_argument("invalid D3D12 recording capacity");
        chunks = std::make_unique<Chunk[]>(capacity);
    }
    RecordStream(const RecordStream&) = delete;
    // Producer only. A cut publishes the old segment before routing any bytes of
    // the next packet. The original consumer continues reading only this queue.
    // Restore nullptr before encoding the ordered submission on the first queue.
    void RouteWritesTo(RecordStream* next) {
        if (next == this || (next && next->destination))
            throw std::invalid_argument("invalid D3D12 recording route");
        Flush();
        destination = next;
    }
    void Write(const void* source, size_t size) {
        if (destination) {
            destination->Write(source, size);
            return;
        }
        repeat_replay = nullptr;
        auto* bytes = static_cast<const std::byte*>(source);
        while (size) {
            auto& chunk = chunks[write_index];
            if (!write_offset) {
                auto used = chunk.used.load(std::memory_order_acquire);
                if (used) {
                    BoundaryWait([&] {
                        do {
                            chunk.used.wait(used, std::memory_order_acquire);
                            used = chunk.used.load(std::memory_order_acquire);
                        } while (used);
                    });
                }
            }
            const auto count = std::min(size, ChunkSize - write_offset);
            std::memcpy(chunk.data.data() + write_offset, bytes, count);
            write_offset += count;
            bytes += count;
            size -= count;
            if (write_offset == ChunkSize)
                Flush();
        }
    }
    void Flush() {
        if (destination) {
            destination->Flush();
            return;
        }
        repeat_replay = nullptr;
        if (!write_offset)
            return;
        chunks[write_index].used.store(write_offset, std::memory_order_release);
        write_offset = 0;
        if (++write_index == active_chunks)
            write_index = 0;
        available.fetch_add(1, std::memory_order_release);
        available.notify_one();
    }
    // Called only after the producer has finished writing. Wakes the consumer at EOF.
    void Close() {
        Flush();
        closed.store(true, std::memory_order_release);
        available.fetch_add(1, std::memory_order_release);
        available.notify_one();
    }
    bool Consume(void* target, bool execute = true) {
        if (direct_replay && !pending_header) {
            if (!Readable())
                return false;
            auto& chunk = chunks[read_index];
            const auto remaining = read_limit - read_offset;
            if (remaining >= sizeof(Header)) {
                Header header;
                const auto* packet = chunk.data.data() + read_offset;
                std::memcpy(&header, packet, sizeof(header));
                const auto* payload = packet + sizeof(header);
                // Snapshot arrays require 16-byte alignment. Chunk splits or raw
                // writes can misalign later packets; those use the scratch path.
                if (header.size <= remaining - sizeof(Header) &&
                    reinterpret_cast<std::uintptr_t>(payload) % 16 == 0) {
                    // Pin the published chunk until Replay AND Release finish, including
                    // throwing replay. Returning it first races producer reuse of arrays.
                    try {
                        header.replay(target, payload, execute);
                    } catch (...) {
                        Advance(sizeof(Header) + header.size);
                        throw;
                    }
                    Advance(sizeof(Header) + header.size);
                    return true;
                }
            }
        }
        if (!pending_header) {
            Header header{};
            if (!Read(&header, sizeof(header)))
                return false;
            pending_header = header;
        }
        // A failed scratch allocation must leave the consumed header available to the
        // failure drain. Otherwise the next Consume interprets payload bytes as a thunk.
        storage.resize((pending_header->size + sizeof(std::max_align_t) - 1) /
                       sizeof(std::max_align_t));
        if (!Read(storage.data(), pending_header->size))
            throw std::runtime_error("truncated D3D12 recording");
        const auto header = *pending_header;
        pending_header.reset(); // Replay releases holds even on exceptions; never retry it.
        header.replay(target, reinterpret_cast<const std::byte*>(storage.data()), execute);
        return true;
    }
    // Only the unpublished tail can grow. Any ordinary write or publication ends
    // the batch, so draws never cross state, query, barrier or submission commands.
    template <class Command>
    void Repeat(const Command& command) {
        if (destination) {
            destination->Repeat(command);
            return;
        }
        if (direct_replay)
            RepeatPacket<Command, 16>(command);
        else
            RepeatPacket<Command, 1>(command);
    }
    template <class Command>
    void Begin(size_t extra = 0) {
        const Header header{[](void* target, const std::byte* data, bool execute) {
                                Command command;
                                std::memcpy(&command, data, sizeof(command));
                                // Even failed/abandoned recordings release all encoded COM holds.
                                try {
                                    if (execute)
                                        command.Replay(target, data);
                                } catch (...) {
                                    command.Release(data);
                                    throw;
                                }
                                command.Release(data);
                            },
                            sizeof(Command) + extra};
        Write(&header, sizeof(header));
    }
    template <class Wait>
    void BoundaryWait(Wait wait) {
        const auto start = metrics ? Clock() : 0;
        wait();
        if (metrics) {
            ++waits;
            wait_ns += Clock() - start;
        }
    }
    bool metrics{};
    uint64_t waits{}, wait_ns{}; // Producer only; sampled by the producer's frame profile.
private:
    // Keep compact packets and the packets following them aligned for direct
    // replay. The legacy format remains available with DIRECT_REPLAY=0.
    template <class Command, size_t Alignment>
    void RepeatPacket(const Command& command) {
        static_assert(std::is_trivially_copyable_v<Command>);
        constexpr size_t prefix = (sizeof(uint32_t) + Alignment - 1) & ~(Alignment - 1);
        constexpr size_t stride = (sizeof(Command) + Alignment - 1) & ~(Alignment - 1);
        const Replay replay = [](void* target, const std::byte* data, bool execute) {
            if (!execute)
                return; // Repeat packets contain values only, never COM holds.
            uint32_t count;
            std::memcpy(&count, data, sizeof(count));
            data += prefix;
            for (uint32_t i = 0; i < count; ++i) {
                Command value;
                std::memcpy(&value, data, sizeof(value));
                value.Replay(target);
                data += stride;
            }
        };
        const std::byte padding[16]{};
        if (repeat_replay == replay && ChunkSize - write_offset > stride) {
            auto* const base = chunks[write_index].data.data();
            Header header;
            uint32_t count;
            std::memcpy(&header, base + repeat_offset, sizeof(header));
            std::memcpy(&count, base + repeat_offset + sizeof(header), sizeof(count));
            header.size += stride;
            ++count;
            std::memcpy(base + repeat_offset, &header, sizeof(header));
            std::memcpy(base + repeat_offset + sizeof(header), &count, sizeof(count));
            std::memcpy(base + write_offset, &command, sizeof(command));
            std::memcpy(base + write_offset + sizeof(command), padding, stride - sizeof(command));
            write_offset += stride;
            return;
        }
        const auto start = write_offset;
        const Header header{replay, prefix + stride};
        const uint32_t count = 1;
        // Write handles tiny chunks and backpressure, including split packets.
        Write(&header, sizeof(header));
        Write(&count, sizeof(count));
        Write(padding, prefix - sizeof(count));
        Write(&command, sizeof(command));
        Write(padding, stride - sizeof(command));
        if (sizeof(header) + prefix + stride < ChunkSize - start) {
            repeat_offset = start;
            repeat_replay = replay;
        }
    }
    static uint64_t Clock() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }
    // A published chunk is immutable until Advance retires it. Cache its byte
    // limit once, rather than acquiring the same atomic for every header/payload.
    bool Readable() {
        if (direct_replay && read_limit)
            return true;
        auto& chunk = chunks[read_index];
        auto used = chunk.used.load(std::memory_order_acquire);
        while (!used) {
            const auto generation = available.load(std::memory_order_acquire);
            used = chunk.used.load(std::memory_order_acquire);
            if (used)
                break;
            if (closed.load(std::memory_order_acquire)) {
                used = chunk.used.load(std::memory_order_acquire);
                if (!used)
                    return false;
                break;
            }
            available.wait(generation, std::memory_order_acquire);
        }
        read_limit = used;
        return true;
    }
    void Advance(size_t size) {
        read_offset += size;
        if (read_offset == read_limit) {
            auto& chunk = chunks[read_index];
            read_offset = 0;
            read_limit = 0;
            if (++read_index == active_chunks)
                read_index = 0;
            chunk.used.store(0, std::memory_order_release);
            chunk.used.notify_one();
        }
    }
    bool Read(void* destination, size_t size) {
        auto* bytes = static_cast<std::byte*>(destination);
        while (size) {
            if (!Readable())
                return false;
            const auto count = std::min(size, read_limit - read_offset);
            std::memcpy(bytes, chunks[read_index].data.data() + read_offset, count);
            bytes += count;
            size -= count;
            Advance(count);
        }
        return true;
    }
    struct Chunk {
        std::atomic<size_t> used{};
        alignas(16) std::array<std::byte, ChunkSize> data;
    };
    RecordStream* destination{}; // Producer only; destination outlives the route.
    std::unique_ptr<Chunk[]> chunks;
    const size_t active_chunks;
    size_t write_index{}, write_offset{}, read_index{}, read_offset{}, read_limit{};
    bool direct_replay{};   // Latched before the recorder starts.
    Replay repeat_replay{}; // Producer only; points only into the unpublished tail.
    size_t repeat_offset{};
    std::atomic<uint64_t> available{};
    std::atomic<bool> closed{};
    std::optional<Header> pending_header;  // Consumer only; survives scratch allocation failure.
    std::vector<std::max_align_t> storage; // Consumer only; no producer allocation.
};
} // namespace D3D12
