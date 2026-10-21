// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace D3D12 {
// Preserve copy order while collapsing adjacent handles from a single CPU heap.
// Repeated null handles remain separate ranges: adjacency never means equality.
template <class Handle, std::size_t Capacity>
struct DescriptorCopyRanges {
    void Add(Handle handle, std::size_t stride, std::uintptr_t heap = 0) {
        if (stride && count && heap == last_heap && handle.ptr >= starts[count - 1].ptr &&
            handle.ptr - starts[count - 1].ptr ==
                static_cast<std::size_t>(sizes[count - 1]) * stride) {
            ++sizes[count - 1];
            return;
        }
        starts[count] = handle;
        sizes[count++] = 1;
        last_heap = heap;
    }
    std::array<Handle, Capacity> starts;
    std::array<std::uint32_t, Capacity> sizes;
    std::size_t count{};
    std::uintptr_t last_heap{};
};

// Fixed storage: descriptor copies never allocate while a draw is being recorded.
// Flush before recycling any source descriptor; sources are CPU-only heap handles.
template <class Handle, std::size_t Capacity = 32>
class DescriptorCopyBatch {
public:
    template <class Copy>
    void Add(Handle destination, Handle source, Copy&& copy) {
        destinations[count] = destination;
        sources[count++] = source;
        if (count == Capacity)
            Flush(copy);
    }
    template <class Copy>
    void Flush(Copy&& copy) {
        if (!count)
            return;
        copy(count, destinations.data(), sources.data());
        count = 0;
    }

private:
    static_assert(Capacity > 0);
    std::array<Handle, Capacity> destinations{};
    std::array<Handle, Capacity> sources{};
    std::size_t count{};
};

// Source handles are valid only until their CPU heap generation changes. GPU tables
// have a separate submission lifetime; recycling CPU sources never extends it.
template <class Handle, std::size_t Capacity = 4096>
struct DescriptorSources {
    // Allocate once with the persistent draw scratch, then swap in constant time.
    std::vector<Handle> handles = std::vector<Handle>(Capacity);
    std::uint64_t generation{};
    std::size_t count{};
    bool valid{};

    void Reset(std::size_t size) {
        count = size;
        generation = 0;
        valid = size <= Capacity;
    }
    template <class Generation, class Resolve>
    void Set(std::size_t index, const DescriptorSources& previous, bool unchanged,
             Generation&& current, Resolve&& resolve) {
        if (!valid || index >= count)
            return;
        const auto handle = previous.Current(current()) && index < previous.count && unchanged
                                ? previous.handles[index]
                                : resolve();
        if (generation && generation != current())
            valid = false;
        generation = current();
        handles[index] = handle;
    }
    bool Current(std::uint64_t current) const {
        return valid && generation == current;
    }
    bool Matches(const DescriptorSources& other, std::uint64_t current) const {
        return Current(current) && other.Current(current) && count == other.count &&
               std::equal(handles.begin(), handles.begin() + count, other.handles.begin());
    }
};

