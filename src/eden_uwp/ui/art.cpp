// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include <iterator>
#include <windows.h>
#include "eden_uwp/ui/art.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <wincodec.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>
#include <wrl/client.h>
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/ui/art_index.h"
#include "eden_uwp/ui/renderer.h"

namespace EdenXbox::Ui {
namespace fs = std::filesystem;
namespace {
std::vector<std::uint8_t> Download(const std::string& url) {
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Storage::Streams;
    constexpr auto timeout = std::chrono::seconds(15);
    Filters::HttpBaseProtocolFilter filter;
    filter.AllowAutoRedirect(false);
    filter.AllowUI(false);
    filter.CookieUsageBehavior(Filters::HttpCookieUsageBehavior::NoCookies);
    HttpClient client{filter};
    HttpRequestMessage message{HttpMethod::Get(),
                               winrt::Windows::Foundation::Uri{winrt::to_hstring(url)}};
    const auto response = AwaitBounded(
        client.SendRequestAsync(message, HttpCompletionOption::ResponseHeadersRead), timeout);
    if (!response || response->StatusCode() != HttpStatusCode::Ok)
        return {};
    const auto stream = AwaitBounded(response->Content().ReadAsInputStreamAsync(), timeout);
    if (!stream)
        return {};
    Buffer buffer{64u << 10};
    std::vector<std::uint8_t> bytes;
    for (;;) {
        const auto read = AwaitBounded(
            stream->ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial), timeout);
        if (!read || read->Length() > (16u << 20) - bytes.size())
            return {};
        if (!read->Length())
            break;
        bytes.insert(bytes.end(), read->data(), read->data() + read->Length());
    }
    return bytes;
}
} // namespace

bool CacheBitmap(const fs::path& file, const Pixels& pixels) {
    const auto bytes = Art::EncodeBitmap(pixels.bgra, pixels.width, pixels.height);
    return !bytes.empty() && Art::WriteBytes(file, bytes);
}

