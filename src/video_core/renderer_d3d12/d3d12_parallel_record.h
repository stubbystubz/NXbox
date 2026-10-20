// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <compare>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "video_core/renderer_d3d12/d3d12_record_args.h"

namespace D3D12 {
// Encoder integration contract:
// * Remember each native state change, including changes outside graphics draws.
// * Split partial updates into keys (one IA slot / root constant DWORD per key).
// * Forget root arguments when their signature changes and tables when heaps change.
// * Copy RTV/DSV descriptors into independently leased slots before Remember. An
//   ordinary DescriptorTicket is single-owner and cannot also belong to a snapshot.
// * Begin/End every query and opaque scope; disable cuts after ExecuteIndirect or
//   an unmodelled state mutation. Reset only at a native command-list Reset.
// The transport cannot infer any of these semantics from opaque replay thunks.
enum class ParallelState : std::uint8_t {
    DescriptorHeaps,
    GraphicsRootSignature,
    ComputeRootSignature,
    Pipeline,
    GraphicsRootArgument,
    ComputeRootArgument,
    GraphicsRootTable,
    ComputeRootTable,
    RenderTargets,
    Viewports,
    Scissors,
    VertexBuffer,
    IndexBuffer,
    StencilReference,
    BlendFactor,
    PrimitiveTopology,
    DepthBounds,
    StreamOutput,
    Predication,
};
struct ParallelStateKey {
    ParallelState state;
    std::uint32_t slot{};
    std::uint32_t word{};
    auto operator<=>(const ParallelStateKey&) const = default;
};

// Captured state owns its variable payloads and COM holds. Unlike a normal stream
// packet, replay does not retire the holds: multiple snapshots may still use them.
class ParallelRecordSnapshot {
    struct Packet {
        std::vector<std::max_align_t> storage;
        void (*replay)(void*, const std::byte*){};
        void (*release)(const std::byte*){};
        size_t offset{};
        bool armed{};
        ~Packet() {
            if (armed)
                release(Data());
        }
        std::byte* Data() {
            return reinterpret_cast<std::byte*>(storage.data());
        }
        void Write(const void* source, size_t count) {
            if (count)
                std::memcpy(Data() + offset, source, count);
            offset += count;
        }
    };
    using Entries = std::vector<std::shared_ptr<Packet>>;
    struct SnapshotCommand {
        Entries* entries;
        void Replay(void* target, const std::byte*) const {
            for (const auto& packet : *entries)
                packet->replay(target, packet->Data());
        }
        void Release(const std::byte*) const {
            delete entries;
        }
    };

public:
    template <auto Invoke, class... Args>
    void Remember(ParallelStateKey key, Args... args) {
        size_t offset{};
        using Pack = decltype(MakeRecordPack(offset, args...));
        using Call = RecordedCall<Invoke, Pack, sizeof...(Args)>;
        static_assert(std::is_trivially_copyable_v<Call>);
        offset = (sizeof(Call) + 15) & ~size_t{15};
        const auto first = offset;
        Call call{MakeRecordPack(offset, args...)};
        auto packet = std::make_shared<Packet>();
        packet->storage.resize((offset + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        packet->replay = [](void* target, const std::byte* data) {
            Call value;
            std::memcpy(&value, data, sizeof(value));
            value.Replay(target, data);
        };
        packet->release = [](const std::byte* data) {
            Call value;
            std::memcpy(&value, data, sizeof(value));
            value.Release(data);
        };
        packet->Write(&call, sizeof(call));
        const std::byte padding[16]{};
        packet->Write(padding, first - sizeof(call));
        (WriteArgument(*packet, args), ...);
        // Allocate the map node before acquiring ownership. Failure leaves the old
        // state intact, with neither leaked COM holds nor a partially installed packet.
        auto entry = entries.try_emplace(key).first;
        (HoldArgument(args), ...);
        packet->armed = true;
        entry->second = std::move(packet);
    }
    void Forget(ParallelState state) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->first.state == state)
                it = entries.erase(it);
            else
                ++it;
        }
    }
    void Reset() {
        entries.clear();
    }
    template <class Stream>
    void Encode(Stream& stream) const {
        auto captured = std::make_unique<Entries>();
        captured->reserve(entries.size());
        for (const auto& [key, packet] : entries)
            captured->push_back(packet);
        const SnapshotCommand command{captured.get()};
        stream.template Begin<SnapshotCommand>();
        stream.Write(&command, sizeof(command));
        captured.release();
    }

private:
    std::map<ParallelStateKey, std::shared_ptr<Packet>> entries;
};

// At most one cut per submission: two lists, two allocators, one queue execute.
// A pending cut is deferred across query/event/split-barrier scopes. Unsupported
// mutations fail closed for the entire submission, rather than guessing state.
class ParallelRecordCuts {
public:
    explicit ParallelRecordCuts(size_t minimum_draws = 1024) : threshold{minimum_draws} {}
    void Reset(bool enabled) {
        queries.clear();
        draws = scopes = 0;
        allowed = enabled;
        cut = false;
    }
    void Draw() {
        ++draws;
    }
    void BeginQuery(std::uintptr_t heap, std::uint32_t type, std::uint32_t index) {
        if (!queries.emplace(Query{heap, type, index}, true).second)
            Disable();
    }
    void EndQuery(std::uintptr_t heap, std::uint32_t type, std::uint32_t index) {
        if (!queries.erase(Query{heap, type, index}))
            Disable();
    }
    // Timestamp EndQuery has no matching BeginQuery and does not open a scope.
    void BeginScope() {
        ++scopes;
    }
    void EndScope() {
        if (scopes)
            --scopes;
        else
            Disable();
    }
    void Disable() {
        allowed = false;
    }
    bool TryCut() {
        if (!allowed || cut || draws < threshold || scopes || !queries.empty())
            return false;
        cut = true;
        return true;
    }

private:
    struct Query {
        std::uintptr_t heap;
        std::uint32_t type, index;
        auto operator<=>(const Query&) const = default;
    };
    std::map<Query, bool> queries;
    size_t threshold, draws{}, scopes{};
    bool allowed{}, cut{};
};

// Second recorder only. The caller keeps the first list on the original recorder.
// Startup and native list/allocator initialization belong to this worker; callbacks
// must wait for the submission fence before reusing an allocator. At submit, encode
// a completion marker, publish, then Wait on the first recorder before its single
// ExecuteCommandLists(2, {first, second}). Both lists share the submission fence.
class ParallelRecordWorker {
public:
    using Initialize = std::function<void*()>;
    explicit ParallelRecordWorker(Initialize initialize);
    ~ParallelRecordWorker();
    ParallelRecordWorker(const ParallelRecordWorker&) = delete;
    RecordStream<>& Stream() {
        return stream;
    }
    void Publish(std::uint64_t sequence);
    void Wait(std::uint64_t sequence);
    void Check();

private:
    void Run(Initialize initialize) noexcept;
    void Fail() noexcept;
    RecordStream<> stream;
    std::atomic<std::uint64_t> finished{};
    std::atomic<unsigned> ready{};
    std::atomic<bool> failed{};
    std::mutex error_mutex;
    std::exception_ptr error;
    std::thread worker; // Start only after all synchronization members are initialized.
};
} // namespace D3D12
