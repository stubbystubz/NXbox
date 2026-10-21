// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/nxbox_wait_diag.h"
#include "video_core/renderer_d3d12/d3d12_context.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <d3d12sdklayers.h>
#include <fmt/format.h>
#include <windows.h>
#include "eden_uwp/renderer_boundary.h"
#ifdef NXBOX_UWP
#include <winrt/Windows.System.h>
#include "eden_uwp/crash_logger.h"
#include "eden_uwp/diagnostic.h"
#endif
#include "eden_uwp/thread_profile.h"
#ifndef NXBOX_UWP
#include <dxgi1_4.h>
#endif
#include "common/logging.h"
#include "common/nxbox_sim_series_s.h"
#include "common/thread.h"
#include "video_core/renderer_d3d12/d3d12_completion.h"
#include "video_core/renderer_d3d12/d3d12_descriptor.h"
#include "video_core/renderer_d3d12/d3d12_footprint.h"
#include "video_core/renderer_d3d12/d3d12_formats.h"
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"
#include "video_core/renderer_d3d12/d3d12_sim_series_s.h"

namespace D3D12 {
// Second list and its allocator ring are exclusively mutated by the second recorder.
// The first recorder reads the closed list only after the worker completion acquire.
struct ParallelNativeState {
    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;
        u64 tick{};
    };
    std::vector<Slot> slots;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12GraphicsCommandList1> list1;
    ComPtr<ID3D12GraphicsCommandList8> list8;
    ID3D12Device* device{};
    ID3D12Fence* fence{};
    RecordingTarget target;
    bool submit_second{}; // First recorder only; never read by the encoder.
    void Initialize(ID3D12Device* value, ID3D12Fence* completion, size_t count) {
        Common::SetCurrentThreadName("D3D12 parallel recorder");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        device = value;
        fence = completion;
        slots.resize(count);
        for (auto& slot : slots)
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&slot.allocator)),
                  "parallel allocator", device);
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots[0].allocator.Get(),
                                        nullptr, IID_PPV_ARGS(&list)),
              "parallel list", device);
        Check(list->Close(), "parallel initial Close", device);
        list.As(&list1);
        list.As(&list8);
        target.list = list.Get();
        target.list1 = list1.Get();
        target.list8 = list8.Get();
    }
    void Begin(u64 tick) {
        auto& slot = slots[(tick - 1) % slots.size()];
        const auto completed = fence->GetCompletedValue();
        Check(device->GetDeviceRemovedReason(), "parallel allocator completion", device);
        if (completed < slot.tick) {
            const HANDLE event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            if (!event)
                Check(HRESULT_FROM_WIN32(GetLastError()), "parallel event", device);
            const auto status = fence->SetEventOnCompletion(slot.tick, event);
            if (FAILED(status)) {
                CloseHandle(event);
                Check(status, "parallel allocator fence", device);
            }
            const auto wait = WaitForSingleObjectEx(event, 10000, FALSE);
            const auto error = GetLastError();
            CloseHandle(event);
            if (wait != WAIT_OBJECT_0)
                Check(wait == WAIT_FAILED ? HRESULT_FROM_WIN32(error)
                                          : HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                      "parallel allocator wait", device);
        }
        Check(slot.allocator->Reset(), "parallel allocator Reset", device);
        Check(list->Reset(slot.allocator.Get(), nullptr), "parallel list Reset", device);
        slot.tick = tick;
    }
};