namespace {
std::int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::int64_t ReadStamp(const fs::path& file) {
    std::int64_t stamp = 0;
    std::ifstream(file) >> stamp;
    return stamp;
}
void WriteStamp(const fs::path& file) {
    const auto value = std::to_string(NowSeconds());
    Art::WriteBytes(file, {value.begin(), value.end()});
}
Art::BackgroundIndex ParseIndex(const std::vector<std::uint8_t>& bytes) {
    using namespace winrt::Windows::Data::Json;
    if (bytes.empty())
        return {};
    const auto object =
        JsonObject::Parse(winrt::to_hstring(std::string(bytes.begin(), bytes.end())));
    Art::BackgroundIndex index;
    for (const auto& pair : object) {
        std::vector<std::string> files;
        for (const auto& value : pair.Value().GetArray()) {
            auto file = winrt::to_string(value.GetString());
            if (!Art::SafeFilename(file))
                return {};
            files.push_back(std::move(file));
        }
        if (!files.empty())
            index.emplace(winrt::to_string(pair.Key()), std::move(files));
    }
    return index;
}
std::vector<std::uint8_t> ReadBytes(const fs::path& file) {
    std::error_code ec;
    if (fs::file_size(file, ec) > (16u << 20) || ec)
        return {};
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
// Decode and center-crop on the worker, keeping full-resolution images off the UI thread.
bool HeroBitmap(const std::vector<std::uint8_t>& bytes, const fs::path& file) {
    using Microsoft::WRL::ComPtr;
    if (bytes.empty())
        return false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    UINT width = 0, height = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                            &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width ||
        !height || width > 16384 || height > 16384)
        return false;
    const double scale = std::max(1920.0 / width, 760.0 / height);
    const auto w = static_cast<UINT>(std::ceil(width * scale));
    const auto h = static_cast<UINT>(std::ceil(height * scale));
    if (w > 16384 || h > 16384 || FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    WICRect crop{static_cast<INT>((w - 1920) / 2), static_cast<INT>((h - 760) / 2), 1920, 760};
    Pixels pixels{1920, 760, std::vector<std::uint8_t>(1920 * 760 * 4)};
    return SUCCEEDED(converter->CopyPixels(&crop, 1920 * 4, static_cast<UINT>(pixels.bgra.size()),
                                           pixels.bgra.data())) &&
           CacheBitmap(file, pixels);
}
} // namespace

struct BackgroundArtSource::Impl {
    struct Job {
        std::string id, name, english;
    };
    explicit Impl(fs::path local) : folder(std::move(local) / "community-art") {}
    fs::path folder;
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stop{false};
    std::map<std::string, ArtState> states;
    std::map<std::string, std::int64_t> retry;
    std::vector<Job> stack;
    Art::BackgroundIndex index;
    bool loaded = false;
    std::int64_t next_refresh = 0;
    fs::path File(const std::string& id) const {
        return folder / (id + ".hero-v1.bmp");
    }
    void LoadIndex() {
        if (stop)
            return;
        const auto file = folder / "backgrounds.json";
        if (!loaded) {
            try {
                index = ParseIndex(ReadBytes(file));
            } catch (...) {
            }
            if (index.empty()) {
                auto previous = file;
                previous += ".previous";
                try {
                    index = ParseIndex(ReadBytes(previous));
                } catch (...) {
                }
            }
            loaded = true;
        }
        const auto now = NowSeconds();
        if (now < next_refresh ||
            (!index.empty() && Art::Fresh(ReadStamp(folder / "index.checked"), now)))
            return;
        next_refresh = now + 3600; // Retry failures without hammering the source.
        try {
            const auto bytes = Download("https://raw.githubusercontent.com/NspxMiguel/NXbox/main/"
                                        "dist/art/backgrounds.json");
            auto fresh = ParseIndex(bytes);
            // Replace only after validation, preserving the old copy on any failure.
            if (!fresh.empty()) {
                if (!stop && Art::ReplaceKeepingOld(file, bytes)) {
                    index = std::move(fresh);
                    WriteStamp(folder / "index.checked");
                }
            }
        } catch (...) {
        }
    }
    ArtState Fetch(const Job& job) {
        std::error_code ec;
        if (fs::is_regular_file(File(job.id), ec))
            return ArtState::Ready;
        LoadIndex();
        const auto miss = folder / (job.id + ".missing");
        if (Art::Fresh(ReadStamp(miss), NowSeconds()))
            return ArtState::None;
        // An unavailable index is a transient transport failure, not a title miss.
        if (index.empty() || stop)
            return ArtState::None;
        const auto files = Art::Lookup(index, job.name, job.english);
        for (const auto& filename : files) {
            if (stop)
                return ArtState::None;
            try {
                const auto bytes = Download("https://images.launchbox-app.com/" + filename);
                if (!stop && HeroBitmap(bytes, File(job.id))) {
                    fs::remove(miss, ec);
                    return ArtState::Ready;
                }
            } catch (...) {
            }
        }
        if (!stop)
            WriteStamp(miss);
        return ArtState::None;
    }
    void Loop() {
        bool apartment = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
            while (!stop) {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, std::chrono::hours(1),
                                  [&] { return stop || !stack.empty(); });
                    if (stop)
                        break;
                    if (stack.empty()) {
                        lock.unlock();
                        LoadIndex();
                        continue;
                    }
                    job = std::move(stack.back());
                    stack.pop_back();
                }
                auto result = ArtState::None;
                try {
                    result = Fetch(job);
                } catch (...) {
                }
                const auto stamp = ReadStamp(folder / (job.id + ".missing"));
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    states[job.id] = result;
                    retry[job.id] = Art::Fresh(stamp, NowSeconds()) ? stamp + Art::kCacheWeek
                                                                    : NowSeconds() + 3600;
                }
                // Publish a cached hero immediately; refresh metadata afterwards.
                LoadIndex();
            }
        } catch (...) {
            stop = true;
        }
        if (apartment)
            winrt::uninit_apartment();
    }
};
BackgroundArtSource::BackgroundArtSource(fs::path local)
    : impl_(std::make_shared<Impl>(std::move(local))) {
    std::thread([impl = impl_] { impl->Loop(); }).detach();
}
BackgroundArtSource::~BackgroundArtSource() {
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->wake.notify_all();
}
void BackgroundArtSource::Request(const std::string& id, const std::string& name,
                                  const std::string& english) {
    if (id.size() != 16 || id.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos)
        return;
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stop)
            return;
        const auto state = impl_->states.find(id);
        if (state != impl_->states.end() &&
            (state->second != ArtState::None || NowSeconds() < impl_->retry[id]))
            return;
        impl_->states[id] = ArtState::Pending;
        impl_->stack.push_back({id, name, english});
    }
    impl_->wake.notify_one();
}
ArtState BackgroundArtSource::Get(const std::string& id, fs::path& file) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->states.find(id);
    if (found == impl_->states.end() || impl_->stop)
        return ArtState::None;
    if (found->second == ArtState::Ready)
        file = impl_->File(id);
    return found->second;
}
} // namespace EdenXbox::Ui