// Bounded set-associative cache: lookup and replacement never allocate. A hash
// selects a candidate only; exact ordered handles and layout resolve collisions.
// CPU generations prevent recycled handles from aliasing different descriptors.
template <class Value, std::size_t Entries = 64, std::size_t MaxHandles = 128, std::size_t Ways = 1>
class BoundHandleTableCache {
public:
    const Value* Find(std::uint64_t tick, std::uint64_t generation,
                      const std::array<std::uint32_t, 3>& sizes,
                      std::span<const std::uintptr_t> handles) const {
        if (handles.size() > MaxHandles)
            return nullptr;
        const auto set = Hash(sizes, handles) % Entries;
        for (std::size_t way = 0; way < Ways; ++way) {
            const auto& entry = entries[set * Ways + way];
            if (entry.valid && entry.tick == tick && entry.generation == generation &&
                entry.sizes == sizes && entry.count == handles.size() &&
                std::equal(handles.begin(), handles.end(), entry.handles.begin()))
                return &entry.value;
        }
        return nullptr;
    }
    void Remember(std::uint64_t tick, std::uint64_t generation,
                  const std::array<std::uint32_t, 3>& sizes,
                  std::span<const std::uintptr_t> handles, Value value) {
        if (handles.size() > MaxHandles)
            return;
        const auto set = Hash(sizes, handles) % Entries;
        // Prefer stale ways; round-robin replacement bounds collision work.
        auto way = next_way[set];
        std::optional<std::size_t> stale;
        for (std::size_t candidate = 0; candidate < Ways; ++candidate) {
            const auto& old = entries[set * Ways + candidate];
            if (!old.valid || old.tick != tick || old.generation != generation) {
                if (!stale)
                    stale = candidate;
            } else if (old.sizes == sizes && old.count == handles.size() &&
                       std::equal(handles.begin(), handles.end(), old.handles.begin())) {
                entries[set * Ways + candidate].value = value;
                return;
            }
        }
        if (stale)
            way = *stale;
        next_way[set] = (way + 1) % Ways;
        auto& entry = entries[set * Ways + way];
        entry.valid = true;
        entry.tick = tick;
        entry.generation = generation;
        entry.sizes = sizes;
        entry.count = handles.size();
        std::copy(handles.begin(), handles.end(), entry.handles.begin());
        entry.value = value;
    }
    void Clear() {
        for (auto& entry : entries)
            entry.valid = false;
    }

private:
    static_assert(Entries > 0 && Ways > 0);
    static std::size_t Hash(const std::array<std::uint32_t, 3>& sizes,
                            std::span<const std::uintptr_t> handles) {
        std::size_t hash = handles.size();
        const auto combine = [&](std::size_t part) {
            hash ^= part + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        };
        for (const auto size : sizes)
            combine(size);
        for (const auto handle : handles)
            combine(handle);
        return hash;
    }
    struct Entry {
        bool valid{};
        std::uint64_t tick{}, generation{};
        std::array<std::uint32_t, 3> sizes{};
        std::size_t count{};
        std::array<std::uintptr_t, MaxHandles> handles{};
        Value value{};
    };
    std::array<Entry, Entries * Ways> entries{};
    std::array<std::size_t, Entries> next_way{};
};

// A recording owns immutable slots only until its submission tick changes.
// Full semantic keys are compared; hash collisions cannot alias different tables.
template <class Value>
class RecordingTableCache {
public:
    std::optional<Value> Find(std::uint64_t current, std::string_view key) {
        Refresh(current);
        const auto found = tables.find(std::string{key});
        return found == tables.end() ? std::nullopt : std::optional<Value>{found->second};
    }
    void Remember(std::uint64_t current, std::string key, Value value) {
        Refresh(current);
        tables.insert_or_assign(std::move(key), std::move(value));
    }

private:
    void Refresh(std::uint64_t current) {
        if (tick != current) {
            tables.clear();
            tick = current;
        }
    }
    std::uint64_t tick{};
    std::unordered_map<std::string, Value> tables;
};

// GPU slots are immutable within a recording. Hash the ordered semantic keys,
// then compare every key and the root-table layout to resolve collisions.
// Value owns resource pins; CPU descriptor heap generations are independent.
template <class Key, class Value, class Hash>
class DescriptorSetCache {
public:
    const Value* Find(std::uint64_t current, const std::array<std::uint32_t, 3>& sizes,
                      std::span<const Key> keys) const {
        if (tick != current)
            return nullptr;
        const auto [first, last] = tables.equal_range(HashSet(sizes, keys));
        for (auto entry = first; entry != last; ++entry) {
            const auto& candidate = entry->second;
            if (candidate.sizes == sizes && candidate.keys.size() == keys.size() &&
                std::equal(keys.begin(), keys.end(), candidate.keys.begin()))
                return &candidate.value;
        }
        return nullptr;
    }
    void Remember(std::uint64_t current, const std::array<std::uint32_t, 3>& sizes,
                  std::span<const Key> keys, Value value) {
        if (tick != current) {
            Clear();
            tick = current;
        }
        tables.emplace(HashSet(sizes, keys),
                       Entry{sizes, {keys.begin(), keys.end()}, std::move(value)});
    }
    void Clear() {
        tables.clear();
    }

private:
    static std::size_t HashSet(const std::array<std::uint32_t, 3>& sizes,
                               std::span<const Key> keys) {
        std::size_t hash = keys.size();
        const auto combine = [&](std::size_t part) {
            hash ^= part + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        };
        for (const auto size : sizes)
            combine(size);
        for (const auto& key : keys)
            combine(Hash{}(key));
        return hash;
    }
    struct Entry {
        std::array<std::uint32_t, 3> sizes;
        std::vector<Key> keys;
        Value value;
    };
    std::uint64_t tick{};
    std::unordered_multimap<std::size_t, Entry> tables;
};
} // namespace D3D12

#ifdef _WIN32
#include <d3d12.h>

#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"