bool BisectEnabled(const char* name) noexcept {
    // Called several times per draw with string literals; reading the process environment each
    // time showed up in the GPU thread's samples. No toggle changes after startup.
    struct Cached {
        const char* name;
        bool value;
    };
    thread_local std::array<Cached, 128> cache{};
    thread_local size_t used = 0;
    for (size_t i = 0; i < used; ++i)
        if (cache[i].name == name)
            return cache[i].value;
    char value[2]{};
    const bool enabled = GetEnvironmentVariableA(name, value, sizeof(value)) == 1 && value[0] == '1';
    if (used < cache.size())
        cache[used++] = {name, enabled};
    return enabled;
}
bool StateFastpathEnabled() noexcept {
    static const bool enabled = [] {
        char value[2]{};
        return !(GetEnvironmentVariableA("NXBOX_D3D12_STATE_FASTPATH", value, sizeof(value)) == 1 &&
                 value[0] == '0');
    }();
    return enabled;
}
// NXBOX_D3D12_WAIT_SPIN=0 blocks on the fence event at once, as before the bounded spin.
static bool WaitSpinEnabled() noexcept {
    static const bool enabled = [] {
        char value[2]{};
        return !(GetEnvironmentVariableA("NXBOX_D3D12_WAIT_SPIN", value, sizeof(value)) == 1 &&
                 value[0] == '0');
    }();
    return enabled;
}
bool StateVerifyEnabled() noexcept {
    static const bool enabled = BisectEnabled("NXBOX_D3D12_STATE_VERIFY");
    return enabled;
}
void StateVerifyMismatch(std::string_view what, const void* fast, const void* slow, size_t size) {
    struct Mismatch {
        std::string_view what;
        u64 fast{}, slow{};
        bool operator==(const Mismatch&) const = default;
    };
    static std::array<Mismatch, 32> reported{};
    static size_t count{};
    const Mismatch mismatch{what, Pso::HashBytes(fast, size), Pso::HashBytes(slow, size)};
    if (count == reported.size() ||
        std::find(reported.begin(), reported.begin() + count, mismatch) != reported.begin() + count)
        return;
    reported[count++] = mismatch;
    Diagnostic(fmt::format("D3D12_STATE_VERIFY mismatch what={} fast={:016x} slow={:016x}", what,
                           mismatch.fast, mismatch.slow));
}
std::uint32_t FlushDrawsKnob() noexcept {
    char value[16]{};
    const auto length = GetEnvironmentVariableA("NXBOX_D3D12_FLUSH_DRAWS", value, sizeof(value));
    if (length == 0)
        return DefaultFlushDraws;
    // A return that does not fit means too many characters for a 32-bit number.
    return length < sizeof(value) ? ParseFlushDraws(value) : 0;
}
bool DxcStrictEnabled() noexcept {
    static const bool enabled = BisectEnabled("NXBOX_D3D12_DXC_STRICT");
    return enabled;
}
bool AsyncPsoEnabled() noexcept {
    static const bool enabled = [] {
        char value[2]{};
        if (GetEnvironmentVariableA("NXBOX_D3D12_ASYNC_PSO", value, sizeof(value)) == 1)
            return value[0] != '0';
#ifdef NXBOX_UWP
        return true;
#else
        return false;
#endif
    }();
    return enabled;
}
void Diagnostic(std::string_view message) noexcept {
    try {
        LOG_ERROR(Render, "{}", message);
#ifdef NXBOX_UWP
        EdenXbox::Diagnostic(std::string{message});
        const std::filesystem::path folder{
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()};
        static std::mutex mutex;
        const std::lock_guard lock{mutex};
        const HANDLE file =
            CreateFile2FromAppW((folder / "nxbox_crash.txt").c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const std::string line = std::string{message} + "\n";
            OVERLAPPED append{};
            append.Offset = append.OffsetHigh = MAXDWORD;
            DWORD written{};
            // No FlushFileBuffers: this runs on render threads, and a written line already
            // survives a process crash; only a console power loss would drop it.
            WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, &append);
            CloseHandle(file);
        }
#endif
    } catch (...) {
        OutputDebugStringA("D3D12 diagnostic unavailable\n");
    }
}
namespace {
// Debug-layer warnings repeat per draw. Logging each one synchronously flushes
// the crash file and can dominate the GPU thread. Errors remain unsuppressed.
bool FirstInfoWarning(D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id) {
    if (severity != D3D12_MESSAGE_SEVERITY_WARNING)
        return true;
    static std::array<std::atomic<bool>, 8192> seen{};
    const auto index = static_cast<size_t>(id);
    return index >= seen.size() || !seen[index].exchange(true, std::memory_order_relaxed);
}
u64 ProfileCpuTime() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
        return DefaultFlushDraws;
    const auto value = [](FILETIME time) {
        return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return value(kernel) + value(user); // 100 ns, kernel + user CPU time.
}
u64 ProfileWallTime() {
    static const double ticks_to_100ns = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return 10000000.0 / static_cast<double>(frequency.QuadPart);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<u64>(static_cast<double>(now.QuadPart) * ticks_to_100ns);
}
void InfoMessage(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
                 D3D12_MESSAGE_ID id, LPCSTR description, void*) noexcept {
    // The callback can run on runtime threads; never call D3D APIs here.
    // Resource creation/destruction and runtime probes are informational traffic.
    if (severity > D3D12_MESSAGE_SEVERITY_WARNING || !FirstInfoWarning(severity, id))
        return;
    try {
#ifdef NXBOX_UWP
        // No renderer call imports host-address heaps. Capture the runtime's caller once,
        // including module offsets, so console evidence can identify an internal probe.
        static std::atomic_flag heap_probe_captured = ATOMIC_FLAG_INIT;
        if (static_cast<u32>(id) == 1086 && !heap_probe_captured.test_and_set()) {
            void* frames[24]{};
            const auto count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
            for (USHORT i = 0; i < count; ++i) {
                HMODULE module{};
                char name[256]{};
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(frames[i]), &module);
                if (module)
                    GetModuleFileNameA(module, name, sizeof(name));
                Diagnostic(fmt::format("D3D12 heap probe frame={} module={} offset={:#x} pc={}", i,
                                       name,
                                       reinterpret_cast<std::uintptr_t>(frames[i]) -
                                           reinterpret_cast<std::uintptr_t>(module),
                                       frames[i]));
            }
        }
#endif
        Diagnostic(fmt::format("D3D12 InfoQueue severity={} id={} category={} {}",
                               static_cast<u32>(severity), static_cast<u32>(id),
                               static_cast<u32>(category), description ? description : ""));
    } catch (...) {
        Diagnostic("D3D12 InfoQueue callback logging failed");
    }
}
std::string Narrow(const wchar_t* value) {
    if (!value)
        return "unnamed";
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return "unavailable";
    std::string text(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), size, nullptr, nullptr);
    text.pop_back();
    return text;
}
std::mutex& InfoQueueMutex() {
    static std::mutex mutex;
    return mutex;
}
void ClearInfoQueue(ID3D12InfoQueue* queue) {
    std::lock_guard lock{InfoQueueMutex()};
    queue->ClearStoredMessages();
}
void LogDeviceInfoQueue(ID3D12Device* device) noexcept {
    try {
        if (!device)
            return;
        static const bool debug = BisectEnabled("NXBOX_D3D12_DEBUG");
        if (!debug)
            return;
        std::lock_guard lock{InfoQueueMutex()};
        ComPtr<ID3D12InfoQueue> info;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info))))
            return;
        const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size{};
            if (FAILED(info->GetMessage(i, nullptr, &size)))
                continue;
            std::vector<u8> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &size)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING &&
                FirstInfoWarning(message->Severity, message->ID))
                Diagnostic(fmt::format("D3D12 InfoQueue severity={} id={} {}",
                                       static_cast<u32>(message->Severity),
                                       static_cast<u32>(message->ID), message->pDescription));
        }
        info->ClearStoredMessages();
    } catch (...) {
        Diagnostic("D3D12 InfoQueue logging failed");
    }
}
template <typename Node>
void LogBreadcrumbs(HRESULT result, const Node* head) {
    Diagnostic(fmt::format("D3D12 DRED breadcrumbs HRESULT={:#010x}", static_cast<u32>(result)));
    for (auto* node = head; SUCCEEDED(result) && node; node = node->pNext) {
        const auto last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        Diagnostic(
            fmt::format("D3D12 DRED list={} queue={} completed={}/{}",
                        node->pCommandListDebugNameA ? std::string{node->pCommandListDebugNameA}
                                                     : Narrow(node->pCommandListDebugNameW),
                        node->pCommandQueueDebugNameA ? std::string{node->pCommandQueueDebugNameA}
                                                      : Narrow(node->pCommandQueueDebugNameW),
                        last, node->BreadcrumbCount));
        if constexpr (requires { node->pBreadcrumbContexts; }) {
            for (UINT i = 0; node->pBreadcrumbContexts && i < node->BreadcrumbContextsCount; ++i) {
                const auto& context = node->pBreadcrumbContexts[i];
                Diagnostic(fmt::format("D3D12 DRED context op[{}]={}", context.BreadcrumbIndex,
                                       Narrow(context.pContextString)));
            }
        }
        if (node->pCommandHistory && last && last <= node->BreadcrumbCount)
            Diagnostic(fmt::format("D3D12 DRED last completed op[{}]={}", last - 1,
                                   static_cast<u32>(node->pCommandHistory[last - 1])));
        if (node->pCommandHistory) {
            const UINT begin = last > 8 ? last - 8 : 0;
            const UINT end = last < node->BreadcrumbCount
                                 ? last + std::min(8u, node->BreadcrumbCount - last)
                                 : node->BreadcrumbCount;
            for (UINT i = begin; i < end; ++i)
                Diagnostic(fmt::format("D3D12 DRED op[{}]={}", i,
                                       static_cast<u32>(node->pCommandHistory[i])));
        }
    }
}
void LogDeviceDred(ID3D12Device* device) noexcept {
    try {
        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        const auto status = device ? device->QueryInterface(IID_PPV_ARGS(&dred)) : E_POINTER;
        if (FAILED(status)) {
            Diagnostic(
                fmt::format("D3D12 DRED unavailable HRESULT={:#010x}", static_cast<u32>(status)));
            return;
        }
        ComPtr<ID3D12DeviceRemovedExtendedData1> extended;
        const auto context_status = device->QueryInterface(IID_PPV_ARGS(&extended));
        if (SUCCEEDED(context_status)) {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
            const auto result = extended->GetAutoBreadcrumbsOutput1(&breadcrumbs);
            LogBreadcrumbs(result, breadcrumbs.pHeadAutoBreadcrumbNode);
        } else {
            Diagnostic(fmt::format("D3D12 DRED contexts unavailable HRESULT={:#010x}",
                                   static_cast<u32>(context_status)));
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
            const auto result = dred->GetAutoBreadcrumbsOutput(&breadcrumbs);
            LogBreadcrumbs(result, breadcrumbs.pHeadAutoBreadcrumbNode);
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
        const auto fault_status = dred->GetPageFaultAllocationOutput(&fault);
        Diagnostic(fmt::format("D3D12 DRED page fault HRESULT={:#010x} address={:#x}",
                               static_cast<u32>(fault_status), fault.PageFaultVA));
        if (SUCCEEDED(fault_status)) {
            const auto allocations = [](const char* kind, const D3D12_DRED_ALLOCATION_NODE* node) {
                for (; node; node = node->pNext)
                    Diagnostic(fmt::format("D3D12 DRED {} allocation={} type={}", kind,
                                           node->ObjectNameA ? std::string{node->ObjectNameA}
                                                             : Narrow(node->ObjectNameW),
                                           static_cast<u32>(node->AllocationType)));
            };
            allocations("existing", fault.pHeadExistingAllocationNode);
            allocations("freed", fault.pHeadRecentFreedAllocationNode);
        }
    } catch (...) {
        Diagnostic("D3D12 DRED logging failed");
    }
}
} // namespace
void TraceApiCall(ID3D12Device* device, const char* call, ID3D12Resource* resource,
                  const void* desc, size_t desc_size) noexcept {
    static std::atomic<bool> reported{false};
    if (!device || !BisectEnabled("NXBOX_D3D12_SUBMIT_TRACE") || reported.load(std::memory_order_relaxed))
        return;
    const HRESULT removed = device->GetDeviceRemovedReason();
    if (SUCCEEDED(removed) || reported.exchange(true))
        return;
    try {
        std::string args;
        const auto* bytes = static_cast<const u8*>(desc);
        for (size_t i = 0; i < desc_size; i += 4) {
            u32 word = 0;
            std::memcpy(&word, bytes + i, std::min<size_t>(4, desc_size - i));
            args += fmt::format("{}{:08x}", i ? " " : "", word);
        }
        std::string target = "null";
        if (resource) {
            const auto r = resource->GetDesc();
            target = fmt::format("dim={} {}x{}x{} mips={} format={} samples={} flags={:#x}",
                                 static_cast<int>(r.Dimension), r.Width, r.Height,
                                 r.DepthOrArraySize, r.MipLevels, static_cast<int>(r.Format),
                                 r.SampleDesc.Count, static_cast<u32>(r.Flags));
        }
        Diagnostic(fmt::format("D3D12_API_TRACE removed={:#010x} after={} resource=[{}] desc=[{}]",
                               static_cast<u32>(removed), call, target, args));
    } catch (...) {
        Diagnostic("D3D12_API_TRACE removed after a void call (details unavailable)");
    }
}
void Check(HRESULT result, const char* operation, ID3D12Device* device) {
    if (FAILED(result)) {
        const auto message = fmt::format(
            "D3D12 {} HRESULT={:#010x} removed={}", operation, static_cast<u32>(result),
            device ? fmt::format("{:#010x}", static_cast<u32>(device->GetDeviceRemovedReason()))
                   : "unavailable");
        Diagnostic(message);
        LogDeviceInfoQueue(device);
        LogDeviceDred(device);
        throw std::runtime_error(message);
    }
}
void CheckPipeline(HRESULT result, const char* operation, ID3D12Device* device, u64 hash) {
    if (FAILED(result))
        Diagnostic(fmt::format("D3D12 PSO hash={:016x} {} HRESULT={:#010x}", hash, operation,
                               static_cast<u32>(result)));
    Check(result, operation, device);
}
Resource::~Resource() {
    if (!mapped.empty())
        handle->Unmap(0, nullptr);
    if (budget)
        budget->used -= allocation_size;
}
Common::GpuProfile::Tick FrameProfile::ReadTicks() {
    const auto index = static_cast<size_t>(FramePhase::Maxwell3DMethods);
    return method_clock_base + timing.cpu[index] +
           (timing.active == FramePhase::Maxwell3DMethods ? ProfileWallTime() - timing.last : 0);
}
bool FrameProfile::OnOwner() {
    return enabled && owner.load() == GetCurrentThreadId();
}
void FrameProfile::BindRecordingThread(bool rebind) {
    if (enabled && rebind && owner.load() != GetCurrentThreadId()) {
        owner = 0;
        timing.active = FramePhase::BetweenDraws;
        if (method_enabled) {
            method_clock_base += timing.cpu[static_cast<size_t>(FramePhase::Maxwell3DMethods)];
            samples.fill({});
        }
        timing.cpu.fill(0);
        counts.fill(0);
        transitions.clear();
        waits.clear();
        frames = fence_wall = 0;
    }
    if (enabled && !owner.load()) {
        initial_cpu = ProfileCpuTime();
        start_wall = ProfileWallTime();
        timing.Reset(start_wall);
        owner.store(GetCurrentThreadId());
    }
}
std::size_t FrameProfile::Enter(Common::GpuProfile::Phase phase) {
    constexpr std::array phases{FramePhase::DmaPusher,
                                FramePhase::Maxwell3DMethods,
                                FramePhase::MacroExecution,
                                FramePhase::DmaEngine,
                                FramePhase::InlineToMemory,
                                FramePhase::ComputeDispatchSetup,
                                FramePhase::Fermi2D,
                                FramePhase::SyncWait,
                                FramePhase::BufferCacheBind,
                                FramePhase::TextureCacheUpdate,
                                FramePhase::DescriptorAndPipeline,
                                FramePhase::QueueIdle};
    const auto target = phases[static_cast<std::size_t>(phase)];
    const auto previous = timing.active;
    // Nested method/bulk dispatch often enters the already active phase. Preserve
    // the outer boundary without two redundant clock reads for each inner scope.
    if (previous == target)
        return static_cast<std::size_t>(FramePhase::Count);
    Switch(target);
    return static_cast<std::size_t>(previous);
}
void FrameProfile::Leave(std::size_t previous) {
    if (previous != static_cast<std::size_t>(FramePhase::Count))
        Switch(static_cast<FramePhase>(previous));
}
u64 FrameProfile::TakeRecorderCpu() {
    if (!recorder_thread)
        return 0;
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(recorder_thread, &creation, &exit, &kernel, &user))
        return 0;
    const auto value = [](FILETIME t) {
        return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    const auto current = value(kernel) + value(user);
    return current - std::exchange(recorder_initial_cpu, current);
}
void FrameProfile::Switch(FramePhase phase) {
    // Wall time: GetThreadTimes ticks at 15.6 ms and costs a syscall per phase switch.
    const auto now = ProfileWallTime();
    timing.Switch(phase, now);
}
FrameProfile::Scope::Scope(FrameProfile& profile_, FramePhase phase) {
    if (!profile_.OnOwner())
        return;
    profile = &profile_;
    previous = profile->timing.active;
    profile->Switch(phase);
    wait = phase == FramePhase::FenceWait;
    if (wait)
        wall_start = ProfileWallTime();
}
FrameProfile::Scope::~Scope() {
    if (!profile)
        return;
    if (wait)
        profile->fence_wall += ProfileWallTime() - wall_start;
    profile->Switch(previous);
}
void FrameProfile::Count(FrameCount counter, u64 amount) {
    if (!OnOwner())
        return;
    counts[static_cast<size_t>(counter)] += amount;
    if (!amount)
        return;
    // Count at the category byte event, including callers outside the buffer cache.
    // One event is one logical upload/snapshot, not one D3D12 copy command.
    switch (counter) {
    case FrameCount::VertexUploadBytes:
        ++counts[static_cast<size_t>(FrameCount::VertexUploadCount)];
        break;
    case FrameCount::IndexUploadBytes:
        ++counts[static_cast<size_t>(FrameCount::IndexUploadCount)];
        break;
    case FrameCount::UniformUploadBytes:
        ++counts[static_cast<size_t>(FrameCount::UniformUploadCount)];
        break;
    case FrameCount::StorageUploadBytes:
        ++counts[static_cast<size_t>(FrameCount::StorageUploadCount)];
        break;
    case FrameCount::TexelUploadBytes:
        ++counts[static_cast<size_t>(FrameCount::TexelUploadCount)];
        break;
    case FrameCount::OtherUploadBytes:
        ++counts[static_cast<size_t>(FrameCount::OtherUploadCount)];
        break;
    default:
        break;
    }
}
void FrameProfile::NoteTransition(u32 before, u32 after, bool buffer) {
    if (enabled && OnOwner())
        ++transitions[(u64{buffer} << 63) | (u64{before} << 32) | after];
}
void FrameProfile::NoteWait(std::string_view site, u64 wall) {
    if (enabled && OnOwner()) {
        auto& entry = waits[site];
        ++entry.first;
        entry.second += wall;
    }
}
void FrameProfile::EndFrame(bool presented) {
    if (!enabled)
        return;
    // Renderer initialization can run on the UI thread; runtime frame recording
    // belongs to the GPU thread. Fencing-thread scopes never claim this ownership.
    if (owner != GetCurrentThreadId()) {
        owner = 0;
        if (method_enabled) {
            method_clock_base += timing.cpu[static_cast<size_t>(FramePhase::Maxwell3DMethods)];
            samples.fill({});
        }
        timing.cpu.fill(0);
        counts.fill(0);
        frames = fence_wall = 0;
    }
    BindRecordingThread();
    Switch(timing.active);
    frames += presented ? 1 : 0;
    const auto wall = ProfileWallTime();
    if (wall - start_wall < 50000000)
        return;
    try {
        constexpr std::array names{"draw_setup",
                                   "descriptor_writes",
                                   "barriers",
                                   "pso",
                                   "buffer_upload",
                                   "texture_upload",
                                   "submit",
                                   "fence_wait",
                                   "pipeline_lookup",
                                   "shader_translation",
                                   "dxc_compile",
                                   "extent_copies",
                                   "feedback_copies",
                                   "index_conversion",
                                   "buffer_cache",
                                   "texture_cache",
                                   "buffer_download",
                                   "texture_download",
                                   "draw_dispatch",
                                   "draw_memory",
                                   "graphics_descriptors",
                                   "render_targets",
                                   "attachment_setup",
                                   "query_state",
                                   "graphics_state",
                                   "resource_bindings",
                                   "image_views",
                                   "graphics_buffers",
                                   "geometry_buffers",
                                   "stage_buffers",
                                   "buffer_validation",
                                   "feedback_setup",
                                   "geometry_setup",
                                   "vertex_copies",
                                   "descriptor_setup",
                                   "draw_recording",
                                   "draw_cleanup",
                                   "dma_pusher",
                                   "maxwell3d_methods",
                                   "macro_execution",
                                   "dma_engine",
                                   "inline_to_memory",
                                   "compute_dispatch_setup",
                                   "fermi2d",
                                   "sync_wait",
                                   "buffer_cache_bind",
                                   "texture_cache_update",
                                   "descriptor_and_pipeline",
                                   "queue_idle",
                                   "texture_upload_diagnostic",
                                   "texture_repack",
                                   "between_draws",
                                   "index_read",
                                   "index_hash",
                                   "index_flush_caching",
                                   "index_coherence",
                                   "index_gpu_modified",
                                   "index_restart_scan",
                                   "upload_allocation",
                                   "upload_transitions",
                                   "upload_recording",
                                   "staging_collect",
                                   "uniform_copy_lookup"};
        constexpr std::array counters{"draws",
                                      "dispatches",
                                      "barriers",
                                      "descriptor_copies",
                                      "descriptor_table_reuses",
                                      "descriptor_writes",
                                      "root_cbv_binds",
                                      "descriptor_copy_calls",
                                      "descriptor_writes_cbv",
                                      "descriptor_writes_srv",
                                      "descriptor_writes_uav",
                                      "descriptor_writes_sampler",
                                      "pso_compiles",
                                      "ExecuteCommandLists",
                                      "pso_cache_misses",
                                      "prewarmed_pso_hits",
                                      "guest_pipeline_misses",
                                      "native_index_draws",
                                      "shader_translations",
                                      "dxc_compiles",
                                      "dxc_cache_misses",
                                      "negative_cache_hits",
                                      "draws_skipped_pending_pso",
                                      "dispatches_skipped_pending_pso",
                                      "extent_copies",
                                      "feedback_copies",
                                      "index_conversions",
                                      "index_conversion_bytes",
                                      "index_conversion_uint8",
                                      "index_conversion_restart",
                                      "index_conversion_strip_or_fan",
                                      "index_conversion_provoking",
                                      "index_conversion_inline",
                                      "index_conversion_cache_hits",
                                      "buffer_upload_bytes",
                                      "buffer_download_bytes",
                                      "skipped_draws",
                                      "forced_submits_memory_pressure",
                                      "forced_submits_allocation_retry",
                                      "forced_submits_staging_pressure",
                                      "forced_submits_descriptor_exhaustion",
                                      "forced_submits_query_bank",
                                      "forced_submits_query_readback",
                                      "forced_submits_buffer_download",
                                      "forced_submits_texture_download",
                                      "forced_submits_skipped_draw",
                                      "forced_submits_guest_flush",
                                      "forced_submits_guest_fence",
                                      "forced_submits_other",
                                      "vertex_upload_bytes",
                                      "index_upload_bytes",
                                      "uniform_upload_bytes",
                                      "storage_upload_bytes",
                                      "texel_upload_bytes",
                                      "other_upload_bytes",
                                      "texture_upload_bytes",
                                      "staging_copy_bytes",
                                      "gpu_buffer_copy_bytes",
                                      "uniform_copy_cache_hits",
                                      "vertex_repack_bytes",
                                      "vertex_padding_bytes",
                                      "forced_submits_fence_wait",
                                      "forced_submits_wait_for_idle",
                                      "forced_submits_flush_all",
                                      "forced_submits_buffer_clear",
                                      "forced_submits_lifecycle",
                                      "forced_submits_present",
                                      "redundant_state_skips",
                                      "copy_hazard_barriers",
                                      "feedback_disjoint",
                                      "feedback_avoided",
                                      "feedback_copy_bytes",
                                      "feedback_reused",
                                      "gpu_resolved_query_reports",
                                      "query_report_fence_waits",
                                      "index_validated_cache_hits",
                                      "index_native_restart_draws",
                                      "index_read_bytes",
                                      "index_downloads",
                                      "index_download_bytes",
                                      "staging_requests",
                                      "staging_fast_requests",
                                      "staging_collections",
                                      "upload_copy_commands",
                                      "vertex_upload_count",
                                      "index_upload_count",
                                      "uniform_upload_count",
                                      "storage_upload_count",
                                      "texel_upload_count",
                                      "other_upload_count",
                                      "uniform_copy_cache_misses",
                                      "fermi_blits_accelerated",
                                      "fermi_blits_software",
                                      "fermi_blit_accelerated_bytes",
                                      "fermi_blit_software_bytes",
                                      "texture_direct_upload_bytes",
                                      "texture_repack_bytes",
                                      "texture_refresh_bytes",
                                      "fermi_discard_upload_bytes",
                                      "staging_new_resources",
                                      "uniform_snapshot_requests",
                                      "uniform_snapshot_new_resources",
                                      "forced_submits_flush_draws",
                                      "attachment_metadata_reuses",
                                      "query_batch_dispatches",
                                      "gpu_depth_reinterpret_bytes",
                                      "cpu_reinterpret_bytes"};
        static_assert(names.size() == static_cast<size_t>(FramePhase::Count));
        static_assert(counters.size() == static_cast<size_t>(FrameCount::Count));
        const double elapsed = static_cast<double>(wall - start_wall);
        std::string line = fmt::format(
            "D3D12_FRAME_PROFILE thread={} frames={} gpu_thread_cpu_pct={:.2f} "
            "fence_wait_wall_ms={:.3f} gpu_busy_ms={:.3f} recorder_busy_ms_per_frame={:.3f} "
            "recorder_backpressure_waits={} recorder_backpressure_ms_per_frame={:.3f}",
            owner.load(), frames,
            100.0 * static_cast<double>(ProfileCpuTime() - initial_cpu) / elapsed,
            static_cast<double>(fence_wall) / 10000.0,
            static_cast<double>(gpu_busy.exchange(0)) / 10000.0,
            static_cast<double>(TakeRecorderCpu()) / (10000.0 * std::max<u64>(frames, 1)),
            std::exchange(recorder_waits, 0),
            static_cast<double>(std::exchange(recorder_wait_ns, 0)) /
                (1000000.0 * std::max<u64>(frames, 1)));
        // Queue waits use the same exclusive clock, but must not rank as CPU work.
        line += fmt::format(
            " gpu_queue_idle_wall_ms={:.3f}",
            static_cast<double>(timing.cpu[static_cast<size_t>(FramePhase::QueueIdle)]) / 10000.0);
        for (size_t i = 0; i < timing.cpu.size(); ++i) {
            if (i == static_cast<size_t>(FramePhase::QueueIdle))
                continue;
            line += fmt::format(" {}_cpu_ms={:.3f}", names[i],
                                static_cast<double>(timing.cpu[i]) / 10000.0);
        }
        for (size_t i = 0; i < counts.size(); ++i)
            line += fmt::format(
                " {}={} {}_per_frame={:.2f}", counters[i], counts[i], counters[i],
                frames ? static_cast<double>(counts[i]) / static_cast<double>(frames) : 0.0);
        constexpr std::array upload_categories{
            std::pair{FrameCount::VertexUploadBytes, FrameCount::VertexUploadCount},
            std::pair{FrameCount::IndexUploadBytes, FrameCount::IndexUploadCount},
            std::pair{FrameCount::UniformUploadBytes, FrameCount::UniformUploadCount},
            std::pair{FrameCount::StorageUploadBytes, FrameCount::StorageUploadCount},
            std::pair{FrameCount::TexelUploadBytes, FrameCount::TexelUploadCount},
            std::pair{FrameCount::OtherUploadBytes, FrameCount::OtherUploadCount},
        };
        for (const auto& [bytes_counter, count_counter] : upload_categories) {
            const auto bytes_index = static_cast<size_t>(bytes_counter);
            const auto count = counts[static_cast<size_t>(count_counter)];
            line += fmt::format(" {}_avg={:.2f}", counters[bytes_index],
                                count ? static_cast<double>(counts[bytes_index]) / count : 0.0);
        }
        std::vector<std::pair<u64, u64>> top(transitions.begin(), transitions.end());
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        line += " transitions_top=";
        for (size_t i = 0; i < std::min<size_t>(top.size(), 12); ++i)
            line += fmt::format("{}{:#x}>{:#x}:{}", top[i].first >> 63 ? "b" : "t",
                                (top[i].first >> 32) & 0x7fffffff, top[i].first & 0xffffffff,
                                top[i].second) + ",";
        line += " waits_top=";
        for (const auto& [site, entry] : waits)
            line += fmt::format("{}:{}:{:.3f},", site, entry.first,
                                static_cast<double>(entry.second) / 10000.0);
        for (const auto& [site, entry] : waits)
            line +=
                fmt::format(" wait_{}_per_frame={:.2f} wait_{}_ms_per_frame={:.3f}", site,
                            frames ? static_cast<double>(entry.first) / frames : 0.0, site,
                            frames ? static_cast<double>(entry.second) / (10000.0 * frames) : 0.0);
        if (guest_visibility)
            line += guest_visibility();
        if (method_enabled) {
            using namespace Common::MaxwellMethodProfile;
            std::string method_line = "NXBOX METHOD_PROFILE";
            const double divisor = frames ? static_cast<double>(frames) : 1.0;
            const auto append = [&](std::string_view name, const Sample& sample) {
                method_line +=
                    fmt::format(" {}={:.6f},{:.3f}", name, sample.ticks / (10000.0 * divisor),
                                sample.count / divisor);
            };
            for (unsigned i = 0; i < Other; ++i)
                append(Names[i], samples[i]);
            Sample other{};
            std::array<unsigned, RegisterCount> indices{};
            for (unsigned i = 0; i < RegisterCount; ++i) {
                indices[i] = i;
                other.ticks += samples[ClassCount + i].ticks;
                other.count += samples[ClassCount + i].count;
            }
            append("other", other);
            std::partial_sort(indices.begin(), indices.begin() + 16, indices.end(),
                              [&](unsigned a, unsigned b) {
                                  const auto& x = samples[ClassCount + a];
                                  const auto& y = samples[ClassCount + b];
                                  if (x.ticks != y.ticks)
                                      return x.ticks > y.ticks;
                                  if (x.count != y.count)
                                      return x.count > y.count;
                                  return a < b;
                              });
            for (unsigned i = 0; i < 16; ++i) {
                const auto index = indices[i];
                const auto& sample = samples[ClassCount + index];
                if (sample.count)
                    append(fmt::format("other_0x{:03x}", index), sample);
            }
            method_line += fmt::format(" frames={}", frames);
            Diagnostic(method_line);
        }
        Diagnostic(line);
        const auto guest = Common::GpuProfile::TakeWindow();
        std::string guest_line = "D3D12_GUEST_WAITS";
        guest_line += fmt::format(" frames={}", frames);
        for (size_t i = 0; i < guest.waits.size(); ++i) {
            const auto& sample = guest.waits[i];
            guest_line += fmt::format(" {}_count={} {}_total_ms={:.3f} {}_max_ms={:.3f}",
                                      Common::GpuProfile::WaitNames[i], sample.count,
                                      Common::GpuProfile::WaitNames[i], sample.total_ns / 1.0e6,
                                      Common::GpuProfile::WaitNames[i], sample.max_ns / 1.0e6);
            guest_line += fmt::format(" {}_per_frame={:.2f} {}_ms_per_frame={:.3f}",
                                      Common::GpuProfile::WaitNames[i],
                                      frames ? static_cast<double>(sample.count) / frames : 0.0,
                                      Common::GpuProfile::WaitNames[i],
                                      frames ? sample.total_ns / (1.0e6 * frames) : 0.0);
        }
        guest_line += fmt::format(
            " fence_samples={} fence_queued_avg_ms={:.3f} fence_gpu_execution_avg_ms={:.3f}"
            " fence_total_avg_ms={:.3f} dropped_fence_requests={} pending_fence_requests={}",
            guest.fence_total.count,
            guest.fence_total.count
                ? guest.fence_queued.total_ns / (1.0e6 * guest.fence_total.count)
                : 0.0,
            guest.fence_total.count
                ? guest.fence_execution.total_ns / (1.0e6 * guest.fence_total.count)
                : 0.0,
            guest.fence_total.count ? guest.fence_total.total_ns / (1.0e6 * guest.fence_total.count)
                                    : 0.0,
            guest.dropped_requests, guest.pending_requests);
        Diagnostic(guest_line);
        const auto methods = Common::GpuProfile::TakeMethodWindow();
        std::string method_line = "D3D12_MAXWELL_METHODS timing_sample_stride=64";
        method_line += Common::GpuProfile::MethodProfilingEnabled() ? " enabled=1" : " enabled=0";
        for (size_t i = 0; i < methods.size(); ++i) {
            const auto& sample = methods[i];
            const auto name = Common::GpuProfile::MethodNames[i];
            const double estimated_ns = sample.timed_calls
                                            ? static_cast<double>(sample.sampled_ns) *
                                                  static_cast<double>(sample.count) /
                                                  static_cast<double>(sample.timed_calls)
                                            : 0.0;
            method_line += fmt::format(
                " {}_count={} {}_units={} {}_skipped={} {}_timed_calls={} {}_estimated_ms={:.3f}"
                " {}_estimated_ms_per_frame={:.3f}",
                name, sample.count, name, sample.units, name, sample.skipped, name,
                sample.timed_calls, name, estimated_ns / 1.0e6, name,
                frames ? estimated_ns / (1.0e6 * frames) : 0.0);
        }
        Diagnostic(method_line);
    } catch (...) {
        Diagnostic("D3D12 frame profile formatting failed");
    }
    if (method_enabled) {
        method_clock_base += timing.cpu[static_cast<size_t>(FramePhase::Maxwell3DMethods)];
        samples.fill({});
    }
    timing.cpu.fill(0);
    counts.fill(0);
    transitions.clear();
    waits.clear();
    frames = fence_wall = 0;
    initial_cpu = ProfileCpuTime();
    start_wall = ProfileWallTime();
    timing.Reset(start_wall);
}
Context::Context() {
    Diagnostic(fmt::format("D3D12 state fastpath={} verify={}", StateFastpathEnabled() ? 1 : 0,
                           StateVerifyEnabled() ? 1 : 0));
    Diagnostic(fmt::format("D3D12 wait spin={}", WaitSpinEnabled() ? 1 : 0));
    SetEnvironmentVariableA("NXBOX_D3D12_COMMAND_FAILURE", nullptr);
    SetEnvironmentVariableA("NXBOX_D3D12_FIRST_FAILURE", nullptr);
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
    const auto dred_status = D3D12GetDebugInterface(IID_PPV_ARGS(&dred));
    if (SUCCEEDED(dred_status)) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        Diagnostic("D3D12 DRED breadcrumbs/page-fault/context enabled before device creation");
    } else {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> legacy;
        const auto status = D3D12GetDebugInterface(IID_PPV_ARGS(&legacy));
        if (SUCCEEDED(status)) {
            legacy->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            legacy->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
        Diagnostic(fmt::format(
            "D3D12 DRED context settings unavailable HRESULT={:#010x}; legacy HRESULT={:#010x}",
            static_cast<u32>(dred_status), static_cast<u32>(status)));
    }
    char debug[2]{};
    if (GetEnvironmentVariableA("NXBOX_D3D12_DEBUG", debug, sizeof(debug)) == 1 &&
        debug[0] == '1') {
        ComPtr<ID3D12Debug> layer;
        const auto status = D3D12GetDebugInterface(IID_PPV_ARGS(&layer));
        if (SUCCEEDED(status)) {
            layer->EnableDebugLayer();
            Diagnostic("D3D12 debug layer enabled");
        } else {
            Diagnostic(fmt::format("D3D12 debug layer unavailable HRESULT={:#010x}",
                                   static_cast<u32>(status)));
        }
    }
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
          "CreateDevice");
