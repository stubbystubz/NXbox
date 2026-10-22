// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_present/recorder.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <roapi.h>
#include "eden_uwp/recorder_policy.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12::Present {
namespace {
namespace R = EdenXbox::Recording;
using Clock = std::chrono::steady_clock;
double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}
constexpr std::string_view Shader = R"(
Texture2D<float4> image : register(t0);
SamplerState linear_sampler : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv.x * 2 - 1, 1 - v.uv.y * 2, 0, 1);
    return v;
}
float4 PS(Vertex v) : SV_Target { return image.SampleLevel(linear_sampler, v.uv, 0); }
)";
void ReserveDisk(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    fs::create_directories(directory);
    std::vector<fs::directory_entry> files;
    std::uintmax_t bytes = 0;
    for (const auto& file : fs::directory_iterator(directory)) {
        if (file.is_regular_file() && file.path().extension() == ".mp4") {
            bytes += file.file_size();
            files.push_back(file);
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return a.last_write_time() < b.last_write_time();
    });
    for (const auto& file : files) {
        if (bytes + R::ClipReserve <= R::ArchiveBytes &&
            fs::space(directory).available >= R::ClipReserve)
            break;
        const auto size = file.file_size();
        fs::remove(file.path());
        bytes -= size;
    }
    if (bytes + R::ClipReserve > R::ArchiveBytes || fs::space(directory).available < R::ClipReserve)
        throw std::runtime_error("recordings need 192 MiB of free space");
}
// BT.709 limited-range NV12. Input is the GPU-scaled RGBA8 display, top row first.
void ToNv12(const u8* rgba, unsigned pitch, unsigned width, unsigned height,
            std::vector<BYTE>& nv12) {
    nv12.resize(size_t(width) * height * 3 / 2);
    const auto byte = [](int value) { return static_cast<BYTE>(std::clamp(value, 0, 255)); };
    const auto pixel = [&](unsigned x, unsigned y) {
        return reinterpret_cast<const BYTE*>(rgba + size_t(y) * pitch + x * 4);
    };
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const auto* p = pixel(x, y);
            nv12[size_t(y) * width + x] =
                byte(16 + ((47 * p[0] + 157 * p[1] + 16 * p[2] + 128) >> 8));
        }
    }
    for (unsigned y = 0; y < height; y += 2) {
        for (unsigned x = 0; x < width; x += 2) {
            int red = 0, green = 0, blue = 0;
            for (unsigned j = 0; j < 2; ++j)
                for (unsigned i = 0; i < 2; ++i) {
                    const auto* p = pixel(x + i, y + j);
                    red += p[0];
                    green += p[1];
                    blue += p[2];
                }
            red /= 4;
            green /= 4;
            blue /= 4;
            const auto offset = size_t(width) * height + size_t(y / 2) * width + x;
            nv12[offset] = byte(128 + ((-26 * red - 87 * green + 112 * blue + 128) >> 8));
            nv12[offset + 1] = byte(128 + ((112 * red - 102 * green - 10 * blue + 128) >> 8));
        }
    }
}
struct Encoder {
    ComPtr<IMFSinkWriter> writer;
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    DWORD stream{};
    bool texture{};
    std::string kind{"software"};
    R::Options options;

