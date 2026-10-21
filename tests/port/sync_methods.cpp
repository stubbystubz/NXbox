// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
// Host replay: production CounterReport and FenceManager, std-only cache/GPU
// doubles.
#include "stubs.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <new>
#include <optional>
#include <string_view>
#include <type_traits>
using u8 = unsigned char;
using u64 = std::uint64_t;
using GPUVAddr = u64;
using DAddr = u64;
using VAddr = u64;
thread_local bool count_allocations{};
thread_local size_t allocations{};
void *operator new(size_t size) {
  if (count_allocations)
    ++allocations;
  if (auto *p = std::malloc(size ? size : 1))
    return p;
  throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
#include "core/core_timing_trace.h"
#include "video_core/fence_manager.h"
#define LOG_ERROR(...) assert(false)
namespace D3D12 {
struct QueryCacheParams {
  static constexpr bool REQUIRE_GPU_COMPLETION = true;
};
} // namespace D3D12
namespace Core {
constexpr u64 DEVICE_PAGEBITS = 12, DEVICE_PAGEMASK = 4095;
}
enum class QueryFlagBits : u32 {
  HasTimestamp = 1,
  IsFence = 2,
  IsRewritten = 4,
  IsInvalidated = 8,
  IsFinalValueSynced = 16,
  IsGuestSynced = 32
};
enum class QueryPropertiesFlags : u32 { HasTimeout = 1, IsAFence = 2 };
template <class T> T operator&(T a, T b) { return T(u32(a) & u32(b)); }
template <class T> T operator|(T a, T b) { return T(u32(a) | u32(b)); }
template <class T> T &operator|=(T &a, T b) { return a = a | b; }
template <class T> bool True(T v) { return u32(v) != 0; }
template <class T> bool False(T v) { return !True(v); }
enum class QueryType { Payload };
struct Query {
  u64 value{}, guest_address{};
  QueryFlagBits flags{};
};
struct Streamer {
  std::array<Query, 740> queries;
  size_t next{};
  size_t WriteCounter(u64 address, bool timestamp, u32 value, u32) {
    auto id = next++;
    assert(id < queries.size());
    queries[id] = {value, address, QueryFlagBits::IsFinalValueSynced};
    if (timestamp)
      queries[id].flags |= QueryFlagBits::HasTimestamp;
    return id;
  }
  Query *GetQuery(size_t id) { return &queries[id]; }
  u64 GetAmendValue() { return 0; }
  void SetAccumulationValue(u64) {}
  void Free(size_t) {}
};
struct Memory {
  std::array<u8, 740 * 16> bytes{};
  template <class T> T *GetPointer(u64 address) {
    return reinterpret_cast<T *>(bytes.data() + address);
  }
  std::optional<u64> GpuToCpuAddress(u64 address) { return address; }
};
struct Part {
  u32 value;
  void Assign(u32 v) { value = v; }
};
struct Location {
  Part stream_id, query_id;
};
struct Cache {
  std::mutex mutex;
  size_t commits{}, pops{};
  std::function<void()> on_commit;
  void AccumulateFlushes() {}
  bool HasUncommittedFlushes() { return false; }
  bool ShouldWaitAsyncFlushes() { return false; }
  void CommitAsyncFlushes() {
    ++commits;
    if (on_commit)
      on_commit();
  }
  void PopAsyncFlushes() {
    assert(pops < commits);
    ++pops;
  }
};
struct Fence : VideoCommon::FenceBase {
  explicit Fence(bool stubbed) : FenceBase(stubbed) {}
};
struct Params {
  using FenceType = std::shared_ptr<Fence>;
  using BufferCacheType = Cache;
  using TextureCacheType = Cache;
  using QueryCacheType = Cache;
  static constexpr bool HAS_ASYNC_CHECK = false;
  static constexpr bool REQUIRE_GPU_COMPLETION = true;
};
struct Manager : VideoCommon::FenceManager<Params> {
  using Base = VideoCommon::FenceManager<Params>;
  Manager(VideoCore::RasterizerInterface &r, Tegra::GPU &g, Cache &t, Cache &b,
          Cache &q)
      : Base(r, g, t, b, q) {}
  bool submitted{};
  size_t retired{}, expected{};
  std::shared_ptr<Fence> CreateFence(bool stubbed) override {
    return std::make_shared<Fence>(stubbed);
  }
  void QueueFence(std::shared_ptr<Fence> &) override {}
  bool IsFenceSignaled(std::shared_ptr<Fence> &) const override {
    return submitted;
  }
  void WaitFence(std::shared_ptr<Fence> &) override { assert(submitted); }
  bool ShouldFlushFence() const override { return false; }
  ~Manager() override = default;
};
struct Rasterizer {
  VideoCore::RasterizerInterface native;
  Tegra::GPU gpu;
  Cache texture, buffer, query;
  Manager manager{native, gpu, texture, buffer, query};
  Rasterizer() {
    // Production QueryCache::CommitAsyncFlushes adds UnregisterPending before
    // the signal.
    query.on_commit = [this] { manager.SyncOperation([] {}); };
  }
  void SignalFence(std::function<void()> f) {
    auto sequence = manager.expected++;
    manager.SignalFence([this, sequence, f = std::move(f)] {
      assert(sequence == manager.retired++);
      f();
    });
  }
  void SyncOperation(std::function<void()> f) {
    manager.SyncOperation(std::move(f));
  }
};
struct Runtime {
  u64 ReportPending(u64) { return 0; }
  void ReportCompleted(bool invalidated, u64) { assert(!invalidated); }
};
struct Gpu {
  u64 GetTicks() { return 123456; }
};
struct Impl {
  Streamer streamer;
  std::array<Streamer *, 1> streamers{&streamer};
  Memory device_memory;
  Rasterizer rasterizer;
  Runtime runtime;
  Gpu gpu;
  std::vector<Location> pending_unregister;
  Query *ObtainQuery(Location loc) {
    return streamer.GetQuery(loc.query_id.value);
  }
};
template <class Traits> struct QueryCacheBase {
  using QueryLocation = Location;
  Impl storage;
  Impl *impl = &storage;
  Memory *gpu_memory = &storage.device_memory;
  std::mutex cache_mutex;
  std::map<u64, std::map<u32, Location>> cached_queries;
  void CounterReport(GPUVAddr, QueryType, QueryPropertiesFlags, u32, u32);
};
template <class Traits>
void QueryCacheBase<Traits>::CounterReport(GPUVAddr addr,
                                           QueryType counter_type,
                                           QueryPropertiesFlags flags,
                                           u32 payload, u32 subreport) {
  __COUNTER_REPORT__
}

int main(int argc, char **argv) {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 2000;
  const bool immediate = argc > 2 && std::string_view{argv[2]} == "immediate";
  Settings::high = false;
  QueryCacheBase<D3D12::QueryCacheParams> cache;
  auto &state = cache.storage;
  auto &manager = state.rasterizer.manager;
  auto frame = [&] {
    manager.submitted = false;
    state.streamer.next = 0;
    state.device_memory.bytes.fill(0);
    // 700 Payload releases (short fence or long immediate) + 40 syncpoints.
    for (u32 i = 0; i < 740; ++i) {
      if (i % 37 == 0 || i % 37 == 1) {
        manager.SignalSyncPoint(7);
        continue;
      }
      const bool fence = immediate ? i % 37 == 2 : i % 37 != 2;
      auto flags = fence ? QueryPropertiesFlags::IsAFence
                         : QueryPropertiesFlags::HasTimeout;
      cache.CounterReport(i * 16, QueryType::Payload, flags, i + 1, 0);
      u32 visible{};
      std::memcpy(&visible, state.device_memory.bytes.data() + i * 16, 4);
      assert(visible == (fence ? 0 : i + 1));
    }
    assert(state.rasterizer.gpu.host.sync.host <
           state.rasterizer.gpu.host.sync.guest);
    manager.submitted = true;
    manager.WaitPendingFences(true);
    assert(state.rasterizer.gpu.host.sync.host ==
           state.rasterizer.gpu.host.sync.guest);
    assert(manager.expected == manager.retired);
    for (u32 i = 0; i < 740; ++i) {
      if (i % 37 < 2)
        continue;
      u32 visible{};
      std::memcpy(&visible, state.device_memory.bytes.data() + i * 16, 4);
      assert(visible == i + 1);
    }
    for (auto *c : {&state.rasterizer.texture, &state.rasterizer.buffer,
                    &state.rasterizer.query})
      assert(c->commits == c->pops);
    cache.cached_queries.clear();
    state.pending_unregister.clear();
  };
  for (int i = 0; i < 50; ++i)
    frame();
  count_allocations = true;
  frame();
  count_allocations = false;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i)
    frame();
  const auto ns = std::chrono::duration<double, std::nano>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  std::cout << "{\"ns_per_call\":" << ns / (frames * 740)
            << ",\"us_per_frame\":" << ns / (frames * 1000)
            << ",\"allocations_per_frame\":" << allocations << "}\n";
}