#ifndef NXBOX_UWP
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create memory budget factory");
    Check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)),
          "Find D3D12 adapter");
    DXGI_ADAPTER_DESC2 adapter_desc{};
    Check(adapter->GetDesc2(&adapter_desc), "Get adapter memory");
    device_local_memory = adapter_desc.DedicatedVideoMemory ? adapter_desc.DedicatedVideoMemory
                                                            : adapter_desc.SharedSystemMemory;
#endif
    if (debug[0] == '1') {
        ComPtr<ID3D12InfoQueue> info;
        const auto status = device.As(&info);
        Diagnostic(fmt::format("D3D12 InfoQueue {} HRESULT={:#010x}",
                               SUCCEEDED(status) ? "available" : "unavailable",
                               static_cast<u32>(status)));
    }
    if (debug[0] == '1') {
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device.As(&info))) {
            info->ClearStorageFilter();
            info->ClearRetrievalFilter();
            info->SetMessageCountLimit(UINT64_MAX);
            // Drain creation messages before installing the live callback.
            LogDeviceInfoQueue(device.Get());
        }
        const auto query = device.As(&info_callback.queue);
        const auto status = SUCCEEDED(query)
                                ? info_callback.queue->RegisterMessageCallback(
                                      InfoMessage, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, nullptr,
                                      &info_callback.cookie)
                                : query;
        info_callback.registered = SUCCEEDED(status);
        Diagnostic(fmt::format("D3D12 InfoQueue callback HRESULT={:#010x} mode={}",
                               static_cast<u32>(status),
                               info_callback.registered ? "callback" : "poll"));
    }
    for (const auto* knob :
         {"NXBOX_D3D12_NO_PSO_LIBRARY", "NXBOX_D3D12_GPU_ASTC", "NXBOX_D3D12_NO_COMPUTE",
          "NXBOX_D3D12_NO_DRAWS", "NXBOX_D3D12_NO_QUERIES", "NXBOX_D3D12_SUBMIT_TRACE",
          "NXBOX_D3D12_FENCE_BATCHING", "NXBOX_D3D12_BATCH_DESCRIPTOR_COPIES",
          "NXBOX_D3D12_ASYNC_DMA", "NXBOX_D3D12_SMALL_RECORDER_QUEUE",
          "NXBOX_D3D12_SYNC_BUFFER_CLEARS", "NXBOX_D3D12_VALIDATE_COMMANDS"})
        Diagnostic(fmt::format("D3D12 bisect {}={}", knob, BisectEnabled(knob) ? 1 : 0));
    Diagnostic(fmt::format("D3D12 flush_draws={}", FlushDrawsKnob()));
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
    Check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)),
          "shader model support before helper PSOs", device.Get());
    Diagnostic(fmt::format("D3D12 shader model={:#x} required=0x60 root signature=1.0",
                           static_cast<u32>(model.HighestShaderModel)));
    if (model.HighestShaderModel < D3D_SHADER_MODEL_6_0)
        throw std::runtime_error("D3D12 requires shader model 6.0");
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const bool have_options = SUCCEEDED(
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
    const bool minmax =
        have_options && options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
    typed_uav_load_additional_formats = have_options && options.TypedUAVLoadAdditionalFormats;
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                              sizeof(options12))) &&
        options12.EnhancedBarriersSupported && options12.RelaxedFormatCastingSupported) {
        device.As(&casting_device);
    }
    Diagnostic(fmt::format("D3D12 relaxed format casting={}", casting_device ? 1 : 0));
