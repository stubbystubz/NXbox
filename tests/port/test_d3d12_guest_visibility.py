# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production CPU visibility paths with host-only cache mocks."""

import unittest
from pathlib import Path

from test_d3d12_depth_recording import function_body, run_cpp
from test_d3d12_query_fence_lifetime import DRIVER

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "src/video_core/renderer_d3d12"


class GuestVisibilityTests(unittest.TestCase):
    def test_query_flush_wins_over_older_overlapping_downloads(self):
        method = function_body(
            (RENDERER / "d3d12_rasterizer.cpp").read_text(), "void RasterizerD3D12::FlushRegion"
        )
        run_cpp(
            r"""
#include <cassert>
#include <cstdint>
#include <mutex>
using u64=std::uint64_t; using DAddr=u64;
namespace VideoCommon {
namespace SubmissionTrace { template<class... Args> void Log(const char*, Args&&...) {} }
enum class CacheType { BufferCache=1,TextureCache=2,QueryCache=4,All=7 };
CacheType operator&(CacheType a,CacheType b) { return CacheType(int(a)&int(b)); }
}
bool True(VideoCommon::CacheType v) { return int(v)!=0; }
int guest=0;
struct Cache {
    std::mutex mutex;
    int value;
    void DownloadMemory(u64,u64) { guest=value; }
    void FlushRegion(u64,u64) { guest=value; }
};
struct QueryRuntime { void FlushPendingReports(u64,u64) {} };
struct RasterizerD3D12 {
    QueryRuntime query_runtime;
    bool OnGpuPipeline() const { return false; }
    template<class F> auto RunGpuPipeline(F&& function) { return function(); }
    Cache texture_cache{{},1},buffer_cache{{},2},query_cache{{},42};
    void FlushRegion(DAddr,u64,VideoCommon::CacheType);
};
"""
            + method
            + r"""
int main() {
    RasterizerD3D12 r;
    r.FlushRegion(4096,8,VideoCommon::CacheType::All);
    assert(guest==42); // Before the fix an older download overwrote the report.
    r.FlushRegion(4096,8,VideoCommon::CacheType::BufferCache);
    assert(guest==2);
    r.FlushRegion(0,8,VideoCommon::CacheType::All);
    assert(guest==2);
}
"""
        )

    def test_cpu_read_and_deferred_write_preserve_gpu_results(self):
        source = (RENDERER / "d3d12_rasterizer.cpp").read_text()
        methods = "\n".join(
            function_body(source, signature)
            for signature in (
                "bool RasterizerD3D12::OnCPUWrite",
                "void RasterizerD3D12::OnCacheInvalidation",
                "VideoCore::RasterizerDownloadArea RasterizerD3D12::GetFlushArea",
            )
        )
        run_cpp(
            r"""
#include <cassert>
#include <cstdint>
#include <mutex>
#include <optional>
using u64=std::uint64_t; using DAddr=u64; using PAddr=u64;
namespace Core { constexpr u64 DEVICE_PAGESIZE=4096; }
namespace Common {
u64 AlignDown(u64 a,u64 b) { return a/b*b; }
u64 AlignUp(u64 a,u64 b) { return (a+b-1)/b*b; }
}
namespace VideoCommon {
namespace SubmissionTrace { template<class... Args> void Log(const char*, Args...) {} }
enum class CacheType { BufferCache=1,TextureCache=2,ShaderCache=4,QueryCache=8 };
CacheType operator|(CacheType a,CacheType b) { return CacheType(int(a)|int(b)); }
}
namespace VideoCore {
struct RasterizerDownloadArea { u64 start_address,end_address; bool preemtive; };
}
struct Cache {
    std::mutex mutex;
    bool dirty=false;
    int writes=0,reads=0;
    void WriteMemory(u64,u64) { ++writes; }
    bool OnCPUWrite(u64,u64) { return dirty; }
    std::optional<VideoCore::RasterizerDownloadArea> GetFlushArea(u64 a,u64 s) {
        ++reads;
        if(dirty) return VideoCore::RasterizerDownloadArea{a,a+s,false};
        return {};
    }
    void InvalidateRegion(u64,u64) { ++writes; }
};
struct QueryRuntime { void FlushPendingReports(u64,u64) {} };
struct RasterizerD3D12 {
    QueryRuntime query_runtime;
    bool OnGpuPipeline() const { return false; }
    template<class F> auto RunGpuPipeline(F&& function) { return function(); }
    Cache buffer_cache,texture_cache,pipeline;
    Cache* default_pipeline_cache=&pipeline;
    int invalidated=0;
    void InvalidateRegion(u64,u64,VideoCommon::CacheType types) { invalidated=int(types); }
    bool OnCPUWrite(DAddr,u64);
    void OnCacheInvalidation(DAddr,u64);
    VideoCore::RasterizerDownloadArea GetFlushArea(PAddr,u64);
};
"""
            + methods
            + r"""
int main() {
    RasterizerD3D12 r;
    r.buffer_cache.dirty=true;
    // GPU::OnCPURead schedules a FlushRegion iff preemtive is false.
    auto a=r.GetFlushArea(4096,8);
    assert(!a.preemtive && a.start_address==4096 && a.end_address==4104);
    assert(r.buffer_cache.reads==1);
    assert(r.OnCPUWrite(4096,8));
    assert(r.texture_cache.writes==0 && r.pipeline.writes==0);
    r.OnCacheInvalidation(4096,8);
    assert(r.invalidated==7); // A gathered CPU store must not cancel query callbacks.
    r.buffer_cache.dirty=false;
    assert(!r.OnCPUWrite(4096,8));
    assert(r.texture_cache.writes==1 && r.pipeline.writes==1);
    r.texture_cache.dirty=true;
    a=r.GetFlushArea(8192,16);
    assert(!a.preemtive && a.start_address==8192);
    r.texture_cache.dirty=false;
    a=r.GetFlushArea(8193,1);
    assert(a.preemtive && a.start_address==8192 && a.end_address==12288);
}
"""
        )

    def test_query_counters_cover_completion_invalidation_and_immediate_payload(self):
        source = (ROOT / "src/video_core/query_cache/query_cache.h").read_text()
        marker = "template <typename Traits>\nvoid QueryCacheBase<Traits>::CounterReport"
        method = marker + source.split(marker, 1)[1].split("template <typename Traits>", 1)[0]
        driver = DRIVER.replace(
            function_body(DRIVER, "struct Runtime") + ";",
            """struct Runtime {
                int pending=0,written=0,invalidated=0;
                u64 sequence=0;
                u64 ReportPending(VAddr) { ++pending; return ++sequence; }
                void ReportCompleted(bool bad, u64 token) {
                    assert(token==sequence && pending>0); --pending;
                    if(bad) ++invalidated; else ++written;
                }
            };""",
        )
        driver = driver[: driver.index("int main()")]
        run_cpp(
            '#include "core/core_timing_trace.h"\n'
            + "namespace D3D12 { struct QueryCacheParams; }\n"
            + driver.replace("__COUNTER_REPORT__", method)
            + r"""
int main() {
    using namespace VideoCommon;
    QueryCacheBase<Native> r;
    r.CounterReport(16,QueryType::Payload,QueryPropertiesFlags::IsAFence,42,0);
    assert(r.impl->runtime.pending==1 && r.Value()==0);
    r.impl->rasterizer.pending[0]();
    assert(r.impl->runtime.pending==0 && r.impl->runtime.written==1 && r.Value()==42);
    r.Retire();
    r.impl->rasterizer.pending.clear();
    r.CounterReport(16,QueryType::Payload,QueryPropertiesFlags::IsAFence,77,0);
    r.impl->payload.queries.back().flags |= QueryFlagBits::IsInvalidated;
    r.impl->rasterizer.pending[0]();
    assert(r.impl->runtime.pending==0 && r.impl->runtime.invalidated==1 && r.Value()==42);
    r.Retire();
    r.CounterReport(16,QueryType::Payload,QueryPropertiesFlags::None,99,0);
    assert(r.impl->runtime.pending==0 && r.Value()==99);
}
"""
        )

    def test_profile_without_present_has_finite_rates_and_visibility(self):
        method = function_body(
            (RENDERER / "d3d12_context.cpp").read_text(), "void FrameProfile::EndFrame"
        )
        run_cpp(
            r"""
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
using u64=std::uint64_t; using DWORD=unsigned;
DWORD GetCurrentThreadId() { return 1; }
u64 ProfileWallTime() { return 60000000; }
u64 ProfileCpuTime() { return 20000000; }
namespace fmt {
template<class T> void Check(T) {}
void Check(double v) { assert(std::isfinite(v)); }
template<class... T> std::string format(const char*,T... args) {
    (Check(args),...); return "profile";
}
}
std::string logged;
void Diagnostic(const std::string& v) {
    if (!v.starts_with("D3D12_GUEST_WAITS") && !v.starts_with("D3D12_MAXWELL_METHODS")) logged=v;
}
#include "video_core/renderer_d3d12/d3d12_context.h"
using D3D12::FramePhase;
using D3D12::FrameCount;
struct Timing {
    std::array<u64,static_cast<size_t>(FramePhase::Count)> cpu{}; u64 last=10000000; int active=0;
    void Reset(u64 v) { last=v; }
};
struct FrameProfile {
    bool enabled=true, method_enabled=false;
    u64 method_clock_base{};
    std::array<Common::MaxwellMethodProfile::Sample,
               Common::MaxwellMethodProfile::ClassCount +
               Common::MaxwellMethodProfile::RegisterCount> samples{};
    std::atomic<DWORD> owner{1};
    Timing timing;
    std::array<u64,static_cast<size_t>(FrameCount::Count)> counts{};
    u64 frames=0,fence_wall=0,start_wall=0,initial_cpu=0;
    std::atomic<u64> gpu_busy{};
    u64 TakeRecorderCpu() { return 0; }
    u64 recorder_waits{}, recorder_wait_ns{};
    std::unordered_map<u64,u64> transitions;
    std::unordered_map<std::string_view,std::pair<u64,u64>> waits;
    D3D12::Present::PresentGapSamples present_gaps, present_call_gaps;
    std::function<std::string()> guest_visibility;
    void BindRecordingThread() {}
    void Switch(int) {}
    void EndFrame(bool presented=true);
};
"""
            + method
            + r"""
int main() {
    FrameProfile p;
    p.guest_visibility=[] { return " pending_guest_queries=3"; };
    p.counts[0]=100;
    p.EndFrame(false);
    assert(logged.ends_with(" pending_guest_queries=3") && p.frames==0);
    auto source=logged;
    p.enabled=false;
    p.EndFrame(false);
    assert(logged==source);
}
"""
        )
        source = (RENDERER / "d3d12_context.cpp").read_text()
        submit = function_body(source, "u64 Context::Submit")
        self.assertIn("profile.EndFrame(false)", submit)
        self.assertLess(submit.index("submitted = tick"), submit.index("profile.EndFrame(false)"))

    def test_fence_and_callback_counters_retire_after_guest_work(self):
        source = (RENDERER / "d3d12_fence_manager.cpp").read_text()
        methods = "\n".join(
            function_body(source, signature)
            for signature in (
                "void FenceManager::SignalFence",
                "void FenceManager::SyncOperation",
            )
        )
        run_cpp(
            r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>
using u64=std::uint64_t;
struct GenericFenceManager {
    std::vector<std::function<void()>> callbacks;
    void SignalFence(std::function<void()>&& f) { callbacks.push_back(std::move(f)); }
    void SyncOperation(std::function<void()>&& f) { callbacks.push_back(std::move(f)); }
};
struct FenceManager : GenericFenceManager {
    std::atomic<u64> pending_guest_fences{},pending_guest_callbacks{};
    void SignalFence(std::function<void()>&&);
    void SyncOperation(std::function<void()>&&);
};
"""
            + methods
            + r"""
int main() {
    FenceManager f;
    int writes=0;
    f.SyncOperation([&] { assert(f.pending_guest_callbacks==1); ++writes; });
    f.SignalFence([&] { assert(f.pending_guest_fences==1); ++writes; });
    assert(writes==0 && f.pending_guest_callbacks==1 && f.pending_guest_fences==1);
    f.callbacks[0]();
    f.callbacks[1]();
    assert(writes==2 && f.pending_guest_callbacks==0 && f.pending_guest_fences==0);
}
"""
        )

    def test_combo_and_foreground_present_are_diagnosable(self):
        source = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        self.assertIn('Diagnostic("HOME_COMBO action=Prompt")', source)
        self.assertIn('Diagnostic("HOME_COMBO action=GoHome")', source)
        source = (ROOT / "src/eden_uwp/ui/renderer.cpp").read_text()
        self.assertIn("SESSION_OVERLAY_CREATE foreground=", source)
        self.assertIn("SESSION_OVERLAY_PRESENT complete", source)


if __name__ == "__main__":
    unittest.main()
