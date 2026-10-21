// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <type_traits>
#include <utility>
#include "video_core/renderer_d3d12/d3d12_record_stream.h"

namespace D3D12 {
// Customize for structures containing COM pointers (barriers and copy locations).
template <class T>
struct RecordLease {
    static void Hold(const T&) {}
    static void Drop(const T&) {}
};
template <class T>
struct RecordLease<T*> {
    static void Hold(T* value) {
        if (value)
            value->AddRef();
    }
    static void Drop(T* value) {
        if (value)
            value->Release();
    }
};
template <class T>
struct RecordValue {
    T value;
    T Resolve(const std::byte*) const {
        return value;
    }
    void Hold() const {
        RecordLease<T>::Hold(value);
    }
    void Drop(const std::byte*) const {
        RecordLease<T>::Drop(value);
    }
};
template <class T>
struct RecordOffset {
    size_t offset, count;
    bool present;
    const T* Resolve(const std::byte* base) const {
        return present ? reinterpret_cast<const T*>(base + offset) : nullptr;
    }
    void Drop(const std::byte* base) const {
        if (present)
            for (size_t i = 0; i < count; ++i)
                RecordLease<T>::Drop(Resolve(base)[i]);
    }
};
template <class T>
struct RecordArray {
    const T* data;
    size_t count;
};
template <class T>
RecordArray<T> Snapshot(const T* data, size_t count) {
    static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 16);
    return {data, data ? count : 0};
}
template <class T>
auto PackArgument(T value, size_t&) {
    return RecordValue<T>{value};
}
template <class T>
auto PackArgument(RecordArray<T> value, size_t& offset) {
    const auto start = offset;
    offset += (sizeof(T) * value.count + 15) & ~size_t{15};
    return RecordOffset<T>{start, value.count, value.data != nullptr};
}
template <class... T>
struct RecordPack;
template <>
struct RecordPack<> {};
template <class T, class... Rest>
struct RecordPack<T, Rest...> {
    T head;
    RecordPack<Rest...> tail;
};
template <size_t I, class T, class... Rest>
const auto& RecordGet(const RecordPack<T, Rest...>& pack) {
    if constexpr (I == 0)
        return pack.head;
    else
        return RecordGet<I - 1>(pack.tail);
}
inline auto MakeRecordPack(size_t&) {
    return RecordPack<>{};
}
template <class T, class... Rest>
auto MakeRecordPack(size_t& offset, T value, Rest... rest) {
    auto head = PackArgument(value, offset);
    auto tail = MakeRecordPack(offset, rest...);
    return RecordPack<decltype(head), decltype(PackArgument(rest, offset))...>{head, tail};
}
template <class T>
void HoldArgument(T value) {
    RecordLease<T>::Hold(value);
}
template <class T>
void HoldArgument(RecordArray<T> value) {
    for (size_t i = 0; i < value.count; ++i)
        RecordLease<T>::Hold(value.data[i]);
}
template <class Stream, class T>
void WriteArgument(Stream&, T) {}
template <class Stream, class T>
void WriteArgument(Stream& stream, RecordArray<T> value) {
    const size_t bytes = sizeof(T) * value.count;
    stream.Write(value.data, bytes);
    const std::byte padding[16]{};
    stream.Write(padding, (-bytes) & 15);
}
// Same byte layout as WriteArgument; no queue routing or boundary check per array.
template <class T>
void CopyArgument(std::byte*&, T) {}
template <class T>
void CopyArgument(std::byte*& output, RecordArray<T> value) {
    const size_t bytes = sizeof(T) * value.count;
    if (bytes)
        std::memcpy(output, value.data, bytes);
    output += bytes;
    const size_t padding = (-bytes) & 15;
    std::memset(output, 0, padding);
    output += padding;
}
template <auto Invoke, class Pack, size_t... I>
void ReplayPack(void* target, const Pack& pack, const std::byte* base, std::index_sequence<I...>) {
    Invoke(target, RecordGet<I>(pack).Resolve(base)...);
}
template <class Pack, size_t... I>
void ReleasePack(const Pack& pack, const std::byte* base, std::index_sequence<I...>) {
    (RecordGet<I>(pack).Drop(base), ...);
}
template <auto Invoke, class Pack, size_t Count>
struct RecordedCall {
    Pack arguments;
    void Replay(void* target, const std::byte* base) const {
        ReplayPack<Invoke>(target, arguments, base, std::make_index_sequence<Count>{});
    }
    void Release(const std::byte* base) const {
        ReleasePack(arguments, base, std::make_index_sequence<Count>{});
    }
};
template <auto Invoke, class Stream, class... Args>
void EncodeCall(Stream& stream, Args... args) {
    size_t offset{};
    using Pack = decltype(MakeRecordPack(offset, args...));
    using Call = RecordedCall<Invoke, Pack, sizeof...(Args)>;
    static_assert(std::is_trivially_copyable_v<Call>);
    // Every variable block is 16-byte aligned, including the first.
    offset = (sizeof(Call) + 15) & ~size_t{15};
    const auto first = offset;
    Call call{MakeRecordPack(offset, args...)};
    (HoldArgument(args), ...);
    if constexpr (requires { stream.template TryBegin<Call>(size_t{}); }) {
        if (auto* packet = stream.template TryBegin<Call>(offset - sizeof(Call))) {
            std::memcpy(packet, &call, sizeof(call));
            std::memset(packet + sizeof(call), 0, first - sizeof(call));
            auto* output = packet + first;
            (CopyArgument(output, args), ...);
            return;
        }
    }
    stream.template Begin<Call>(offset - sizeof(Call));
    stream.Write(&call, sizeof(call));
    const std::byte padding[16]{};
    stream.Write(padding, first - sizeof(call));
    (WriteArgument(stream, args), ...);
}
} // namespace D3D12