namespace Tegra::Texture {
struct TSCEntry;
}
namespace D3D12 {
class ImageView;
/// Rewrites an SRV description to the shader's declared dimension when the resource allows it.
std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> AdaptSrvDimension(
    const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, D3D12_SRV_DIMENSION wanted,
    const D3D12_RESOURCE_DESC& resource);
class Sampler;
class Framebuffer;

// ---- Buffer cache bindings (filled by BufferCacheRuntime hooks, consumed per draw) ----
struct BoundBuffer {
    ResourcePtr resource; // null: null buffer, a null descriptor is written
    u32 offset{};
    u32 size{};
    bool written{};
    u32 pixel_format{}; // VideoCore::Surface::PixelFormat for texel buffers
    bool image{};       // Image buffers use UAV descriptors, including read-only images.
};
struct StageBufferBindings {
    std::vector<BoundBuffer> uniform;
    std::vector<BoundBuffer> storage;
    std::vector<BoundBuffer> texel; // texture buffers first, then image buffers
    void Clear() {
        uniform.clear();
        storage.clear();
        texel.clear();
    }
};
struct BoundVertexBuffer {
    ResourcePtr resource;
    u64 offset{};
    u64 size{};
    u32 stride{};
};
struct GeometryBindings {
    static constexpr size_t MaxVertexBuffers = 32;
    std::array<BoundVertexBuffer, MaxVertexBuffers> vertex;
    ResourcePtr index;
    u64 index_offset{};
    u64 index_size{};
    bool index_is_32bit{};
    bool index_unsupported{}; // u8 indices / indexed quad conversion: draw is skipped
};
struct BindingState {
    static constexpr size_t NumStages = 5;
    std::array<StageBufferBindings, NumStages> stages;
    GeometryBindings geometry;
    size_t current_stage{};
};

// ---- Pure helpers (also used by the rasterizer) ----
DXGI_FORMAT DsvFormat(DXGI_FORMAT view_format);
// shader_compares: the shader samples with a depth reference (TextureDescriptor::is_depth).
D3D12_SAMPLER_DESC MakeSamplerDesc(const Tegra::Texture::TSCEntry& entry, bool shader_compares);
// Min/max reduction samplers need TiledResourcesTier 2. The Xbox Series reports tier 1 and
// removes the device (DXGI_ERROR_INVALID_CALL) on such a CreateSampler; set once per device.
void SetMinMaxFilteringSupported(bool supported);

struct DescriptorKey {
    std::array<u8, 64> description{};
    std::uintptr_t resource{};
    u32 kind{}, size{};
    bool operator==(const DescriptorKey&) const = default;
};
// Planning uses the same writers and full keys as the CPU descriptor cache.
struct DescriptorPlan {
    DescriptorSources<std::uintptr_t> sources;
    std::array<u32, 3> sizes{};
    std::vector<DescriptorKey> keys;
    std::vector<u8> initialized;
    std::vector<ResourcePtr> resources;
    explicit DescriptorPlan(std::array<u32, 3> sizes_) {
        // Allocate both persistent plans once, including the previous-draw swap
        // partner. Larger future layouts can still grow through the legacy path.
        keys.reserve(4096);
        resources.reserve(4096);
        Reset(sizes_);
    }
    void Reset(std::array<u32, 3> sizes_) {
        sizes = sizes_;
        const u32 count = sizes[0] + sizes[1] + sizes[2];
        // Every slot, including padding, is written before FinishDrawTable.
        // Keep key storage initialized and reuse its capacity across draws.
        keys.resize(count);
        if (BisectEnabled("NXBOX_D3D12_VALIDATE_COMMANDS"))
            initialized.assign(count, 0);
        sources.Reset(count);
        resources.clear();
        resources.resize(count);
    }
};

struct DescriptorTable {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    u32 count{};
    u32 stride{};
    DescriptorPlan* plan{};
    u32 first{};
    D3D12_CPU_DESCRIPTOR_HANDLE Slot(u32 index) const {
        return {cpu.ptr + static_cast<SIZE_T>(index) * stride};
    }
    // Sub-range of a larger allocation (one ring allocation feeds the three view tables).
    DescriptorTable Slice(u32 offset, u32 n) const {
        return {Slot(offset), {gpu.ptr + static_cast<UINT64>(offset) * stride},
                n,            stride,
                plan,         first + offset};
    }
};

// Shader-visible CBV/SRV/UAV and sampler ring heaps plus CPU-only RTV/DSV rings. Every
// allocation is tagged with Context::CurrentTick() and recycled once that tick completes.
// When a ring is full the allocator submits the open command list and/or waits on the oldest
// tick; callers must therefore allocate BEFORE recording per-draw state.
class D3D12DescriptorAllocator {
public:
    static constexpr u32 ViewHeapSize = 1u << 18;
    static constexpr u32 SamplerHeapSize = 2048; // D3D12 limit for shader-visible samplers
    static constexpr u32 RtvHeapSize = 4096;
    static constexpr u32 DsvHeapSize = 512;

