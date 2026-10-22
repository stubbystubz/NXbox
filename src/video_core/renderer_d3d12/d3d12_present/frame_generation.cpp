// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cstring>
#include <fmt/format.h>
#include "d3d12_present_shader_source.h"
#include "video_core/renderer_d3d12/d3d12_present/display_refresh.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation.h"
#include "video_core/renderer_d3d12/d3d12_present/frame_generation_policy.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12::Present {
struct FrameGenerator::Impl {
    Context& context;
    FrameGenerationPolicy policy;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12QueryHeap> timing_heap;
    u64 timing_frequency{};
    bool gpu_budget_missed{};
    double latest_gpu_ms{};
    struct Slot {
        ComPtr<ID3D12DescriptorHeap> heap;
        ResourcePtr readback;
        // Heap ownership lasts through the final dispatch, not just the statistic copy.
        u64 tick{}, sample_tick{}, sample_id{};
        unsigned cells{}, query_pairs{}, generation_mode{2};
        double game_seconds{1.0 / 30};
        bool pending{};
    };
    std::array<Slot, 3> slots;
    unsigned next_slot{};
    Slot* active_slot{};
    unsigned mode{2};
    float fraction{0.5f};
    double refresh_hz{};
    DisplayRefreshEstimate refresh_estimate;
    bool logged_refresh{};
    u64 sample_id{}, completed_sample{};
    double latest_difference{};
    std::array<ComPtr<ID3D12PipelineState>, 4> pipelines;
    ResourcePtr previous, current, real, output, luma, motion, stats, zero, external_motion;
    unsigned width{}, height{}, bx{}, by{};
    unsigned output_width{}, output_height{}, source_width{}, source_height{};
    bool history{}, analyze_only{};
    FrameGenerationEvent last_rejection{FrameGenerationEvent::Failure};
    DXGI_FORMAT history_format{DXGI_FORMAT_R8G8B8A8_UNORM};
    // Why frames were or were not generated, logged every 300 real frames.
    struct Tally {
        unsigned calls{}, slow_or_fast{}, no_history{}, too_different{}, generated{}, measured{};
        double difference_sum{}, difference_max{};
    } tally;
    void Count(unsigned Tally::*reason, double difference = -1) {
        if (analyze_only)
            return;
        tally.*reason += reason == &Tally::generated ? mode - 1 : 1;
        if (difference >= 0) {
            ++tally.measured;
            tally.difference_sum += difference;
            tally.difference_max = std::max(tally.difference_max, difference);
        }
        if (++tally.calls < 300)
            return;
        const unsigned measured = tally.measured;
        Diagnostic(fmt::format("D3D12_FRAME_GEN mode={}x frames={} generated={} rejected_timing={} "
                               "rejected_history={} rejected_difference={} "
                               "difference_mean={:.3f} difference_max={:.3f} gpu_ms={:.3f} "
                               "budget_missed={}",
                               gpu_budget_missed ? 0 : mode, tally.calls, tally.generated,
                               tally.slow_or_fast, tally.no_history, tally.too_different,
                               measured ? tally.difference_sum / measured : 0.0,
                               tally.difference_max, latest_gpu_ms, gpu_budget_missed));
        tally = {};
    }
    explicit Impl(Context& context_) : context{context_} {}
    void Initialize() {
        if (root)
            return;
        if (!analyze_only) {
            D3D12_QUERY_HEAP_DESC timing{};
            timing.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            timing.Count = static_cast<unsigned>(slots.size()) * 8;
            Check(context.Device()->CreateQueryHeap(&timing, IID_PPV_ARGS(&timing_heap)),
                  "Frame generation timing heap");
            Check(context.Queue()->GetTimestampFrequency(&timing_frequency),
                  "Frame generation timing frequency");
        }
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 5};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = {2, ranges};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = {0, 0, 7};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC desc{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, errors;
        Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
              "Frame generation root serialization");
        Check(context.Device()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                    blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "Frame generation root");
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 10;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        for (auto& slot : slots) {
            Check(context.Device()->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&slot.heap)),
                  "Frame generation descriptors");
        }
        ShaderCompiler compiler;
        const auto source = BuildFrameGenerationShaderSource();
        constexpr std::array entries{L"LumaCS", L"MotionCS", L"WarpCS", L"DownsampleCS"};
        for (unsigned i = 0; i < entries.size(); ++i) {
            const auto shader = compiler.Compile(source, entries[i], L"cs_6_0");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = root.Get();
            pso.CS = {shader.data(), shader.size()};
            Check(context.Device()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipelines[i])),
                  "Frame generation compute PSO");
        }
    }
    ResourcePtr Texture(unsigned w, unsigned h, DXGI_FORMAT format, bool uav = true) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        return context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    }
    void ReleaseResources() noexcept {
        previous.reset();
        current.reset();
        real.reset();
        output.reset();
        luma.reset();
        motion.reset();
        stats.reset();
        for (auto& slot : slots)
            slot.readback.reset();
        zero.reset();
        external_motion.reset();
    }
    void Allocate(unsigned w, unsigned h) {
        const auto extent = CheaperHistoryExtent(w, h, source_width, source_height);
        if (output_width == w && output_height == h && width == extent.width &&
            height == extent.height)
            return;
        ResetHistory();
        previous.reset();
        current.reset();
        real.reset();
        output.reset();
        luma.reset();
        motion.reset();
        bx = (extent.width + 7) / 8;
        by = (extent.height + 7) / 8;
        previous = Texture(extent.width, extent.height, history_format);
        current = Texture(extent.width, extent.height, history_format);
        if (extent.width != w || extent.height != h)
            real = Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
        output = analyze_only ? current : Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
        luma = Texture(bx, by, DXGI_FORMAT_R32G32_FLOAT);
        motion = Texture(bx, by, DXGI_FORMAT_R32G32B32A32_FLOAT);
        if (!stats) {
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = 256;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            stats = context.CreateResource(buffer, D3D12_HEAP_TYPE_DEFAULT,
                                           D3D12_RESOURCE_STATE_COMMON);
            for (auto& slot : slots)
                slot.readback = context.CreateBuffer(256, D3D12_HEAP_TYPE_READBACK);
            zero = context.CreateBuffer(256, D3D12_HEAP_TYPE_UPLOAD);
            std::memset(zero->mapped.data(), 0, 256);
        }
        width = extent.width;
        height = extent.height;
        output_width = w;
        output_height = h;
    }
    void Views(Slot& slot) {
        auto cpu = slot.heap->GetCPUDescriptorHandleForHeapStart();
        const auto stride = context.Device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (const auto& resource :
             {previous, current, luma, external_motion ? external_motion : motion,
              real ? real : current}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = resource->handle->GetDesc().Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            context.ValidateViewFormat(resource, view.Format, "Frame generation SRV");
            context.Device()->CreateShaderResourceView(resource->handle.Get(), &view, cpu);
            cpu.ptr += stride;
        }
        for (const auto& resource : {luma, motion, output, stats, current}) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            if (resource == stats) {
                view.Format = DXGI_FORMAT_R32_TYPELESS;
                view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                view.Buffer.NumElements = 64;
                view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            } else {
                view.Format = resource->handle->GetDesc().Format;
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            }
            context.Device()->CreateUnorderedAccessView(resource->handle.Get(), nullptr, &view,
                                                        cpu);
            cpu.ptr += stride;
        }
    }
    void Copy(const ResourcePtr& destination, const ResourcePtr& source) {
        context.Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
        context.Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST);
        context.Retain(source);
        context.Retain(destination);
        context.Commands()->CopyResource(destination->handle.Get(), source->handle.Get());
    }
    void Dispatch(Slot& slot, unsigned pass, unsigned w, unsigned h) {
        context.InvalidateRecordedState();
        context.MarkWork("frame generation");
        auto* commands = context.Commands();
        ID3D12DescriptorHeap* heaps[]{slot.heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(root.Get());
        commands->SetComputeRootDescriptorTable(0, slot.heap->GetGPUDescriptorHandleForHeapStart());
        const std::array<unsigned, 6> constants{width, height, bx, by, output_width, output_height};
        commands->SetComputeRoot32BitConstants(1, 6, constants.data(), 0);
        commands->SetComputeRoot32BitConstants(1, 1, &fraction, 6);
        commands->SetPipelineState(pipelines[pass].Get());
        if (auto* profile = context.PassProfile()) {
            constexpr std::array labels{"frame generation luma", "frame generation motion",
                                        "frame generation warp",
                                        "frame generation history downsample"};
            profile->Label(labels[pass]);
            profile->Target({w, h, static_cast<u32>(output->handle->GetDesc().Format), 0, 0});
        }
        const unsigned query_base = static_cast<unsigned>(&slot - slots.data()) * 8;
        if (timing_heap && (pass == 0 || pass == 2)) {
            if (pass == 0)
                slot.query_pairs = 1;
            else
                ++slot.query_pairs;
            commands->EndQuery(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                               query_base + (slot.query_pairs - 1) * 2);
        }
        commands->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        if (timing_heap && (pass == 1 || pass == 2)) {
            const unsigned pair = slot.query_pairs - 1;
            commands->EndQuery(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                               query_base + pair * 2 + 1);
            context.Retain(slot.readback);
            commands->ResolveQueryData(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                       query_base + pair * 2, 2, slot.readback->handle.Get(),
                                       8 + pair * 16);
        }
        slot.tick = context.CurrentTick();
    }
    void ResetHistory() {
        active_slot = nullptr;
        history = false;
        latest_difference = 0;
        completed_sample = 0;
        // Discard decisions without releasing in-flight heap/readback ownership.
        for (auto& slot : slots)
            slot.pending = false;
    }
    void ReadGpuTime(const Slot& slot) {
        if (!timing_heap || !timing_frequency || slot.query_pairs < 2)
            return;
        u64 ticks = 0;
        for (unsigned pair = 0; pair < slot.query_pairs; ++pair) {
            std::array<u64, 2> timestamps{};
            std::memcpy(timestamps.data(), slot.readback->mapped.data() + 8 + pair * 16, 16);
            if (timestamps[1] >= timestamps[0])
                ticks += timestamps[1] - timestamps[0];
        }
        latest_gpu_ms = double(ticks) * 1000 / timing_frequency;
        // Budget of the sampled real frame; no idle time between interpolated presents
        // is counted. A miss latches generation off for this renderer session.
        if (!gpu_budget_missed &&
            FrameGenerationBudgetMissed(latest_gpu_ms, slot.game_seconds, slot.generation_mode)) {
            gpu_budget_missed = true;
            Diagnostic(fmt::format("D3D12_FRAME_GEN mode=off rejected_timing=gpu_budget "
                                   "gpu_ms={:.3f} requested={}x",
                                   latest_gpu_ms, slot.generation_mode));
        }
    }
    void ReadDifferences() {
        for (auto& slot : slots) {
            if (!slot.pending || !context.IsFree(slot.tick) || !context.IsFree(slot.sample_tick))
                continue;
            if (slot.sample_id > completed_sample) {
                std::uint32_t sum{};
                std::memcpy(&sum, slot.readback->mapped.data(), sizeof(sum));
                latest_difference = double(sum) / (4095.0 * slot.cells);
                completed_sample = slot.sample_id;
            }
            ReadGpuTime(slot);
            slot.pending = false;
        }
    }
    Slot* AcquireSlot() {
        for (unsigned i = 0; i < slots.size(); ++i) {
            const auto index = (next_slot + i) % slots.size();
            auto& slot = slots[index];
            if (slot.tick && !context.IsFree(slot.tick))
                continue;
            next_slot = (index + 1) % slots.size();
            return &slot;
        }
        return nullptr;
    }
    ResourcePtr Generate(const ResourcePtr& source, double seconds) {
        if (seconds > 0 && !policy.CanGenerate(true, seconds, 0)) {
            ResetHistory();
            last_rejection = FrameGenerationEvent::Timing;
            Count(&Tally::slow_or_fast);
            return {}; // Fast games do not copy history or wait for GPU completion.
        }
        Initialize();
        const auto desc = source->handle->GetDesc();
        Allocate(static_cast<unsigned>(desc.Width), desc.Height);
        if (external_motion) {
            const auto motion_desc = external_motion->handle->GetDesc();
            if (motion_desc.Width != bx || motion_desc.Height != by)
                external_motion.reset();
        }
        ReadDifferences();
        if (gpu_budget_missed) {
            last_rejection = FrameGenerationEvent::GpuBudget;
            ResetHistory();
            Count(&Tally::slow_or_fast);
            return {};
        }
        auto* available = AcquireSlot();
        if (!available) {
            last_rejection = FrameGenerationEvent::ResourcePressure;
            ResetHistory();
            Count(&Tally::no_history);
            return {}; // Backpressure skips the effect, never waits for a descriptor heap.
        }
        auto& slot = *available;
        slot.generation_mode = mode;
        slot.game_seconds = seconds > 0 ? seconds : 1.0 / 30;
        if (history)
            std::swap(previous, current);
        Views(slot);
        for (const auto& resource :
             {previous, current, real, output, luma, motion, stats, zero, external_motion})
            context.Retain(resource);
        if (real) {
            Copy(real, source);
            context.Transition(real, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.Transition(current, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Dispatch(slot, 3, width, height);
            context.GlobalUavBarrier();
        } else {
            Copy(current, source);
        }
        const bool had_history = history;
        history = true;
        if (!policy.CanGenerate(had_history, seconds, 0)) {
            last_rejection =
                had_history ? FrameGenerationEvent::Timing : FrameGenerationEvent::History;
            Count(had_history ? &Tally::slow_or_fast : &Tally::no_history);
            return {};
        }
        context.Retain(slot.readback);
        // Use the newest completed earlier sample. A cut may generate one extra
        // midpoint (more if the GPU is behind); startup assumes no difference.
        const double difference = latest_difference;
        context.Transition(previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.Transition(current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Copy(stats, zero);
        context.Transition(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.Transition(luma, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Dispatch(slot, 0, bx, by);
        context.GlobalUavBarrier();
        context.Transition(stats, D3D12_RESOURCE_STATE_COPY_SOURCE);
        context.Commands()->CopyBufferRegion(slot.readback->handle.Get(), 0, stats->handle.Get(), 0,
                                             4);
        slot.tick = slot.sample_tick = context.CurrentTick();
        slot.sample_id = ++sample_id;
        slot.cells = bx * by;
        slot.pending = true;
        if (!policy.CanGenerate(true, seconds, difference)) {
            last_rejection = FrameGenerationEvent::Difference;
            Count(&Tally::too_different, difference);
            return {};
        }
        active_slot = &slot;
        Count(&Tally::generated, difference);
        context.Transition(luma, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!external_motion) {
            context.Transition(motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Dispatch(slot, 1, bx, by);
            context.GlobalUavBarrier();
        } else if (timing_heap) {
            // Close the analysis timestamp when the temporal upscaler already
            // produced this pair's field. No second motion dispatch is needed.
            const unsigned base = static_cast<unsigned>(&slot - slots.data()) * 8;
            context.Commands()->EndQuery(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
            context.Commands()->ResolveQueryData(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                                 base, 2, slot.readback->handle.Get(), 8);
        }
        context.Transition(external_motion ? external_motion : motion,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (analyze_only)
            return current;
        context.Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Dispatch(slot, 2, output_width, output_height);
        context.GlobalUavBarrier();
        return output;
    }
};
FrameGenerator::FrameGenerator(Context& context) : impl{std::make_unique<Impl>(context)} {}
FrameGenerator::~FrameGenerator() = default;
ResourcePtr FrameGenerator::Generate(const ResourcePtr& source, double seconds,
                                     float fraction) noexcept {
    impl->last_rejection = FrameGenerationEvent::Failure;
    if (!std::isfinite(fraction) || fraction <= 0 || fraction > 2)
        return {};
    impl->fraction = fraction;
    if (impl->policy.failed)
        return {};
    try {
        return impl->Generate(source, seconds);
    } catch (const std::exception&) {
        if (impl->policy.Fail())
            Diagnostic("D3D12 frame generation disabled for session: shader/PSO/resource "
                       "preparation failed");
    } catch (...) {
        if (impl->policy.Fail())
            Diagnostic("D3D12 frame generation disabled for session: unknown failure");
    }
    // Context retains recorded uses until completion. Release unused/partial
    // allocations immediately so a failed effect cannot starve the real renderer.
    // Root, heaps and PSOs stay alive until renderer teardown drains the queue.
    impl->ReleaseResources();
    return {};
}
FrameGenerationEvent FrameGenerator::LastRejection() const {
    return impl->last_rejection;
}
bool FrameGenerator::Analyze(const ResourcePtr& source) noexcept {
    impl->analyze_only = true;
    impl->history_format = source->handle->GetDesc().Format;
    return bool(Generate(source, 1.0 / 30));
}
void FrameGenerator::UseMotion(const ResourcePtr& motion) {
    impl->external_motion = motion;
}
void FrameGenerator::Reset() {
    impl->ResetHistory();
}
ResourcePtr FrameGenerator::Motion() const {
    return impl->motion;
}
ResourcePtr FrameGenerator::Previous() const {
    return impl->previous;
}
double FrameGenerator::RefreshHz() const {
    return impl->refresh_hz;
}
unsigned FrameGenerator::Configure(IDXGISwapChain3* swapchain, unsigned requested, bool locked) {
    DXGI_SWAP_CHAIN_DESC desc{};
    const bool specified_refresh = SUCCEEDED(swapchain->GetDesc(&desc)) &&
                                   desc.BufferDesc.RefreshRate.Denominator &&
                                   desc.BufferDesc.RefreshRate.Numerator;
    if (specified_refresh)
        impl->refresh_hz =
            double(desc.BufferDesc.RefreshRate.Numerator) / desc.BufferDesc.RefreshRate.Denominator;
    // A specified output mode is authoritative. For flip-model 0/0, use a
    // sustained counter window rather than a quotient between adjacent frames.
    DXGI_FRAME_STATISTICS sample{};
    if (specified_refresh) {
        impl->refresh_estimate.Reset();
    } else if (SUCCEEDED(swapchain->GetFrameStatistics(&sample))) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart > 0) {
            impl->refresh_estimate.Observe(sample.SyncRefreshCount,
                                           static_cast<double>(sample.SyncQPCTime.QuadPart) /
                                               static_cast<double>(frequency.QuadPart));
        } else {
            impl->refresh_estimate.Reset();
        }
        impl->refresh_hz = impl->refresh_estimate.Hertz();
    } else {
        impl->refresh_estimate.Reset();
        impl->refresh_hz = 0;
    }
    const unsigned mode =
        locked ? requested : DisplayFrameGenerationMode(requested, impl->refresh_hz);
    if (!impl->logged_refresh || mode != impl->mode) {
        ComPtr<IDXGIOutput> output;
        DXGI_OUTPUT_DESC output_desc{};
        const bool found = SUCCEEDED(swapchain->GetContainingOutput(&output)) &&
                           SUCCEEDED(output->GetDesc(&output_desc));
        Diagnostic(fmt::format("D3D12_FRAME_GEN requested={}x mode={}x refresh_hz={:.2f} "
                               "output_found={} refresh_unknown={}",
                               requested, mode, impl->refresh_hz, found, impl->refresh_hz == 0));
        impl->logged_refresh = true;
    }
    impl->policy.locked = locked;
    impl->mode = mode;
    impl->fraction = mode > 1 ? 1.f / mode : .5f;
    return mode;
}
ResourcePtr FrameGenerator::Interpolate(float fraction) noexcept {
    if (!impl->active_slot || impl->policy.failed || !std::isfinite(fraction) || fraction <= 0 ||
        fraction > 2 || impl->active_slot->query_pairs >= 4)
        return {};
    try {
        impl->fraction = fraction;
        for (const auto& image : {impl->previous, impl->current,
                                  impl->external_motion ? impl->external_motion : impl->motion}) {
            impl->context.Transition(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            impl->context.Retain(image);
        }
        impl->context.Retain(impl->output);
        impl->context.Transition(impl->output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl->Dispatch(*impl->active_slot, 2, impl->output_width, impl->output_height);
        impl->context.GlobalUavBarrier();
        return impl->output;
    } catch (...) {
        if (impl->policy.Fail())
            Diagnostic("D3D12 frame generation disabled for session: interpolation failed");
        return {};
    }
}
void FrameGenerator::SetSourceExtent(unsigned width, unsigned height) {
    impl->source_width = width;
    impl->source_height = height;
}
ResourcePtr FrameGenerator::Current() const {
    return impl->real ? impl->real : impl->current;
}
} // namespace D3D12::Present