    void Open(const std::filesystem::path& file, R::Options config, bool gpu) {
        writer.Reset();
        manager.Reset();
        device.Reset();
        options = config;
        texture = gpu;
        ComPtr<IMFAttributes> attributes;
        Check(MFCreateAttributes(&attributes, 4), "RECORDER attributes");
        Check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, gpu),
              "RECORDER hardware");
        if (gpu) {
            ComPtr<ID3D11DeviceContext> immediate;
            D3D_FEATURE_LEVEL level;
            Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    nullptr, 0, D3D11_SDK_VERSION, &device, &level, &immediate),
                  "RECORDER D3D11 device");
            UINT token{};
            Check(MFCreateDXGIDeviceManager(&token, &manager), "RECORDER device manager");
            Check(manager->ResetDevice(device.Get(), token), "RECORDER reset manager");
            Check(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager.Get()),
                  "RECORDER manager");
        }
        Check(MFCreateSinkWriterFromURL(file.c_str(), nullptr, attributes.Get(), &writer),
              "RECORDER sink writer");
        ComPtr<IMFMediaType> output, input;
        Check(MFCreateMediaType(&output), "RECORDER output type");
        Check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "RECORDER video");
        Check(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "RECORDER H264");
        Check(output->SetUINT32(MF_MT_AVG_BITRATE, options.Width() == 1920 ? 12000000 : 8000000),
              "RECORDER bitrate");
        Check(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
              "RECORDER progressive");
        Check(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, options.Width(), options.Height()),
              "RECORDER size");
        Check(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, options.Fps(), 1),
              "RECORDER rate");
        Check(MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "RECORDER aspect");
        Check(output->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709),
              "RECORDER primaries");
        Check(output->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "RECORDER matrix");
        Check(output->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235),
              "RECORDER range");
        Check(writer->AddStream(output.Get(), &stream), "RECORDER add stream");
        Check(MFCreateMediaType(&input), "RECORDER input type");
        Check(output->CopyAllItems(input.Get()), "RECORDER input attributes");
        Check(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "RECORDER NV12");
        Check(input->DeleteItem(MF_MT_AVG_BITRATE), "RECORDER raw bitrate");
        Check(writer->SetInputMediaType(stream, input.Get(), nullptr), "RECORDER input");
        Check(writer->BeginWriting(), "RECORDER begin");
        kind = "software";
        if (gpu) {
            kind = "unknown";
            ComPtr<IMFSinkWriterEx> extended;
            if (SUCCEEDED(writer.As(&extended))) {
                for (DWORD i = 0; i < 8; ++i) {
                    GUID category{};
                    ComPtr<IMFTransform> transform;
                    if (FAILED(extended->GetTransformForStream(stream, i, &category, &transform)))
                        break;
                    if (category != MFT_CATEGORY_VIDEO_ENCODER)
                        continue;
                    ComPtr<IMFAttributes> attrs;
                    UINT32 length{};
                    if (SUCCEEDED(transform->GetAttributes(&attrs)))
                        kind = SUCCEEDED(
                                   attrs->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length))
                                   ? "hardware"
                                   : "software";
                }
            }
        }
    }
    void Write(const std::vector<BYTE>& nv12, LONGLONG timestamp) {
        ComPtr<IMFMediaBuffer> buffer;
        if (texture) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = options.Width();
            desc.Height = options.Height();
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_NV12;
            desc.Usage = D3D11_USAGE_DEFAULT;
            D3D11_SUBRESOURCE_DATA data{nv12.data(), options.Width(), 0};
            ComPtr<ID3D11Texture2D> surface;
            Check(device->CreateTexture2D(&desc, &data, &surface), "RECORDER NV12 texture");
            Check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), surface.Get(), 0, FALSE,
                                            &buffer),
                  "RECORDER surface buffer");
            ComPtr<IMF2DBuffer> planar;
            DWORD length{};
            Check(buffer.As(&planar), "RECORDER planar buffer");
            Check(planar->GetContiguousLength(&length), "RECORDER planar length");
            Check(buffer->SetCurrentLength(length), "RECORDER surface length");
        } else {
            Check(MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &buffer),
                  "RECORDER memory buffer");
            BYTE* bits{};
            Check(buffer->Lock(&bits, nullptr, nullptr), "RECORDER lock");
            std::memcpy(bits, nv12.data(), nv12.size());
            Check(buffer->Unlock(), "RECORDER unlock");
            Check(buffer->SetCurrentLength(static_cast<DWORD>(nv12.size())), "RECORDER length");
        }
        ComPtr<IMFSample> sample;
        Check(MFCreateSample(&sample), "RECORDER sample");
        Check(sample->AddBuffer(buffer.Get()), "RECORDER sample buffer");
        Check(sample->SetSampleTime(timestamp), "RECORDER time");
        Check(sample->SetSampleDuration(10000000 / options.Fps()), "RECORDER duration");
        Check(writer->WriteSample(stream, sample.Get()), "RECORDER write sample");
    }
};
} // namespace

struct Recorder::Impl {
    Context& context;
    struct Slot {
        ResourcePtr target, readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::atomic<bool> queued{};
        u64 tick{};
        LONGLONG timestamp{};
        std::uint64_t sequence{};
    };
    std::array<Slot, R::Slots> slots;
    R::Ring ring;
    R::Timeline timeline;
    const R::Options options{R::options};
    ComPtr<ID3D12DescriptorHeap> rtvs, srvs;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    UINT rtv_stride{}, srv_stride{};
    std::atomic<bool> desired{}, ready{}, quit{}, failed{}, capturing{};
    bool initialized{}, was_ready{};
    unsigned seen_toggles{};
    std::uint64_t sequence{};
    std::thread worker;