    explicit D3D12DescriptorAllocator(Context& context, u32 view_capacity = ViewHeapSize,
                                      u32 sampler_capacity = SamplerHeapSize);

    // Reserve the whole draw/dispatch before allocating from any ring. Reclaiming
    // another heap must never submit tables already reserved for this operation.
    void Reserve(u32 view_count, u32 sampler_count, u32 rtv_count = 0, u32 dsv_count = 0);
    // Reserve only missing tables, then recheck after Reserve (it may change the tick).
    bool CanReuseDrawTable(const DescriptorPlan& plan, bool sampler = false) const;
    DescriptorTable FinishDrawTable(DescriptorPlan& plan, bool sampler = false);
    // Rehydrate semantic memo hits through the same generation-checked sources as writers.
    void ResolvePlanSources(DescriptorPlan& plan, bool sampler = false);
    void SetHeaps(CommandFacade* commands, bool cache_draw_state = false);
    DescriptorTable AllocateViews(u32 count);
    DescriptorTable AllocateSamplers(u32 count);
    D3D12_CPU_DESCRIPTOR_HANDLE AllocateRtv(u32 count);
    D3D12_CPU_DESCRIPTOR_HANDLE AllocateDsv();
    /// Fixed RTV/DSV slots for the draw path. OMSetRenderTargets reads RTV/DSV
    /// descriptors when it is recorded, so these can be rewritten at any time and
    /// only need rewriting when the bound attachments change.
    D3D12_CPU_DESCRIPTOR_HANDLE BoundRtvs() const {
        return bound_rtvs.heap->GetCPUDescriptorHandleForHeapStart();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE BoundDsv() const {
        return bound_dsv.heap->GetCPUDescriptorHandleForHeapStart();
    }

    // Writers. Null variants keep unbound/unsupported slots valid.
    void WriteNullCbv(const DescriptorTable& table, u32 slot);
    void WriteNullSrv(const DescriptorTable& table, u32 slot, D3D12_SRV_DIMENSION dimension);
    void WriteNullUav(const DescriptorTable& table, u32 slot);
    void WriteNullSampler(const DescriptorTable& table, u32 slot);
    void WriteCbv(const DescriptorTable& table, u32 slot, const BoundBuffer& buffer);
    void WriteStorageUav(const DescriptorTable& table, u32 slot, const BoundBuffer& buffer);
    void WriteTexelSrv(const DescriptorTable& table, u32 slot, const BoundBuffer& buffer);
    void WriteTexelUav(const DescriptorTable& table, u32 slot, const BoundBuffer& buffer);
    // Returns false (and writes a null view) when the view has no backing image.
    bool WriteImageSrv(const DescriptorTable& table, u32 slot, const ImageView& view,
                       D3D12_SRV_DIMENSION fallback_dimension, const ResourcePtr& replacement = {});
    void WriteSampler(const DescriptorTable& table, u32 slot, const Sampler& sampler,
                      bool shader_compares);

    bool WriteImageUav(const DescriptorTable& table, u32 slot, const ImageView& view,
                       DXGI_FORMAT format);
    // Copy an immutable CPU descriptor into a fence-retired shader-visible slot.
    void WriteUavDescriptor(const DescriptorTable& table, u32 slot, const ResourcePtr& resource,
                            const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc);
    // Semantic keys must include resource identity, ranges, format and swizzle.
    // Tables are reusable only within one recording tick, never across retirement.
    std::optional<DescriptorTable> FindTable(std::string_view key, bool sampler = false);
    void RememberTable(std::string key, DescriptorTable table, bool sampler = false);
    ID3D12DescriptorHeap* ResourceHeap() const {
        return views.heap.Get();
    }
    ID3D12DescriptorHeap* SamplerHeap() const {
        return samplers.heap.Get();
    }

    // Render target views derived from an image view's subresource range.
    // nullptr (or a view without image) creates a null RTV that discards writes.
    void CreateRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const ImageView* view,
                   const ResourcePtr& replacement = {}, u32 layer_count = 0);
    void CreateDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const ImageView& view,
                   D3D12_DSV_FLAGS flags = D3D12_DSV_FLAG_NONE, u32 layer_count = 0);
    /// Release cached GPU tables and let the CPU view cache refresh its resource pins.
    void TickFrame() {
        new_frame = true;
        for (auto& cache : handle_table_cache)
            cache.Clear();
        for (auto& cache : draw_table_cache)
            cache.Clear();
        if (descriptor_table_cache) {
            for (auto& previous : previous_draw_tables)
                previous.tick = 0;
        }
    }