#else
    Diagnostic("D3D12 relaxed format casting=0 (SDK lacks ID3D12Device10)");
#endif
    SetMinMaxFilteringSupported(minmax);
    Diagnostic(fmt::format("D3D12 tiled resources tier={} minmax_filtering={}",
                           have_options ? static_cast<int>(options.TiledResourcesTier) : -1,
                           minmax ? 1 : 0));
    Diagnostic("D3D12 CreateDevice complete; creating direct queue");
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue",
          device.Get());
    queue->SetName(L"NXbox direct queue");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence",
          device.Get());
    if (GpuPassProfile::Enabled())
        pass_profile = std::make_unique<GpuPassProfile>(*this);
    facade.device = device.Get();
    facade.reuse_draw_state = BisectEnabled("NXBOX_D3D12_RECORD_STATE");
    {
        char value[2]{};
        facade.compact_draws =
            !(GetEnvironmentVariableA("NXBOX_D3D12_COMPACT_DRAWS", value, sizeof(value)) == 1 &&
              value[0] == '0');
    }
    Diagnostic(fmt::format("D3D12 compact_draws={} record_state={}", facade.compact_draws,
                           facade.reuse_draw_state));
    // On by default: +8-16% fps in BotW, MK8D and Odyssey on a Series X. =0 records inline.
    {
        char value[2]{};
        deferred = !(GetEnvironmentVariableA("NXBOX_D3D12_DEFERRED_RECORDING", value,
                                             sizeof(value)) == 1 &&
                     value[0] == '0');
    }
    Diagnostic(fmt::format("D3D12 deferred_recording={} chunk_size={} chunk_count={}",
                           deferred ? 1 : 0, RecordStream<>::chunk_size,
                           RecordStream<>::chunk_count));
    if (deferred) {
        record_descriptors = std::make_unique<RecordDescriptors>();
        record_descriptors->Initialize(device.Get());
        facade.descriptors = record_descriptors.get();
        // Timestamp segments must bracket all work in a submission. Parallel recording
        // executes its second list after the first list's closing timestamp.
        facade.parallel_enabled = BisectEnabled("NXBOX_D3D12_PARALLEL_RECORD") &&
                                  !NxboxSimSeriesS::Settings().enabled && !pass_profile;
        if (facade.parallel_enabled) {
            facade.parallel_descriptors = std::make_shared<RecordDescriptors>();
            facade.parallel_descriptors->Initialize(device.Get());
            facade.parallel_native = std::make_shared<ParallelNativeState>();
            auto* native = facade.parallel_native.get();
            facade.parallel_worker = std::make_shared<ParallelRecordWorker>([this, native] {
                native->Initialize(device.Get(), fence.Get(), allocators.size());
                return &native->target;
            });
            facade.parallel_begin = [this, native] {
                stream->RouteWritesTo(&facade.parallel_worker->Stream());
                EncodeCall<[](void*, std::uintptr_t address, std::uintptr_t owner, u64 tick) {
                    // Resetting the reusable list must follow the preceding CPU execute,
                    // even if this allocator slot has never been used before.
                    reinterpret_cast<Context*>(owner)->Acknowledge(tick - 1);
                    reinterpret_cast<ParallelNativeState*>(address)->Begin(tick);
                }>(*stream, reinterpret_cast<std::uintptr_t>(native),
                   reinterpret_cast<std::uintptr_t>(this), CurrentTick());
            };
        }
        Diagnostic(fmt::format("D3D12 parallel_record={}", facade.parallel_enabled));
        StartRecorder();
        facade.stream = stream.get();
    } else {
        InitializeLists();
        facade.immediate = commands.Get();
    }
    if (NxboxSimSeriesS::Settings().enabled) {
        try {
            sim_series_s = std::make_unique<SimSeriesS>(*this);
        } catch (...) {
            // Context's destructor will not run if opt-in shader/resource creation fails.
            // Stop the recorder before unwinding its joinable std::thread member.
            if (recorder.joinable()) {
                stream->Close();
                recorder.join();
            }
            throw;
        }
    }
}
void Context::InitializeLists() {
    for (auto& slot : allocators) {
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&slot.handle)),
              "CreateCommandAllocator", device.Get());
    }
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].handle.Get(),
                                    nullptr, IID_PPV_ARGS(&commands)),
          "CreateCommandList", device.Get());
    Check(commands->Close(), "initial Close", device.Get());
}