    explicit Impl(Context& ctx) : context{ctx} {
        const char* automatic = std::getenv("NXBOX_RECORD");
        const auto seconds = R::Seconds(automatic);
        timeline.duration = seconds ? seconds : R::MaxSeconds;
        timeline.delay = R::Seconds(std::getenv("NXBOX_RECORD_DELAY"));
        timeline.fps = options.Fps();
        timeline.automatic = automatic ? seconds != 0 : options.mode != 0;
    }
    ~Impl() {
        desired.store(false);
        quit.store(true);
        if (worker.joinable())
            worker.join();
        R::active.store(false);
    }
    void Initialize() {
        auto* device = context.Device();
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.NumDescriptors = R::Slots;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvs)), "RECORDER RTV heap");
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srvs)), "RECORDER SRV heap");
        rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srv_stride =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{1, &parameter, 1, &sampler,
                                            D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, errors;
        Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
              "RECORDER serialize root");
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&root)),
              "RECORDER root");
        ShaderCompiler compiler;
        const auto vs = compiler.Compile(Shader, L"VS", L"vs_6_0");
        const auto ps = compiler.Compile(Shader, L"PS", L"ps_6_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.VS = {vs.data(), vs.size()};
        desc.PS = {ps.data(), ps.size()};
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.DepthStencilState.StencilReadMask = desc.DepthStencilState.StencilWriteMask = 0xff;
        desc.DepthStencilState.FrontFace =
            desc.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                               D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)),
              "RECORDER pipeline");
        for (unsigned i = 0; i < R::Slots; ++i) {
            auto& slot = slots[i];
            D3D12_RESOURCE_DESC texture{};
            texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texture.Width = options.Width();
            texture.Height = options.Height();
            texture.DepthOrArraySize = texture.MipLevels = 1;
            texture.SampleDesc.Count = 1;
            texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            slot.target = context.CreateResourceNoWait(texture, D3D12_HEAP_TYPE_DEFAULT,
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            UINT64 bytes{};
            device->GetCopyableFootprints(&texture, 0, 1, 0, &slot.footprint, nullptr, nullptr,
                                          &bytes);
            D3D12_RESOURCE_DESC readback{};
            readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readback.Width = bytes;
            readback.Height = readback.SampleDesc.Count = 1;
            readback.DepthOrArraySize = readback.MipLevels = 1;
            readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            slot.readback = context.CreateResourceNoWait(readback, D3D12_HEAP_TYPE_READBACK,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
            auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += i * rtv_stride;
            device->CreateRenderTargetView(slot.target->handle.Get(), nullptr, rtv);
        }
        initialized = true;
    }
    void Capture(const ResourcePtr& source, D3D12_CPU_DESCRIPTOR_HANDLE display, bool game) {
        capturing.store(true);
        struct CaptureGuard {
            std::atomic<bool>& flag;
            ~CaptureGuard() {
                flag.store(false);
            }
        } capture_guard{capturing};
        if (failed.load())
            return;
        const double now = Now();
        const auto toggles = R::toggles.load();
        const bool automatic = game && timeline.AutoStart(now);
        const bool manual = ((toggles - seen_toggles) & 1u) != 0;
        if (manual || (automatic && !desired.load())) {
            const bool start = manual ? !desired.load() : true;
            if (start)
                R::pending.store(true);
            desired.store(start);
        }
        seen_toggles = toggles;
        const bool recording = ready.load() && desired.load();
        if (recording && !was_ready)
            timeline.Start(now);
        was_ready = recording;
        if (recording && timeline.Expired(now))
            desired.store(false);
        if (!desired.load())
            return;
        if (!worker.joinable())
            worker = std::thread([this] { Run(); });
        if (!recording)
            return;
        // A small red recording dot at the top right, included in the captured display.
        context.Transition(source, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto extent = source->handle->GetDesc();
        if (extent.Width >= 48 && extent.Height >= 48) {
            const LONG right = static_cast<LONG>(extent.Width) - 24;
            std::array<D3D12_RECT, 12> dot;
            for (LONG y = 0; y < 12; ++y) {
                const LONG half = static_cast<LONG>(std::sqrt(36.f - (y - 5.5f) * (y - 5.5f)));
                dot[y] = {right - 6 - half, 24 + y, right - 6 + half, 25 + y};
            }
            constexpr float red[]{1.f, .08f, .08f, 1.f};
            context.Commands()->ClearRenderTargetView(display, red, static_cast<UINT>(dot.size()),
                                                      dot.data());
        }
        if (!timeline.Due(now))
            return;
        const int index = ring.Acquire();
        if (index < 0)
            return;
        struct SlotGuard {
            R::Ring& ring;
            unsigned index;
            bool published{};
            ~SlotGuard() {
                if (!published)
                    ring.Release(index);
            }
        } slot_guard{ring, static_cast<unsigned>(index)};
        auto& slot = slots[index];
        context.Transition(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context.Transition(slot.target, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto srv = srvs->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += index * srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        context.ValidateViewFormat(source, view.Format, "RECORDER display SRV");
        context.Device()->CreateShaderResourceView(source->handle.Get(), &view, srv);
        auto gpu_srv = srvs->GetGPUDescriptorHandleForHeapStart();
        gpu_srv.ptr += index * srv_stride;
        auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += index * rtv_stride;
        const D3D12_VIEWPORT viewport{0, 0, float(options.Width()), float(options.Height()), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(options.Width()), LONG(options.Height())};
        context.InvalidateRecordedState();
        auto* commands = context.Commands();
        ID3D12DescriptorHeap* heaps[]{srvs.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootSignature(root.Get());
        commands->SetPipelineState(pipeline.Get());
        commands->SetGraphicsRootDescriptorTable(0, gpu_srv);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        context.Transition(slot.target, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = slot.target->handle.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = slot.readback->handle.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = slot.footprint;
        context.BeforeCopyWrite(slot.readback, 0, slot.readback->mapped.size());
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        context.Retain(slot.target);
        context.Retain(slot.readback);
        context.Retain(source);
        slot.tick = context.PendingFenceTick();
        slot.timestamp = timeline.Timestamp(now);
        slot.sequence = ++sequence;
        slot.queued.store(true, std::memory_order_release);
        slot_guard.published = true;
    }
    void Run() noexcept {
        const HRESULT com = RoInitialize(RO_INIT_MULTITHREADED);
        bool mf = false;
        try {
            Check(com, "RECORDER WinRT apartment");
            // MF startup is deferred until a recording is requested.
            Encoder encoder;
            std::filesystem::path file;
            std::vector<BYTE> nv12;
            std::uint64_t frames = 0, drop_start = 0;
            double encode_ms = 0, opened = 0;
            auto close = [&] {
                if (!encoder.writer) {
                    R::pending.store(false);
                    return;
                }
                const auto status = encoder.writer->Finalize();
                encoder.writer.Reset();
                Diagnostic("RECORDER stopped file=" + file.string() +
                           " frames=" + std::to_string(frames) +
                           " drops=" + std::to_string(ring.drops.load() - drop_start) +
                           " encoder=" + encoder.kind +
                           " average_encode_ms=" + std::to_string(frames ? encode_ms / frames : 0) +
                           " finalize_hr=" + std::to_string(status) + " audio=none");
                if (FAILED(status) || !frames) {
                    std::error_code ec;
                    std::filesystem::remove(file, ec);
                }
                ready.store(false);
                R::active.store(false);
                R::pending.store(false);
            };
            try {
                while (true) {
                    if (R::stop_requested.exchange(false))
                        desired.store(false);
                    if (desired.load() && !encoder.writer && !quit.load()) {
                        R::pending.store(true);
                        if (!mf) {
                            Check(MFStartup(MF_VERSION), "RECORDER MFStartup");
                            mf = true;
                        }
                        if (R::local_state.empty())
                            throw std::runtime_error("LocalState is not configured");
                        const auto directory = R::local_state / "recordings";
                        ReserveDisk(directory);
                        // Wait only on the worker when a rapid restart hits the same second.
                        // Keep the required filename format and never truncate a prior clip.
                        do {
                            const auto time = std::time(nullptr);
                            std::tm calendar{};
                            localtime_s(&calendar, &time);
                            file = directory / R::FileName(R::title_id, calendar);
                            if (!std::filesystem::exists(file))
                                break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        } while (desired.load() && !quit.load());
                        if (!desired.load() || quit.load())
                            continue;
                        const char* software = std::getenv("NXBOX_RECORD_SOFTWARE");
                        const bool gpu = !software || std::string_view{software} != "1";
                        try {
                            encoder.Open(file, options, gpu);
                        } catch (const std::exception& error) {
                            if (!gpu)
                                throw;
                            Diagnostic(std::string(
                                           "RECORDER texture setup unavailable; using CPU NV12: ") +
                                       error.what());
                            encoder.writer.Reset();
                            std::error_code ec;
                            std::filesystem::remove(file, ec);
                            encoder.Open(file, options, false);
                        }
                        frames = 0;
                        // D3D12 device creation/map/PSO calls are thread-safe. This
                        // path never touches Context command recording or collection.
                        if (!initialized)
                            Initialize();
                        encode_ms = 0;
                        drop_start = ring.drops.load();
                        opened = Now();
                        Diagnostic("RECORDER started file=" + file.string() +
                                   " encoder=" + encoder.kind +
                                   " input=" + (encoder.texture ? "D3D11-NV12" : "CPU-NV12") +
                                   " size=" + std::to_string(options.Width()) + "x" +
                                   std::to_string(options.Height()) +
                                   " fps=" + std::to_string(options.Fps()) + " audio=none");
                        ready.store(true, std::memory_order_release);
                        R::active.store(true);
                    }
                    if (encoder.writer && Now() - opened >= timeline.duration)
                        desired.store(false);
                    // Closing the ready gate before inspecting the producer prevents a late
                    // publication from racing Finalize. Capture checks it inside its guard.
                    if (!desired.load())
                        ready.store(false);
                    Slot* pending = nullptr;
                    unsigned selected = 0;
                    for (unsigned i = 0; i < R::Slots; ++i) {
                        if (slots[i].queued.load(std::memory_order_acquire) &&
                            (!pending || slots[i].sequence < pending->sequence)) {
                            pending = &slots[i];
                            selected = i;
                        }
                    }
                    const bool complete = pending && context.IsFree(pending->tick);
                    if (pending && quit.load() && !complete) {
                        // The owner already drained Context, or reported a device/submit failure.
                        // A fence for a failed submission must not hang renderer teardown.
                        pending->queued.store(false);
                        ring.Release(selected);
                        continue;
                    }
                    if (pending && complete) {
                        const double begin = Now();
                        ToNv12(pending->readback->mapped.data() + pending->footprint.Offset,
                               pending->footprint.Footprint.RowPitch, options.Width(),
                               options.Height(), nv12);
                        try {
                            encoder.Write(nv12, pending->timestamp);
                        } catch (const std::exception&) {
                            if (frames || !encoder.texture)
                                throw;
                            encoder.writer.Reset();
                            std::error_code ec;
                            std::filesystem::remove(file, ec);
                            encoder.Open(file, options, false);
                            Diagnostic(
                                "RECORDER texture input rejected; encoder=software input=CPU-NV12");
                            encoder.Write(nv12, pending->timestamp);
                        }
                        encode_ms += (Now() - begin) * 1000;
                        ++frames;
                        pending->queued.store(false, std::memory_order_release);
                        ring.Release(selected);
                        // Reserve includes mux overhead; terminate before a clip can exhaust it.
                        if (frames % options.Fps() == 0 &&
                            std::filesystem::file_size(file) > R::ClipReserve - 8 * 1024 * 1024)
                            desired.store(false);
                    } else {
                        // Re-scan after the producer guard: it may have published a slot
                        // after the earlier scan and then left Capture before this check.
                        bool drained = !capturing.load();
                        if (drained) {
                            for (const auto& owned : ring.busy)
                                drained = drained && !owned.load(std::memory_order_acquire);
                        }
                        if (!desired.load() && drained)
                            close();
                        if (quit.load() && drained)
                            break;
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(encoder.writer ? 2 : 10));
                    }
                }
                close();
            } catch (...) {
                close();
                throw;
            }
        } catch (const std::exception& error) {
            if (!initialized) {
                for (auto& slot : slots) {
                    slot.target.reset();
                    slot.readback.reset();
                }
            }
            Diagnostic(std::string("RECORDER failed: ") + error.what());
            failed.store(true);
            desired.store(false);
            ready.store(false);
            R::active.store(false);
            R::pending.store(false);
        } catch (...) {
            Diagnostic("RECORDER failed: unknown exception");
            failed.store(true);
            desired.store(false);
            ready.store(false);
            R::active.store(false);
            R::pending.store(false);
        }
        if (mf)
            MFShutdown();
        if (SUCCEEDED(com))
            RoUninitialize();
    }
};
Recorder::Recorder(Context& context) {
    try {
        impl = std::make_unique<Impl>(context);
    } catch (...) {
        Diagnostic("RECORDER unavailable: worker allocation/start failed");
        R::pending.store(false);
        R::active.store(false);
    }
}
Recorder::~Recorder() = default;
void Recorder::Capture(const ResourcePtr& source, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                       bool game_frame) {
    if (!impl)
        return;
    try {
        impl->Capture(source, rtv, game_frame);
    } catch (const std::exception& error) {
        impl->failed.store(true);
        impl->desired.store(false);
        if (!impl->worker.joinable()) {
            R::pending.store(false);
            R::active.store(false);
        }
        Diagnostic(std::string("RECORDER capture disabled: ") + error.what());
    }
}
} // namespace D3D12::Present