  private:
    struct Ring {
        ComPtr<ID3D12DescriptorHeap> heap;
        RingAllocator math;
        u32 stride{};
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        bool shader_visible{};
        Ring(u32 capacity) : math{capacity} {}
    };
    void Init(Ring& ring, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity, bool visible);
    DescriptorTable Allocate(Ring& ring, u32 count);

    struct DescriptorKeyHash {
        size_t operator()(const DescriptorKey& key) const noexcept {
            const std::string_view bytes{reinterpret_cast<const char*>(key.description.data()),
                                         key.size};
            return std::hash<std::string_view>{}(bytes) ^
                   std::hash<std::uintptr_t>{}(key.resource) ^ (static_cast<size_t>(key.kind) << 1);
        }
    };
    struct CachedDescriptor {
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        ResourcePtr resource; // Prevent address reuse while the descriptor is cached.
    };
    struct DescriptorCache {
        ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        std::unordered_map<DescriptorKey, CachedDescriptor, DescriptorKeyHash> entries;
        u32 cursor{}, stride{}, capacity{};
        u64 generation{1};
    };
    void InitCache(DescriptorCache& cache, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity);
    void RefreshCaches();
    bool IsNullDescriptorHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
        return persistent_null_descriptors && handle.ptr >= null_view_cache.cpu.ptr &&
               handle.ptr - null_view_cache.cpu.ptr <
                   static_cast<SIZE_T>(null_view_cache.capacity) * null_view_cache.stride;
    }
    using CopyBatch = DescriptorCopyBatch<D3D12_CPU_DESCRIPTOR_HANDLE>;
    D3D12_CPU_DESCRIPTOR_HANDLE ResolveDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                  const ResourcePtr& resource,
                                                  const DescriptorKey& key, CopyBatch* batch);
    void FlushCopies(D3D12_DESCRIPTOR_HEAP_TYPE type, CopyBatch& batch);
    void CopyDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE type, size_t count,
                         const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
                         const D3D12_CPU_DESCRIPTOR_HANDLE* sources);
    void WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE type, const ResourcePtr& resource, u32 kind,
                     const void* desc, size_t size, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                     CopyBatch* batch = nullptr);
    static DescriptorKey MakeDescriptorKey(const ResourcePtr& resource, u32 kind, const void* desc,
                                           size_t size);
    void WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE type, const ResourcePtr& resource, u32 kind,
                     const void* desc, size_t size, const DescriptorTable& table, u32 slot);
    struct PreviousDrawTable {
        u64 tick{};
        DescriptorPlan plan{{0, 0, 0}};
        DescriptorTable table;
    };
    struct CachedDrawTable {
        DescriptorTable table;
        std::vector<ResourcePtr> resources;
    };
    const DescriptorTable* FindHandleDrawTable(const DescriptorPlan& plan, bool sampler) const;
    const CachedDrawTable* FindCachedDrawTable(const DescriptorPlan& plan, bool sampler) const;
    bool CanReusePreviousDrawTable(const DescriptorPlan& plan, bool sampler) const;
    void RememberDrawTable(const DescriptorPlan& plan, DescriptorTable table, bool sampler);
    std::array<DescriptorSetCache<DescriptorKey, CachedDrawTable, DescriptorKeyHash>, 2>
        draw_table_cache;
    std::array<PreviousDrawTable, 2> previous_draw_tables;
    std::array<BoundHandleTableCache<DescriptorTable, 64, 128, 4>, 2> handle_table_cache;
    const bool handle_table_reuse;
    const bool descriptor_table_cache;
    const bool descriptor_fastpath;
    const bool descriptor_ranges;
    const bool persistent_null_descriptors;
    u64 cache_tick{};
    bool new_frame{true};
    DescriptorCache view_cache, sampler_cache;
    DescriptorCache null_view_cache;
    DescriptorCache bound_rtvs, bound_dsv;
    std::unordered_set<int> logged_srv_mismatches;
    std::unordered_map<SIZE_T, DescriptorKey> slot_contents;
    std::array<RecordingTableCache<DescriptorTable>, 2> table_cache;
    Context& context;
    Ring views{ViewHeapSize};
    Ring samplers{SamplerHeapSize};
    Ring rtvs{RtvHeapSize};
    Ring dsvs{DsvHeapSize};
};
} // namespace D3D12

#endif // _WIN32