void Context::CheckRecorder() {
    if (facade.parallel_worker)
        facade.parallel_worker->Check();
    recorder_progress.Check();
}
void Context::Acknowledge(u64 tick) {
    if (deferred)
        recorder_progress.Await(tick);
}
void Context::StartRecorder() {
    stream = std::make_unique<RecordStream<>>(
        BisectEnabled("NXBOX_D3D12_SMALL_RECORDER_QUEUE") ? 8 : RecordStream<>::chunk_count,
        BisectEnabled("NXBOX_D3D12_DIRECT_REPLAY"));
    stream->metrics = profile.Enabled();
    recorder = std::thread{[this] {
        RecordingTarget target;
        target.owner = this;
        bool execute = true;
        auto fail = [this] { recorder_progress.Fail(std::current_exception()); };
        try {
            // Thread naming converts UTF-8 on Windows and can allocate/throw too.
            Common::SetCurrentThreadName("D3D12 recorder");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
            InitializeLists();
            facade.depth_bounds_status = commands.As(&extended1);
            facade.independent_stencil_status = commands.As(&extended8);
            target.list = commands.Get();
            target.list1 = extended1.Get();
            target.list8 = extended8.Get();
            recorder_progress.Ready();
        } catch (...) {
            fail();
            execute = false;
        }
        // Continue draining holds after a failed replay so a producer blocked at a chunk
        // boundary can always finish publishing and observe the exception at submit/wait.
        for (;;) {
            try {
                const bool more = stream->Consume(
                    &target, execute && !submit_failed.load(std::memory_order_acquire));
                if (!more)
                    break;
            } catch (...) {
                fail();
                execute = false;
            }
        }
    }};
    profile.BindRecorder(recorder.native_handle());
    try {
        recorder_progress.WaitReady();
    } catch (...) {
        stream->Close();
        recorder.join();
        throw;
    }
}
void Context::RecorderBegin() {
    auto& slot = allocators[allocator_index];
    // No completion callbacks on this thread. Fence-only retirement of native allocators.
    if (!IsFree(slot.tick)) {
        const HANDLE event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (!event)
            Check(HRESULT_FROM_WIN32(GetLastError()), "recorder CreateEventExW", device.Get());
        const auto status = fence->SetEventOnCompletion(slot.tick, event);
        if (FAILED(status)) {
            CloseHandle(event);
            Check(status, "recorder SetEventOnCompletion", device.Get());
        }
        const auto wait = WaitForSingleObjectEx(event, 10000, FALSE);
        const auto error = GetLastError();
        CloseHandle(event);
        Check(device->GetDeviceRemovedReason(), "recorder allocator wait", device.Get());
        if (wait != WAIT_OBJECT_0) {
            Check(wait == WAIT_FAILED ? HRESULT_FROM_WIN32(error)
                                      : HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                  "recorder allocator wait", device.Get());
        }
    }
    Check(slot.handle->Reset(), "allocator Reset", device.Get());
    Check(commands->Reset(slot.handle.Get(), nullptr), "command list Reset", device.Get());
}
void Context::RecorderSubmit(u64 tick) {
    Check(commands->Close(), "Close", device.Get());
    auto* parallel = facade.parallel_native.get();
    const bool second = parallel && std::exchange(parallel->submit_second, false);
    ID3D12CommandList* lists[]{commands.Get(), second ? parallel->list.Get() : nullptr};
    Check(device->GetDeviceRemovedReason(), "before ExecuteCommandLists", device.Get());
    queue->ExecuteCommandLists(second ? 2 : 1, lists);
    Check(device->GetDeviceRemovedReason(), "after ExecuteCommandLists", device.Get());
    if (info_callback.registered)
        ClearInfoQueue(info_callback.queue.Get());
    else
        LogDeviceInfoQueue(device.Get());
    Check(queue->Signal(fence.Get(), tick), "Signal", device.Get());
    allocators[allocator_index].tick = tick;
    allocator_index = (allocator_index + 1) % allocators.size();
    recorder_progress.Acknowledge(tick);
    NxboxWait::Max(NxboxWait::Counter::SubmittedTick, tick);
}
u64 Context::SealRecording(FrameCount reason) {
    CheckRecorder();
    InvalidateRecordedState();
    flush_cadence.Submitted();
    if (!recording)
        return sealed;
    const auto tick = CurrentTick();
    if (pass_profile)
        pass_profile->EndSubmission();
    if (gpu_time_heap && profile.Enabled()) {
        const auto first = static_cast<UINT>(((tick - 1) % allocators.size()) * 2);
        facade.EndQuery(gpu_time_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        facade.ResolveQueryData(gpu_time_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2,
                                gpu_time_readback->handle.Get(), u64{first} * 8);
        completion.push_back([this, first] {
            u64 stamps[2]{};
            std::memcpy(stamps, gpu_time_readback->mapped.data() + u64{first} * 8, sizeof(stamps));
            if (stamps[1] > stamps[0])
                profile.gpu_busy += static_cast<u64>(static_cast<double>(stamps[1] - stamps[0]) *
                                                     gpu_ticks_to_100ns);
        });
    }
    std::string contents;
    for (const auto& label : work) {
        if (!contents.empty())
            contents += ", ";
        contents += label;
    }
    const auto name = fmt::format("NXbox execute {}: {}", ++execute_sequence,
                                  contents.empty() ? "barriers/state" : contents);
    const std::wstring wide_name(name.begin(), name.end());
    EncodeCall<[](void* target, const wchar_t* name) {
        static_cast<RecordingTarget*>(target)->list->SetName(name);
    }>(*stream, Snapshot(wide_name.c_str(), wide_name.size() + 1));
    if (BisectEnabled("NXBOX_D3D12_SUBMIT_TRACE") || execute_sequence <= 8 ||
        execute_sequence % 120 == 0)
        Diagnostic(fmt::format("D3D12 ExecuteCommandLists sequence={} tick={} contents={}",
                               execute_sequence, tick, contents));
    // Install completion ownership before publication of the submission control record.
    {
        std::lock_guard lock{completion_mutex};
        batches.push_back({tick, std::move(completion), std::move(retained)});
        completion.clear();
        retained.clear();
    }
    if (facade.validation.enabled) {
        const auto summary = facade.validation.Summary();
        EncodeCall<[](void*, const char* message) { Diagnostic(message); }>(
            *stream, Snapshot(summary.c_str(), summary.size() + 1));
    }
    if (facade.parallel_split) {
        EncodeCall<[](void* target) {
            Check(static_cast<RecordingTarget*>(target)->list->Close(), "parallel Close");
        }>(*stream);
        stream->RouteWritesTo(nullptr);
        facade.parallel_worker->Publish(tick);
        EncodeCall<[](void* target, u64 tick) {
            auto* context = static_cast<Context*>(static_cast<RecordingTarget*>(target)->owner);
            context->facade.parallel_worker->Wait(tick);
            context->facade.parallel_native->submit_second = true;
        }>(*stream, tick);
    }
    // Encoded snapshot packets retain their own immutable leases until replay retires.
    facade.parallel_snapshot.Reset();
    EncodeCall<[](void* target, u64 tick) {
        static_cast<Context*>(static_cast<RecordingTarget*>(target)->owner)->RecorderSubmit(tick);
    }>(*stream, tick);
    sealed = tick;
    NxboxWait::Max(NxboxWait::Counter::SealedTick, tick);
    recording = false;
    work.clear();
    copy_writes.clear();
    stream->Flush();
    profile.Count(FrameCount::Executes);
    profile.Count(reason);
    if (profile.Enabled()) {
        profile.recorder_waits += std::exchange(stream->waits, 0);
        profile.recorder_wait_ns += std::exchange(stream->wait_ns, 0);
    }
    CheckRecorder();
    if (BisectEnabled("NXBOX_D3D12_SUBMIT_TRACE"))
        WaitSubmitted(tick, "submit trace");
    profile.EndFrame(false);
    return tick;
}

Context::~Context() {
    try {
        Finish(FrameCount::ForcedSubmitLifecycle);
    } catch (const std::exception& error) {
        ReportFailure(error);
    } catch (...) {
        Diagnostic("D3D12 context teardown: non-standard exception");
    }
    if (recorder.joinable()) {
        stream->Close();
        recorder.join();
    }
    facade.parallel_worker.reset();
    facade.parallel_snapshot.Reset();
}
void Context::LogInfoQueue() noexcept {
    if (deferred)
        return; // Recorder owns draining the shared InfoQueue.

    if (info_callback.registered)
        ClearInfoQueue(info_callback.queue.Get());
    else
        LogDeviceInfoQueue(device.Get());
}
void Context::ReportFailure(const std::exception& error) noexcept {
    try {
        // The same unsupported state may recur every draw. Preserve its exact reason once.
        static std::mutex mutex;
        static std::set<std::string> reasons;
        bool first;
        {
            const std::lock_guard lock{mutex};
            first = reasons.emplace(error.what()).second;
        }
        if (first) {
            Diagnostic(fmt::format(
                "D3D12 boundary event=renderer_exception fallback=skip_work what={} "
                "removed={:#010x}",
                error.what(),
                static_cast<u32>(device ? device->GetDeviceRemovedReason() : E_POINTER)));
            LogInfoQueue();
            LogDeviceDred(device.Get());
            SetEnvironmentVariableA("NXBOX_D3D12_FIRST_FAILURE", error.what());
        }
        // Unsupported guest work is recoverable. Only device loss disables future submissions.
        if (device && FAILED(device->GetDeviceRemovedReason()))
            SetEnvironmentVariableA("NXBOX_D3D12_COMMAND_FAILURE", "1");
    } catch (...) {
        // Allocation/logging failure must not turn the noexcept reporter into terminate.
#ifdef NXBOX_UWP
        EdenXbox::CrashRendererException("failure_report", error.what());
#endif
        OutputDebugStringA(error.what());
        OutputDebugStringA("\nD3D12 failure reporting incomplete\n");
    }
}

bool Context::IsFree(u64 tick) const {
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        Check(device->GetDeviceRemovedReason(), "GetCompletedValue device removed", device.Get());
        throw std::runtime_error("D3D12 fence returned UINT64_MAX with S_OK removed reason");
    }
    return (!deferred || LastSubmittedTick() >= tick) && completed >= tick;
}
u64 Context::CompletedTick() const {
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        Check(device->GetDeviceRemovedReason(), "GetCompletedValue device removed", device.Get());
        throw std::runtime_error("D3D12 fence returned UINT64_MAX with S_OK removed reason");
    }
    return deferred ? std::min(completed, LastSubmittedTick()) : completed;
}
void Context::Wait(u64 tick, std::string_view site) {
    if (deferred)
        CheckRecorder();
    if (tick > LastSealedTick()) {
        if (tick != CurrentTick() || (deferred && !recording))
            throw std::invalid_argument("D3D12 future fence");
        Submit(FrameCount::ForcedSubmitFenceWait);
    }
    WaitSubmitted(tick, site);
    LogInfoQueue();
}
void Context::WaitSubmitted(u64 tick, std::string_view site) {
    const NxboxWait::Scope wait_diag_scope{NxboxWait::Kind::Fence, GetCurrentThreadId(), site,
                                      std::array<std::uint64_t, 1>{tick}};
    Acknowledge(tick);
    if (!IsFree(tick)) {
        const u64 wait_start = profile.Enabled() ? ProfileWallTime() : 0;
        const FrameProfile::Scope frame_wait{profile, FramePhase::FenceWait};
        const bool spun_ready =
            WaitSpinEnabled() &&
            SpinUntilReady([this, tick] { return IsFree(tick); },
                           [] { return std::chrono::steady_clock::now(); },
                           [] { YieldProcessor(); }, std::chrono::microseconds{50});
        if (!spun_ready) {
            // Each waiter owns its event; GPU and fencing threads may wait concurrently.
            const HANDLE wait_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            if (!wait_event)
                Check(HRESULT_FROM_WIN32(GetLastError()), "CreateEventExW", device.Get());
            const auto result = fence->SetEventOnCompletion(tick, wait_event);
            if (FAILED(result)) {
                CloseHandle(wait_event);
                Check(result, "SetEventOnCompletion", device.Get());
            }
            DWORD wait;
            DWORD wait_error;
            {
                const EdenXbox::ThreadProfile::Scope wait_scope{
                    EdenXbox::ThreadProfile::Wait::Fence};
                wait = WaitForSingleObjectEx(wait_event, 10000, FALSE);
                wait_error = GetLastError();
            }
            CloseHandle(wait_event);
            if (wait != WAIT_OBJECT_0) {
                Diagnostic(fmt::format("D3D12 wait tick={} result={:#x}", tick, wait));
                Check(device->GetDeviceRemovedReason(), "device removed while waiting",
                      device.Get());
                Check(wait == WAIT_FAILED ? HRESULT_FROM_WIN32(wait_error)
                                          : HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                      "WaitForSingleObjectEx", device.Get());
                throw std::runtime_error("D3D12 fence wait failed/timed out");
            }
        }
        if (profile.Enabled()) {
            const auto wall = ProfileWallTime() - wait_start;
            profile.NoteWait(site, wall);
            profile.NoteWait(spun_ready ? "spin_ready" : "event_block", wall);
        }
    }
    Check(device->GetDeviceRemovedReason(), "device removed after waiting", device.Get());
    CompleteThrough(tick);
}
void Context::CompleteThrough(u64 tick) {
    // Keep callback execution ordered with readback retirement on the fencing thread.
    // Detach first so reentrant waits cannot replay a batch.
    if (deferred)
        tick = std::min(tick, LastSubmittedTick());
    NxboxWait::Max(NxboxWait::Counter::CompletedObservedTick, tick);
    std::scoped_lock lock{completion_mutex};
    while (!batches.empty() && batches.front().tick <= tick) {
        auto batch = std::move(batches.front());
        batches.pop_front();
        // Retain the batch resources until every callback has had its turn, including
        // callbacks after a failed readback. Never replay an already detached batch.
        for (auto& callback : batch.callbacks) {
            try {
                callback();
            } catch (const std::exception& error) {
                ReportFailure(error);
            } catch (...) {
                const EdenXbox::UnknownRendererException error{
                    "D3D12 completion callback: non-standard exception"};
                ReportFailure(error);
            }
        }
    }
}
void Context::Collect() {
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX)
        IsFree(0); // Preserve device removal diagnostics.
    CompleteThrough(completed);
}
CommandFacade* Context::Commands() {
    profile.BindRecordingThread();
    static const bool debug_recorder = BisectEnabled("NXBOX_D3D12_DEBUG");
    if (debug_recorder) {
        // Diagnostic: the command list and copy hazard state are single-threaded.
        static std::atomic<DWORD> owner_thread{};
        static std::atomic<u32> foreign{};
        DWORD expected{};
        const DWORD self = GetCurrentThreadId();
        if (!owner_thread.compare_exchange_strong(expected, self) && expected != self &&
            foreign.fetch_add(1) < 16)
            Diagnostic(fmt::format("D3D12_FOREIGN_RECORDER thread={} owner={} recording={}",
                                   self, expected, recording));
    }
    if (!recording) {
        Collect();
        if (!deferred)
            WaitSubmitted(allocators[allocator_index].tick, "allocator");
        // Timestamp readback slots also need their completion callback before reuse.
        if (deferred && gpu_time_heap && CurrentTick() > allocators.size())
            WaitSubmitted(CurrentTick() - allocators.size(), "recorder timestamp reuse");
        // Created before Reset: CreateResource may submit under memory pressure.
        if (profile.Enabled() && !gpu_time_heap) {
            D3D12_QUERY_HEAP_DESC desc{};
            desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            desc.Count = static_cast<UINT>(allocators.size() * 2);
            Check(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&gpu_time_heap)),
                  "Create GPU time query heap", device.Get());
            gpu_time_readback = CreateResource(
                D3D12_RESOURCE_DESC{D3D12_RESOURCE_DIMENSION_BUFFER, 0,
                                    allocators.size() * 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                    {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                    D3D12_RESOURCE_FLAG_NONE},
                D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            UINT64 frequency{};
            Check(queue->GetTimestampFrequency(&frequency), "GetTimestampFrequency",
                  device.Get());
            gpu_ticks_to_100ns = 10000000.0 / static_cast<double>(frequency);
        }
    }
    // A completion callback may have started the next command list.
    if (!recording && sim_series_s)
        sim_series_s->PrepareFrame();
    if (!recording) {
        if (deferred) {
            CheckRecorder();
            EncodeCall<[](void* target) {
                static_cast<Context*>(static_cast<RecordingTarget*>(target)->owner)
                    ->RecorderBegin();
            }>(*stream);
        } else {
            Check(allocators[allocator_index].handle->Reset(), "allocator Reset", device.Get());
            Check(commands->Reset(allocators[allocator_index].handle.Get(), nullptr),
                  "command list Reset", device.Get());
        }
        facade.validation.Begin(CurrentTick());
        facade.ResetDrawState();
        facade.BeginParallelRecording();
        copy_writes.clear();
        // Globally unique across Context instances; never reuse a resource stamp.
        static std::atomic<u64> next_retention_generation{1};
        retention_generation = next_retention_generation.fetch_add(1, std::memory_order_relaxed);
        recording = true;
        facade.graphics_root_cbvs.Reset();
        InvalidateRecordedState();
        if (gpu_time_heap && profile.Enabled()) {
            facade.EndQuery(
                gpu_time_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                static_cast<UINT>(
                    (deferred ? (CurrentTick() - 1) % allocators.size() : allocator_index) * 2));
        }
        if (sim_series_s)
            sim_series_s->BeginSegment(facade);
        if (pass_profile)
            pass_profile->BeginSubmission(facade);
    }
    return &facade;
}
void Context::GpuPassFrame() {
    if (pass_profile)
        pass_profile->EndFrame();
}
void Context::SimulateSeriesSFrame() {
    if (sim_series_s)
        sim_series_s->EndFrame();
}
void Context::MarkWork(std::string_view label) {
    Commands();
    if (facade.pass_profile)
        facade.pass_profile->Label(label);
    // Copies, uploads and queries never touch graphics pipeline state; every other
    // non-draw path may (compute shares the current PSO, clears and blits draw).
    // Do this even for a label already seen on this recording.
    constexpr std::array<std::string_view, 7> state_neutral{
        "guest draws",         "upload/copy",          "vertex binding padding",
        "query readback/copy", "queries",              "query report/predicate copy",
        "timestamp query"};
    if (std::find(state_neutral.begin(), state_neutral.end(), label) == state_neutral.end())
        InvalidateRecordedState();
    if (label == "guest draws") {
        profile.Count(FrameCount::Draws);
        flush_cadence.Drew();
    }
    if (std::find(work.begin(), work.end(), label) != work.end())
        return;
    work.emplace_back(label);
    // PIX string marker encoding consumed by DRED breadcrumb contexts.
    const std::wstring marker(label.begin(), label.end());
    facade.SetMarker(0, marker.c_str(), static_cast<UINT>((marker.size() + 1) * sizeof(wchar_t)));
}
u64 Context::Submit(FrameCount reason) {
    // Device removal can throw after the command list was closed but before its tick was
    // signalled. Fences that captured that tick would never count as submitted.
    struct FailureFlag {
        std::atomic<bool>& flag;
        int exceptions{std::uncaught_exceptions()};
        ~FailureFlag() {
            if (std::uncaught_exceptions() > exceptions)
                flag.store(true, std::memory_order_release);
        }
    } failure_flag{submit_failed};
    if (before_submit)
        before_submit();
    if (sim_series_s && recording)
        sim_series_s->EndSegment(facade);
    const FrameProfile::Scope profile_submit{profile, FramePhase::Submit};
    if (deferred) {
        try {
            return SealRecording(reason);
        } catch (...) {
            recorder_progress.Fail(std::current_exception());
            throw;
        }
    }
    InvalidateRecordedState();
    flush_cadence.Submitted();
    // QueueFence and FlushCommands can both request the same submission.
    // Empty requests reuse its value instead of emitting another queue signal.
    if (!recording)
        return submitted;
    if (recording) {
        LogInfoQueue();
        if (pass_profile)
            pass_profile->EndSubmission();
        if (gpu_time_heap && profile.Enabled()) {
            const auto first = static_cast<UINT>(allocator_index * 2);
            commands->EndQuery(gpu_time_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
            commands->ResolveQueryData(gpu_time_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2,
                                       gpu_time_readback->handle.Get(), u64{first} * 8);
            completion.push_back([this, first] {
                u64 stamps[2]{};
                std::memcpy(stamps, gpu_time_readback->mapped.data() + u64{first} * 8,
                            sizeof(stamps));
                if (stamps[1] > stamps[0])
                    profile.gpu_busy += static_cast<u64>(
                        static_cast<double>(stamps[1] - stamps[0]) * gpu_ticks_to_100ns);
            });
        }
        Check(commands->Close(), "Close", device.Get());
        ID3D12CommandList* lists[] = {commands.Get()};
        std::string contents;
        for (const auto& label : work) {
            if (!contents.empty())
                contents += ", ";
            contents += label;
        }
        const auto name = fmt::format("NXbox execute {}: {}", ++execute_sequence,
                                      contents.empty() ? "barriers/state" : contents);
        const std::wstring wide_name(name.begin(), name.end());
        commands->SetName(wide_name.c_str());
        // NXBOX_D3D12_SUBMIT_TRACE=1 logs every submission and waits for it to finish, so the last
        // logged line before a device removal names the command list that caused it.
        static const bool submit_trace = BisectEnabled("NXBOX_D3D12_SUBMIT_TRACE");
        if (submit_trace || execute_sequence <= 8 || execute_sequence % 120 == 0)
            Diagnostic(fmt::format("D3D12 ExecuteCommandLists sequence={} tick={} contents={}",
                                   execute_sequence, CurrentTick(), contents));
        Check(device->GetDeviceRemovedReason(), "before ExecuteCommandLists", device.Get());
        if (facade.validation.enabled)
            Diagnostic(facade.validation.Summary());
        queue->ExecuteCommandLists(1, lists);
        profile.Count(FrameCount::Executes);
        profile.Count(reason);
        recording = false;
        allocators[allocator_index].tick = CurrentTick();
        allocator_index = (allocator_index + 1) % allocators.size();
        work.clear();
        copy_writes.clear();
        LogInfoQueue();
        Check(device->GetDeviceRemovedReason(), "after ExecuteCommandLists", device.Get());
    }
    const u64 tick = CurrentTick();
    {
        std::scoped_lock lock{completion_mutex};
        batches.push_back({tick, std::move(completion), std::move(retained)});
        completion.clear();
        retained.clear();
    }
    Check(queue->Signal(fence.Get(), tick), "Signal", device.Get());
    submitted = tick;
    NxboxWait::Max(NxboxWait::Counter::SealedTick, tick);
    NxboxWait::Max(NxboxWait::Counter::SubmittedTick, tick);
    submit_failed.store(false, std::memory_order_release);
    if (BisectEnabled("NXBOX_D3D12_SUBMIT_TRACE"))
        WaitSubmitted(tick, "submit trace");
    // Report progress even when the guest has stopped presenting.
    profile.EndFrame(false);
    LogInfoQueue();
    return submitted;
}
bool Context::FlushDrawsIfDue() {
    if (!flush_cadence.Due())
        return false;
    Submit(FrameCount::ForcedSubmitFlushDraws);
    return true;
}
void Context::Finish(FrameCount reason) {
    Wait(Submit(reason), "finish");
}
void Context::Retain(const ResourcePtr& resource) {
    // Opening a recording can retire resources and run callbacks. Consult the stamp after it.
    if (!recording)
        Commands();
    if (!resource || resource->retained_generation == retention_generation)
        return;
    // Publish only after push_back succeeds, so allocation failure cannot lose the lease.
    retained.push_back(resource);
    resource->retained_generation = retention_generation;
}
namespace {
constexpr bool ShaderReadStateCovers(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    constexpr auto shader_reads =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // A transition to a subset of an existing shader-read state orders no writes. Preserve
    // the actual superset in the tracker so the next write barrier has the correct Before.
    // COMMON (zero), write states and split subresources must keep their normal barriers.
    return after != 0 && (before & ~shader_reads) == 0 && (before & after) == after;
}
} // namespace
void Context::Transition(const ResourcePtr& resource, D3D12_RESOURCE_STATES state) {
    if (!resource)
        throw std::invalid_argument("D3D12 null resource");
    if (before_resource_use)
        before_resource_use(resource);
    Retain(resource);
    if (!resource->sub_states.empty()) {
        const std::pair<ResourcePtr, D3D12_RESOURCE_STATES> use{resource, state};
        TransitionBatch(std::span{&use, 1});
        return;
    }
    if (state & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                 D3D12_RESOURCE_STATE_STREAM_OUT))
        ++resource->content_version;
    if (resource->heap != D3D12_HEAP_TYPE_DEFAULT || resource->state == state)
        return;
    if (ShaderReadStateCovers(resource->state, state))
        return;
    const FrameProfile::Scope profile_barrier{profile, FramePhase::Barriers};
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource->handle.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = resource->state;
    barrier.Transition.StateAfter = state;
    facade.ResourceBarrier(1, &barrier);
    profile.Count(FrameCount::Barriers);
    if (profile.Enabled())
        profile.NoteTransition(resource->state, state,
                           resource->handle->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
    copy_writes.erase(resource->handle.Get());
    resource->state = state;
}
void Context::TransitionSubresources(const ResourcePtr& resource, std::span<const u32> subresources,
                                     D3D12_RESOURCE_STATES state) {
    if (!resource)
        throw std::invalid_argument("D3D12 null subresource resource");
    Retain(resource);
    const auto desc = resource->handle->GetDesc();
    if (resource->heap != D3D12_HEAP_TYPE_DEFAULT ||
        desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        throw std::invalid_argument(
            "D3D12 subresource transitions require a default color texture");
    const u32 layers =
        desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize;
    const u32 count = desc.MipLevels * layers; // Color textures have one plane.
    for (const auto subresource : subresources) {
        if (subresource >= count)
            throw std::out_of_range("D3D12 texture subresource transition");
    }
    if (subresources.empty())
        return;
    if (resource->sub_states.empty())
        resource->sub_states.assign(count, resource->state);
    if (state & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                 D3D12_RESOURCE_STATE_STREAM_OUT))
        ++resource->content_version;
    if (std::all_of(subresources.begin(), subresources.end(),
                    [&](u32 index) { return resource->sub_states[index] == state; }))
        return;
    const FrameProfile::Scope profile_barrier{profile, FramePhase::Barriers};
    auto& barriers = transition_barriers;
    barriers.clear();
    barriers.reserve(subresources.size());
    for (const auto subresource : subresources) {
        auto& before = resource->sub_states[subresource];
        if (before == state)
            continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource->handle.Get(), subresource, before, state};
        barriers.push_back(barrier);
        if (profile.Enabled())
            profile.NoteTransition(before, state, false);
        before = state;
    }
    if (!barriers.empty()) {
        facade.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        profile.Count(FrameCount::Barriers, barriers.size());
        copy_writes.erase(resource->handle.Get());
    }
}
void Context::CrossListCopyBarrier(Resource& resource, u64 tick, u64 offset, u64 size) {
    // The Xbox queue can overlap consecutive command lists: a copy recorded in an earlier,
    // still-running list (e.g. a new buffer's zero fill) raced the next upload into it.
    static const bool cross_list = !BisectEnabled("NXBOX_D3D12_NO_CROSS_LIST_COPY_BARRIER");
    if (cross_list && resource.last_copy_tick && resource.last_copy_tick < tick &&
        !IsFree(resource.last_copy_tick)) {
        const FrameProfile::Scope profile_barrier{profile, FramePhase::Barriers};
        if (resource.heap == D3D12_HEAP_TYPE_DEFAULT && resource.sub_states.empty() &&
            resource.state == D3D12_RESOURCE_STATE_COPY_DEST) {
            // Same reason as BeforeCopyWrite: aliasing barriers did not drain copies here.
            D3D12_RESOURCE_BARRIER barriers[2]{};
            barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition = {resource.handle.Get(),
                                      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON};
            barriers[1].Transition = {resource.handle.Get(),
                                      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
            facade.ResourceBarrier(2, barriers);
            profile.Count(FrameCount::Barriers, 2);
        } else {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            facade.ResourceBarrier(1, &barrier);
            profile.Count(FrameCount::Barriers);
        }
        profile.Count(FrameCount::CopyHazardBarriers);
        static u32 logged{};
        if (logged < 8 || ++logged % 4096 == 0) {
            logged += logged < 8;
            Diagnostic(fmt::format("D3D12_CROSS_LIST_COPY_BARRIER resource={:#x} previous_tick={} "
                                   "tick={} offset={} size={}",
                                   resource.handle->GetGPUVirtualAddress(),
                                   resource.last_copy_tick, tick, offset, size));
        }
        copy_writes.clear();
    }
}
void Context::BeforeCopyWrite(const ResourcePtr& resource, u64 offset, u64 size) {
    if (!resource)
        throw std::invalid_argument("D3D12 null copy destination");
    if (!size)
        return;
    // Start recording before inspecting ranges; completion callbacks can record work.
    Retain(resource);
    auto* destination = resource->handle.Get();
    const u64 tick = CurrentTick();
    CrossListCopyBarrier(*resource, tick, offset, size);
    resource->last_copy_tick = tick;
    const auto found = copy_writes.find(destination);
    if (found != copy_writes.end()) {
        for (const auto& range : found->second) {
            // Half-open ranges, without overflowing offset + size at the buffer end.
            const bool overlaps = offset < range.offset ? range.offset - offset < size
                                                        : offset - range.offset < range.size;
            if (!overlaps)
                continue;
            const FrameProfile::Scope profile_barrier{profile, FramePhase::Barriers};
            profile.Count(FrameCount::CopyHazardBarriers);
            ++copy_hazard_count;
            // A null aliasing barrier did not order copy-to-copy writes on the Xbox: a new
            // buffer's zero fill landed over the tail of the upload recorded after it.
            // Round-trip the destination through COMMON; transitions drain its prior writes.
            static const bool alias_only = BisectEnabled("NXBOX_D3D12_ALIAS_COPY_BARRIER");
            if (!alias_only && resource->heap == D3D12_HEAP_TYPE_DEFAULT &&
                resource->sub_states.empty() &&
                resource->state == D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER barriers[2]{};
                barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[0].Transition = {destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_COMMON};
                barriers[1].Transition = {destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_COMMON,
                                          D3D12_RESOURCE_STATE_COPY_DEST};
                facade.ResourceBarrier(2, barriers);
                profile.Count(FrameCount::Barriers, 2);
                copy_writes.erase(found);
                break;
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            barrier.Aliasing.pResourceBefore = nullptr;
            barrier.Aliasing.pResourceAfter = nullptr;
            facade.ResourceBarrier(1, &barrier);
            profile.Count(FrameCount::Barriers);
            // The null/null aliasing barrier orders all prior work, not just this buffer.
            copy_writes.clear();
            break;
        }
    }
    copy_writes[destination].push_back({offset, size});
}
void Context::GlobalUavBarrier() {
    const FrameProfile::Scope profile_barrier{profile, FramePhase::Barriers};
    auto* list = Commands();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    list->ResourceBarrier(1, &barrier);
    profile.Count(FrameCount::Barriers);
}
void Context::TransitionBatch(std::span<const std::pair<ResourcePtr, D3D12_RESOURCE_STATES>> uses) {
    if (before_resource_use) {
        for (const auto& use : uses)
            before_resource_use(use.first);
    }
    // Start/retire recording before reading tracked states: completion callbacks
    // can themselves record transitions. Keep all intermediate transitions ordered.
    auto* list = Commands();
    for (const auto& use : uses) {
        if (!use.first)
            throw std::invalid_argument("D3D12 null batch resource");
        Retain(use.first);
    }
    const bool needs_barrier = std::any_of(uses.begin(), uses.end(), [](const auto& use) {
        const auto& [resource, state] = use;
        if (!resource->sub_states.empty())
            return std::any_of(resource->sub_states.begin(), resource->sub_states.end(),
                               [&](auto before) { return before != state; });
        return resource->heap == D3D12_HEAP_TYPE_DEFAULT && resource->state != state &&
               !ShaderReadStateCovers(resource->state, state);
    });
    if (!needs_barrier) {
        for (const auto& [resource, state] : uses) {
            if (state & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                         D3D12_RESOURCE_STATE_STREAM_OUT))
                ++resource->content_version;
            if (!resource->sub_states.empty()) {
                resource->sub_states.clear();
                resource->state = state;
            }
        }
        return;
    }
    const FrameProfile::Scope profile_barrier{profile, FramePhase::Barriers};
    // Retain can open a list and run callbacks which also transition resources.
    // Acquire the shared scratch only after every resource has been retained.
    auto& barriers = transition_barriers;
    barriers.clear();
    barriers.reserve(uses.size());
    for (const auto& [resource, state] : uses) {
        if (state & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                     D3D12_RESOURCE_STATE_STREAM_OUT))
            ++resource->content_version;
        if (!resource->sub_states.empty()) {
            for (u32 subresource = 0; subresource < resource->sub_states.size(); ++subresource) {
                const auto before = resource->sub_states[subresource];
                if (before == state)
                    continue;
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = {resource->handle.Get(), subresource, before, state};
                barriers.push_back(barrier);
                if (profile.Enabled())
                    profile.NoteTransition(before, state, false);
            }
            resource->sub_states.clear();
            resource->state = state;
            continue;
        }
        if (resource->heap != D3D12_HEAP_TYPE_DEFAULT || resource->state == state ||
            ShaderReadStateCovers(resource->state, state))
            continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource->handle.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              resource->state, state};
        barriers.push_back(barrier);
        if (profile.Enabled())
            profile.NoteTransition(resource->state, state,
                               resource->handle->GetDesc().Dimension ==
                                   D3D12_RESOURCE_DIMENSION_BUFFER);
        resource->state = state;
    }
    if (!barriers.empty()) {
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        profile.Count(FrameCount::Barriers, barriers.size());
        for (const auto& barrier : barriers)
            copy_writes.erase(barrier.Transition.pResource);
    }
}
void Context::AfterCompletion(std::function<void()> callback) {
    Commands();
    completion.push_back(std::move(callback));
}
TextureMemoryBudget NativeTextureMemoryBudget(u64 cache) noexcept {
#ifdef NXBOX_UWP
    try {
        using winrt::Windows::System::MemoryManager;
        return TextureMemoryThresholds(MemoryManager::AppMemoryUsageLimit(),
                                       MemoryManager::AppMemoryUsage(), cache);
    } catch (...) {
        // Retain the ordinary watermarks if platform telemetry is unavailable.
    }
#else
    static_cast<void>(cache);
#endif
    return {};
}
u64 NativeBufferMemoryTarget(u64 cache) noexcept {
#ifdef NXBOX_UWP
    try {
        using winrt::Windows::System::MemoryManager;
        return BufferMemoryTarget(MemoryManager::AppMemoryUsageLimit(),
                                  MemoryManager::AppMemoryUsage(), cache);
    } catch (...) {
        // Keep allocation available if the platform telemetry is temporarily unavailable.
    }
#endif
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        return BufferMemoryTarget(memory.ullTotalPhys, memory.ullTotalPhys - memory.ullAvailPhys,
                                  cache);
    return 1ULL << 30;
}
namespace {
// Each placed resource owns its page lease until every fence/binding reference is gone.
// Empty pages are evicted oldest-first under pressure; populated pages never move.
struct BufferHeapPage {
    static constexpr u64 Size = 16ULL << 20;
    ComPtr<ID3D12Heap> handle;
    D3D12_HEAP_TYPE type;
    BufferHeapSlots slots{static_cast<u32>(Size / BufferHeapSlots::Alignment)};
    std::shared_ptr<MemoryBudget> budget;
    std::mutex mutex;
    ~BufferHeapPage() {
        if (handle)
            budget->used -= Size;
    }
};
struct BufferHeapPool {
    std::vector<std::shared_ptr<BufferHeapPage>> pages;
    void Trim() {
        std::erase_if(pages, [](const auto& page) {
            const std::scoped_lock lock{page->mutex};
            return page->slots.Used() == 0;
        });
    }
    ResourcePtr Allocate(ID3D12Device* device, const std::shared_ptr<MemoryBudget>& budget,
                         const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE type,
                         D3D12_RESOURCE_STATES state, const std::shared_ptr<BufferHeapPool>& owner,
                         bool allow_growth = true) {
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
        if (allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes > BufferHeapPage::Size)
            return {};
        auto pending = std::make_unique<Resource>();
        // Round up: a truncated count let the next placed buffer overlap this one's tail, and
        // its zero clear erased live data. A zero count also dereferenced an empty slot.
        const auto count = static_cast<u32>(
            std::max<u64>(1, (allocation.SizeInBytes + BufferHeapSlots::Alignment - 1) /
                                 BufferHeapSlots::Alignment));
        std::shared_ptr<BufferHeapPage> page;
        std::optional<u32> start;
        for (const auto& candidate : pages) {
            if (candidate->type != type)
                continue;
            const std::scoped_lock lock{candidate->mutex};
            start = candidate->slots.Allocate(count);
            if (start) {
                page = candidate;
                break;
            }
        }
        if (!page && !allow_growth)
            return {};
        if (!page) {
            page = std::make_shared<BufferHeapPage>();
            page->budget = budget;
            page->type = type;
            D3D12_HEAP_DESC heap{};
            heap.SizeInBytes = BufferHeapPage::Size;
            heap.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            heap.Properties.Type = type;
            heap.Properties.CreationNodeMask = heap.Properties.VisibleNodeMask = 1;
            heap.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            Check(device->CreateHeap(&heap, IID_PPV_ARGS(&page->handle)), "Create buffer heap",
                  device);
            budget->used += BufferHeapPage::Size;
            pages.push_back(page);
            start = page->slots.Allocate(count);
            if (!start)
                return {};
        }
        ResourcePtr resource{
            pending.release(), [page, start = *start, count, owner](Resource* value) {
                // The owner capture preserves the pool while a fence still holds this lease.
                (void)owner;
                delete value; // Release the resource before recycling its heap range.
                const std::scoped_lock lock{page->mutex};
                page->slots.Release(start, count);
            }};
        Check(device->CreatePlacedResource(page->handle.Get(),
                                           u64{*start} * BufferHeapSlots::Alignment, &desc, state,
                                           nullptr, IID_PPV_ARGS(&resource->handle)),
              "Create placed buffer", device);
        resource->heap = type;
        resource->state = state;
        resource->allocation_size = allocation.SizeInBytes;
        if (type != D3D12_HEAP_TYPE_DEFAULT) {
            void* mapped{};
            const D3D12_RANGE range{
                0, type == D3D12_HEAP_TYPE_READBACK ? static_cast<SIZE_T>(desc.Width) : 0};
            Check(resource->handle->Map(0, &range, &mapped), "Map placed buffer", device);
            resource->mapped = {static_cast<u8*>(mapped), static_cast<size_t>(desc.Width)};
        }
        return resource;
    }
};
std::shared_ptr<BufferHeapPool> BufferPool(const std::shared_ptr<MemoryBudget>& budget) {
    static std::mutex mutex;
    static std::unordered_map<MemoryBudget*, std::weak_ptr<BufferHeapPool>> pools;
    // Repeated allocations on one device need neither the registry lock nor its sweep.
    // Compare ownership too: a new budget may reuse an expired budget's address.
    thread_local std::weak_ptr<MemoryBudget> last_budget;
    thread_local std::weak_ptr<BufferHeapPool> last_pool;
    if (!last_budget.owner_before(budget) && !budget.owner_before(last_budget)) {
        if (auto pool = last_pool.lock())
            return pool;
    }
    const std::scoped_lock lock{mutex};
    std::erase_if(pools, [](const auto& entry) { return entry.second.expired(); });
    auto& entry = pools[budget.get()];
    auto pool = entry.lock();
    if (!pool) {
        pool = std::make_shared<BufferHeapPool>();
        entry = pool;
    }
    last_budget = budget;
    last_pool = pool;
    return pool;
}
} // namespace
ResourcePtr Context::CreateResource(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap,
                                    D3D12_RESOURCE_STATES state) {
    Collect();
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
    if (allocation.SizeInBytes == UINT64_MAX)
        throw std::runtime_error("D3D12 invalid resource allocation description");
    // This is a pressure target, never a correctness limit. Cache owners may still
    // hold live resources after fence retirement; only the device can reject them.
    const auto target = NativeBufferMemoryTarget(budget->used);
    if (budget->used >= target || allocation.SizeInBytes > target - budget->used) {
        Finish(FrameCount::ForcedSubmitMemoryPressure);
        BufferPool(budget)->Trim();
    }
    auto resource = std::make_shared<Resource>();
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    const auto create = [&]() -> HRESULT {
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
        const auto layout =
            state == D3D12_RESOURCE_STATE_COMMON          ? D3D12_BARRIER_LAYOUT_COMMON
            : state == D3D12_RESOURCE_STATE_COPY_DEST     ? D3D12_BARRIER_LAYOUT_COPY_DEST
            : state == D3D12_RESOURCE_STATE_COPY_SOURCE   ? D3D12_BARRIER_LAYOUT_COPY_SOURCE
            : state == D3D12_RESOURCE_STATE_RENDER_TARGET ? D3D12_BARRIER_LAYOUT_RENDER_TARGET
                                                          : D3D12_BARRIER_LAYOUT_UNDEFINED;
        if (casting_device && heap == D3D12_HEAP_TYPE_DEFAULT &&
            layout != D3D12_BARRIER_LAYOUT_UNDEFINED &&
            desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && desc.SampleDesc.Count == 1 &&
            !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
            std::vector<DXGI_FORMAT> formats;
            const auto storage = std::ranges::find(FormatTable, static_cast<u32>(desc.Format),
                                                   &FormatInfo::resource_format);
            if (storage != FormatTable.end() && storage->block_width == 1 &&
                storage->block_height == 1 && !storage->depth) {
                for (const auto& candidate : FormatTable) {
                    if (candidate.depth || candidate.converted_astc ||
                        candidate.conversion != FormatConversion::None ||
                        candidate.block_width != 1 || candidate.block_height != 1 ||
                        candidate.bytes_per_block != storage->bytes_per_block)
                        continue;
                    const auto format = static_cast<DXGI_FORMAT>(candidate.view_format);
                    if (std::ranges::find(formats, format) != formats.end())
                        continue;
                    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
                    const auto dimension = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                                               ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
                                           : desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                               ? D3D12_FORMAT_SUPPORT1_TEXTURE3D
                                               : D3D12_FORMAT_SUPPORT1_TEXTURE2D;
                    // Preserve native-family SRVs (notably sRGB on an UAV-capable
                    // typeless resource); each descriptor still needs usage support.
                    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                           sizeof(support))) ||
                        !(support.Support1 & dimension) ||
                        (FormatFamily(format) != FormatFamily(desc.Format) &&
                         (((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
                           !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) ||
                          ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
                           !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))))
                        continue;
                    formats.push_back(format);
                }
            }
            if (!formats.empty()) {
                D3D12_RESOURCE_DESC1 extended{};
                extended.Dimension = desc.Dimension;
                extended.Alignment = desc.Alignment;
                extended.Width = desc.Width;
                extended.Height = desc.Height;
                extended.DepthOrArraySize = desc.DepthOrArraySize;
                extended.MipLevels = desc.MipLevels;
                extended.Format = desc.Format;
                extended.SampleDesc = desc.SampleDesc;
                extended.Layout = desc.Layout;
                extended.Flags = desc.Flags;
                const auto result = casting_device->CreateCommittedResource3(
                    &properties, D3D12_HEAP_FLAG_NONE, &extended, layout, nullptr, nullptr,
                    static_cast<UINT32>(formats.size()), formats.data(),
                    IID_PPV_ARGS(&resource->handle));
                if (SUCCEEDED(result)) {
                    resource->castable_formats.assign(formats.begin(), formats.end());
                    return result;
                }
                // A driver can reject a particular list even with OPTIONS12 support.
                // Keep the legacy resource and let the cache select synchronized aliases.
                if (result != E_INVALIDARG && result != DXGI_ERROR_UNSUPPORTED)
                    return result;
                Check(device->GetDeviceRemovedReason(), "format casting device removed",
                      device.Get());
                resource->handle.Reset();
            }
        }
#endif
        return device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state,
                                               nullptr, IID_PPV_ARGS(&resource->handle));
    };
    auto result = create();
    if (result == E_OUTOFMEMORY) {
        Finish(FrameCount::ForcedSubmitAllocationRetry);
        BufferPool(budget)->Trim();
        result = create();
    }
    Check(result, "CreateCommittedResource after fence retirement", device.Get());
    resource->heap = heap;
    resource->state = state;
    resource->allocation_size = allocation.SizeInBytes;
    resource->budget = budget;
    budget->used += allocation.SizeInBytes;
    if (heap != D3D12_HEAP_TYPE_DEFAULT) {
        void* mapped{};
        D3D12_RANGE read_range{0, heap == D3D12_HEAP_TYPE_READBACK ? static_cast<SIZE_T>(desc.Width)
                                                                   : 0};
        Check(resource->handle->Map(0, &read_range, &mapped), "Map staging", device.Get());
        resource->mapped = {static_cast<u8*>(mapped), static_cast<size_t>(desc.Width)};
    }
    return resource;
}
void Context::ValidateViewFormat(const ResourcePtr& resource, DXGI_FORMAT format,
                                 const char* call) const {
    // Buffer descriptors use structured/raw/typed rules instead of texture families.
    if (!resource || resource->Description().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        resource->SupportsViewFormat(format))
        return;
    facade.validation.ViewFormat(call, resource->Description().Format, format,
                                 resource->castable_formats);
    const auto message =
        fmt::format("D3D12_VALIDATION invalid tick={} call={} resource_format={} view_format={} "
                    "resource_family={} view_family={} castable_count={}",
                    CurrentTick(), call, static_cast<u32>(resource->Description().Format),
                    static_cast<u32>(format), FormatFamily(resource->Description().Format),
                    FormatFamily(format), resource->castable_formats.size());
    // Never send a known-invalid descriptor to the runtime, even with validation off.
    throw std::invalid_argument(message);
}
ResourcePtr Context::CreateBuffer(u64 size, D3D12_HEAP_TYPE heap) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // CBVs need a complete final 256-byte unit even for a short guest range.
    desc.Width = Align(std::max(u64{1}, size), 256);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // P3: guest storage buffers are bound as raw UAVs.
    if (heap == D3D12_HEAP_TYPE_DEFAULT)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto state = heap == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                       : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                          : D3D12_RESOURCE_STATE_COMMON;
    auto pool = BufferPool(budget);
    Collect();
    static const bool pressure_reuse = [] {
        char value[2]{};
        return !(GetEnvironmentVariableA("NXBOX_D3D12_BUFFER_PRESSURE_REUSE", value,
                                         sizeof(value)) == 1 &&
                 value[0] == '0');
    }();
    try {
        // Diagnostic: committed buffers only, to rule out placed-heap aliasing.
        static const bool no_placed = BisectEnabled("NXBOX_D3D12_NO_PLACED");
        // Free slots already have backing memory and no outstanding resource lease.
        // Reusing them cannot grow the budget, even when live caches exceed its target.
        auto resource = pressure_reuse && !no_placed
                            ? pool->Allocate(device.Get(), budget, desc, heap, state, pool, false)
                            : ResourcePtr{};
        if (!resource) {
            if (budget->used >= NativeBufferMemoryTarget(budget->used)) {
                Finish(FrameCount::ForcedSubmitMemoryPressure);
                pool->Trim();
            }
            if (!no_placed)
                resource = pool->Allocate(device.Get(), budget, desc, heap, state, pool);
        }
        if (resource) {
            const FrameProfile::Scope profile_alias{profile, FramePhase::Barriers};
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            barrier.Aliasing.pResourceAfter = resource->handle.Get();
            Commands()->ResourceBarrier(1, &barrier);
            profile.Count(FrameCount::Barriers);
            Retain(resource);
            return resource;
        }
    } catch (const std::exception& error) {
        Diagnostic(fmt::format("D3D12 buffer allocation retry reason={}", error.what()));
        Finish(FrameCount::ForcedSubmitAllocationRetry);
        pool->Trim();
        // A committed allocation can succeed when a larger heap cannot fit.
    }
    return CreateResource(desc, heap, state);
}
void StagingRing::Collect(bool trim) {
    const FrameProfile::Scope profile_collect{context.Profile(), FramePhase::StagingCollect};
    context.Profile().Count(FrameCount::StagingCollections);
    context.Collect();
    // One conservative completion snapshot per sweep, rather than a fence/recorder
    // poll for every page. A newly completed page can wait until the next sweep.
    static const bool draw_fastpaths = !BisectEnabled("NXBOX_D3D12_NO_DRAW_FASTPATHS");
    const u64 completed = draw_fastpaths ? context.CompletedTick() : 0;
    const auto is_free = [&](u64 tick) {
        return draw_fastpaths ? tick <= completed : context.IsFree(tick);
    };
    if (trim) {
        // Fence completion alone is insufficient: mapped spans and uniform caches
        // may still own a CPU lease. Only the ring's last reference can be dropped.
        std::erase_if(pages, [&is_free](const Page& page) {
            return is_free(page.tick) && page.resource.use_count() == 1;
        });
    }
    for (auto& page : pages) {
        if (is_free(page.tick) && page.resource.use_count() == 1)
            page.cursor = 0;
    }
}
StagingBufferRef StagingRing::Request(size_t size) {
    const FrameProfile::Scope profile_allocation{context.Profile(), FramePhase::UploadAllocation};
    const bool snapshot = heap == D3D12_HEAP_TYPE_DEFAULT;
    context.Profile().Count(snapshot ? FrameCount::UniformSnapshotRequests
                                     : FrameCount::StagingRequests);
    const auto allocate = [&](u64 bytes) {
        auto resource = context.CreateBuffer(bytes, heap);
        context.Profile().Count(snapshot ? FrameCount::UniformSnapshotNewResources
                                         : FrameCount::StagingNewResources);
        return resource;
    };
    const auto slice = [](const ResourcePtr& resource, size_t offset, size_t bytes) {
        return resource->mapped.empty() ? std::span<u8>{} : resource->mapped.subspan(offset, bytes);
    };
    if (!size)
        throw std::invalid_argument("D3D12 empty staging allocation");
    const u64 aligned = Align(size, 512);
    // Append-only allocations need no fence query or completion callbacks. Reclaim
    // only when existing space is exhausted (or at the explicit frame collection).
    for (auto& page : pages) {
        if (aligned <= page.capacity - page.cursor) {
            if (!snapshot)
                context.Profile().Count(FrameCount::StagingFastRequests);
            const auto offset = page.cursor;
            page.cursor += aligned;
            page.tick = context.CurrentTick();
            return {page.resource, static_cast<size_t>(offset),
                    slice(page.resource, static_cast<size_t>(offset), size)};
        }
    }
    Collect();
    for (auto& page : pages) {
        if (aligned <= page.capacity - page.cursor) {
            const auto offset = page.cursor;
            page.cursor += aligned;
            page.tick = context.CurrentTick();
            return {page.resource, static_cast<size_t>(offset),
                    slice(page.resource, static_cast<size_t>(offset), size)};
        }
    }
    u64 capacity{};
    for (const auto& page : pages)
        capacity += page.capacity;
    const u64 page_size = std::max(4ULL << 20, aligned);
    if (page_size > (64ULL << 20)) {
        // Oversized guest images use a dedicated lease, not a permanent ring page.
        auto buffer = allocate(aligned);
        return {buffer, 0, slice(buffer, 0, size)};
    }
    if (page_size > (64ULL << 20) - capacity) {
        // Evict optional CPU caches before waiting; their hot entries could pin
        // every page indefinitely. GPU/binding leases still prevent early reuse.
        if (release_leases)
            release_leases();
        // Submit the producer before waiting, then reuse completed, unleased pages.
        // A request can arrive with a tight staging lease still held by its caller.
        context.Finish(FrameCount::ForcedSubmitStagingPressure);
        Collect();
        for (auto& page : pages) {
            if (aligned <= page.capacity - page.cursor) {
                const auto offset = page.cursor;
                page.cursor += aligned;
                page.tick = context.CurrentTick();
                return {page.resource, static_cast<size_t>(offset),
                        slice(page.resource, static_cast<size_t>(offset), size)};
            }
        }
        // Drop completed smaller pages to make room for this size class.
        std::erase_if(pages, [&](const Page& page) {
            return context.IsFree(page.tick) && page.resource.use_count() == 1;
        });
        capacity = 0;
        for (const auto& page : pages)
            capacity += page.capacity;
        if (page_size > (64ULL << 20) - capacity) {
            auto buffer = allocate(aligned);
            return {buffer, 0, slice(buffer, 0, size)};
        }
    }
    pages.push_back({allocate(page_size), aligned, context.CurrentTick(), page_size});
    return {pages.back().resource, 0, slice(pages.back().resource, 0, size)};
}
} // namespace D3D12
