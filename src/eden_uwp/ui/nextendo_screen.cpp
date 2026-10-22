// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/nextendo_screen.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>

#include "common/scope_exit.h"
#include "core/hle/service/nextendo/crypto.h"
#include "eden_uwp/await_bounded.h"
#include "eden_uwp/nextendo_online.h"
#include "eden_uwp/save_sync/qrcodegen.hpp"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/theme.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {
namespace nx = Service::Nextendo;
namespace sockets = winrt::Windows::Networking::Sockets;
namespace streams = winrt::Windows::Storage::Streams;

// Requires privateNetworkClientServer (already declared in dist/nxbox/AppxManifest.xml).
class PhoneCallback {
    struct Inbox {
        std::mutex mutex;
        std::string state;
        std::optional<std::string> code;
        bool stopped = false;
        bool claimed = false;
        std::vector<sockets::StreamSocket> clients;
    };
    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    sockets::StreamSocketListener listener;

    static winrt::fire_and_forget Receive(std::shared_ptr<Inbox> inbox,
                                          sockets::StreamSocket socket) {
        co_await winrt::resume_background();
        std::optional<std::string> pending;
        try {
            streams::DataReader reader(socket.InputStream());
            reader.InputStreamOptions(streams::InputStreamOptions::Partial);
            std::string request;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 4096 &&
                   std::chrono::steady_clock::now() < deadline) {
                const auto loaded = AwaitBounded(
                    reader.LoadAsync(static_cast<std::uint32_t>(4096 - request.size())),
                    std::chrono::seconds(5));
                if (!loaded || !*loaded)
                    break;
                std::vector<std::uint8_t> bytes(*loaded);
                reader.ReadBytes(bytes);
                request.append(bytes.begin(), bytes.end());
            }
            bool accepted = false;
            if (request.starts_with("GET ") && request.find("\r\n\r\n") != std::string::npos) {
                const auto end = request.find(' ', 4);
                const auto line_end = request.find("\r\n");
                if (end != std::string::npos &&
                    (request.substr(end, line_end - end) == " HTTP/1.1" ||
                     request.substr(end, line_end - end) == " HTTP/1.0")) {
                    std::lock_guard lock(inbox->mutex);
                    auto code = nx::PhoneCallbackCode(std::string_view(request).substr(4, end - 4),
                                                      inbox->state);
                    if (!inbox->stopped && !inbox->claimed && code) {
                        inbox->claimed = true;
                        pending = std::move(code);
                        accepted = true;
                    }
                }
            }
            const std::string body = accepted
                                         ? "<!doctype html><meta charset=utf-8><p>Pronto! Volte "
                                           "para o Xbox. / Done! Go back to your Xbox.</p>"
                                         : "Not found";
            const auto response = std::string("HTTP/1.1 ") +
                                  (accepted ? "200 OK" : "404 Not Found") +
                                  "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: "
                                  "no-store\r\nConnection: close\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\n\r\n" + body;
            streams::DataWriter writer(socket.OutputStream());
            writer.WriteBytes(winrt::array_view<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(response.data()),
                reinterpret_cast<const std::uint8_t*>(response.data() + response.size())));
            AwaitBounded(writer.StoreAsync(), std::chrono::seconds(5));
        } catch (...) {
            // Closing the screen also closes any in-flight reads and writes.
        }
        socket.Close();
        std::lock_guard lock(inbox->mutex);
        std::erase(inbox->clients, socket);
        if (!inbox->stopped && pending)
            inbox->code = std::move(pending);
    }

public:
    explicit PhoneCallback(std::string state) {
        inbox->state = std::move(state);
        listener.ConnectionReceived([box = inbox](const auto&, const auto& args) {
            const auto socket = args.Socket();
            {
                std::lock_guard lock(box->mutex);
                if (box->stopped || box->clients.size() >= 8) {
                    socket.Close();
                    return;
                }
                box->clients.push_back(socket);
            }
            Receive(box, socket);
        });
    }
    winrt::Windows::Foundation::IAsyncOperation<bool> Start() {
        co_await listener.BindServiceNameAsync(L"17822");
        co_return true;
    }
    std::optional<std::string> TakeCode() {
        std::lock_guard lock(inbox->mutex);
        return std::exchange(inbox->code, std::nullopt);
    }
    void Stop() {
        listener.Close();
        std::vector<sockets::StreamSocket> clients;
        {
            std::lock_guard lock(inbox->mutex);
            inbox->stopped = true;
            clients.swap(inbox->clients);
        }
        for (const auto& socket : clients)
            socket.Close();
    }
    ~PhoneCallback() {
        Stop();
    }
};

std::string PrivateAddress() {
    using namespace winrt::Windows::Networking;
    for (const auto& host : Connectivity::NetworkInformation::GetHostNames()) {
        if (host.Type() == HostNameType::Ipv4) {
            const auto ip = winrt::to_string(host.CanonicalName());
            if (nx::IsPrivateIPv4(ip))
                return ip;
        }
    }
    return {};
}
} // namespace

bool RunNextendoScreen(Renderer& renderer, const winrt::Windows::UI::Core::CoreWindow& window,
                       Input& input) {
    namespace nx = Service::Nextendo;
    bool closed = false;
    const auto token = window.Closed([&](const auto&, const auto&) { closed = true; });
    SCOPE_EXIT {
        window.Closed(token);
    };
    std::unique_ptr<TextEntry> keyboard;
    std::unique_ptr<nx::Authorization> authorization;
    std::future<Text> task;
    std::shared_ptr<PhoneCallback> phone;
    std::optional<qrcodegen::QrCode> qr;
    bool browser_flow = false;
    std::string phone_state;
    auto phone_deadline = nx::Authorization::Clock::time_point{};
    const auto stop_phone = [&] {
        if (phone)
            phone->Stop();
        phone.reset();
        qr.reset();
    };
    SCOPE_EXIT {
        stop_phone();
    };
    // Without a linked account games that go online fail with a Nintendo error code, so say so.
    const auto idle_feedback = [] {
        return NextendoOnline::LinkedAccount().pid != 0 ? Text::NextendoInstructions
                                                        : Text::NextendoNeedsSignIn;
    };
    Text feedback = idle_feedback();
    // All network/vault operations run off the UI thread, and errors are fixed localized keys.
    const auto run = [&](auto work) {
        task = std::async(std::launch::async, [work] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            SCOPE_EXIT {
                winrt::uninit_apartment();
            };
            try {
                return work();
            } catch (...) {
                return Text::NextendoFailed;
            }
        });
    };
    const auto complete = [&](std::string callback) {
        stop_phone();
        const auto profile = NextendoOnline::CurrentProfileId();
        const auto flow = std::shared_ptr<nx::Authorization>(std::move(authorization));
        run([flow, callback = std::move(callback), profile] {
            const auto result =
                flow->Complete(callback, nx::Authorization::Clock::now(), profile,
                               NextendoOnline::Exchange, NextendoOnline::SaveAccount);
            return result == nx::AuthResult::Ready ? Text::NextendoSignedIn : Text::NextendoFailed;
        });
    };
    while (!closed) {
        window.Dispatcher().ProcessEvents(
            winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
        input.Update();
        bool busy = task.valid();
        if (busy && task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            feedback = task.get();
            if (feedback == Text::NextendoPhoneHint && !authorization)
                feedback = idle_feedback();
            busy = false;
            if (feedback == Text::NextendoFailed && phone) {
                stop_phone();
                authorization->Cancel();
            }
            input.ConsumeUntilRelease();
        }
        if (phone && input.Pressed(Button::B)) {
            stop_phone();
            authorization->Cancel();
            authorization.reset();
            feedback = idle_feedback();
            input.ConsumeUntilRelease();
        }
        if (phone && !busy) {
            if (auto code = phone->TakeCode()) {
                // Nextendo accepts only loopback redirects for emulator clients: the device that
                // signs in forwards its 127.0.0.1:17822 callback to this LAN listener.
                complete(std::string(nx::CallbackUri) + "?code=" + nx::UrlEncode(*code) +
                         "&state=" + nx::UrlEncode(phone_state));
                busy = true;
            } else if (nx::Authorization::Clock::now() >= phone_deadline) {
                stop_phone();
                authorization->Cancel();
                feedback = Text::NextendoPhoneExpired;
            }
        }
        if (keyboard) {
            const auto result = keyboard->Update(input);
            if (result == TextEntry::Result::Cancelled) {
                authorization->Cancel();
                authorization.reset();
                browser_flow = false;
                keyboard.reset();
                feedback = idle_feedback();
            } else if (result == TextEntry::Result::Confirmed) {
                const auto callback = winrt::to_string(keyboard->Text());
                keyboard.reset();
                complete(callback);
            }
        } else if (!busy) {
            if (input.Pressed(Button::B)) {
                if (!authorization)
                    return false;
                stop_phone();
                authorization->Cancel();
                authorization.reset();
                feedback = idle_feedback();
            }
            if (input.Pressed(Button::A) && !authorization) {
                feedback = NextendoOnline::SetEnabled(!nx::IsEnabled()) ? idle_feedback()
                                                                        : Text::NextendoFailed;
            }
            if (input.Pressed(Button::X)) {
                const bool browser = authorization != nullptr;
                browser_flow = browser;
                stop_phone();
                if (NextendoOnline::LinkedAccount().pid != 0) {
                    run([] {
                        NextendoOnline::SignOut();
                        return Text::NextendoSignedOut;
                    });
                } else {
                    const auto config = nx::GetConfig();
                    if (!config.RoutesReady() || config.client_id.empty()) {
                        feedback = Text::NextendoNotConfigured;
                    } else {
                        try {
                            if (authorization)
                                authorization->Cancel();
                            const auto verifier = nx::RandomUrl(32);
                            phone_state = browser
                                              ? nx::RandomUrl(24)
                                              : nx::PhoneState(nx::RandomUrl(16), PrivateAddress());
                            // Leave X available for the browser fallback when LAN discovery fails.
                            if (phone_state.empty()) {
                                authorization = std::make_unique<nx::Authorization>(
                                    config.client_id, verifier, nx::PkceChallenge(verifier),
                                    nx::RandomUrl(24), nx::Authorization::Clock::now());
                                feedback = Text::NextendoPhoneUnavailable;
                                continue;
                            }
                            authorization = std::make_unique<nx::Authorization>(
                                config.client_id, verifier, nx::PkceChallenge(verifier),
                                phone_state, nx::Authorization::Clock::now(), nx::CallbackUri);
                            const auto url = authorization->AuthorizeUrl();
                            if (!browser) {
                                qr = qrcodegen::QrCode::encodeText(url.c_str(),
                                                                   qrcodegen::QrCode::Ecc::MEDIUM);
                                phone = std::make_shared<PhoneCallback>(phone_state);
                                // A loopback authorization expires after five minutes.
                                phone_deadline =
                                    nx::Authorization::Clock::now() + std::chrono::minutes(5);
                                run([receiver = phone] {
                                    const auto started =
                                        AwaitBounded(receiver->Start(), std::chrono::seconds(15));
                                    return started && *started ? Text::NextendoPhoneHint
                                                               : Text::NextendoFailed;
                                });
                            } else {
                                run([url] {
                                    const auto operation =
                                        winrt::Windows::System::Launcher::LaunchUriAsync(
                                            winrt::Windows::Foundation::Uri(
                                                winrt::to_hstring(url)));
                                    const auto opened =
                                        AwaitBounded(operation, std::chrono::seconds(15));
                                    return opened && *opened ? Text::NextendoReturnHint
                                                             : Text::NextendoFailed;
                                });
                            }
                        } catch (...) {
                            stop_phone();
                            feedback = Text::NextendoFailed;
                        }
                    }
                }
            }
            if (input.Pressed(Button::Menu) && authorization && browser_flow && !phone) {
                keyboard = std::make_unique<TextEntry>(
                    Tr(Text::NextendoCallbackTitle),
                    winrt::to_hstring(std::string(nx::CallbackUri) + '?').c_str());
            }
            if (input.Pressed(Button::Y) && !authorization) {
                run([] {
                    return NextendoOnline::RefreshFriends() ? Text::NextendoFriendsLoaded
                                                            : Text::NextendoFailed;
                });
            }
        }
        renderer.BeginFrame();
        renderer.FillRounded(D2D1::RectF(0, 0, kCanvasWidth, kCanvasHeight), 0, Theme::kBackground);
        DrawClock(renderer);
        renderer.DrawString(Tr(Text::NextendoTitle), Font::TitleSmall,
                            D2D1::RectF(kScreenMargin, 168, 1800, 310), Theme::kText);
        renderer.DrawString(Tr(nx::IsEnabled() ? Text::NextendoOn : Text::NextendoOff),
                            Font::Heading, D2D1::RectF(kScreenMargin, 340, 1800, 400),
                            Theme::kText);
        renderer.DrawString(Tr(busy ? Text::NextendoBusy : feedback), Font::Body,
                            D2D1::RectF(kScreenMargin, 430, qr ? 1080 : 1800, 640),
                            Theme::kTextSecondary);
        const auto identity = NextendoOnline::LinkedAccount();
        const auto friends = NextendoOnline::Friends();
        std::wstring social = Widen(identity.username);
        for (std::size_t i = 0; i < friends.size() && i < 4; ++i) {
            social += L"\n" + Widen(friends[i].name) + L" · " +
                      Tr(friends[i].status ? Text::NextendoOn : Text::NextendoOff);
        }
        renderer.DrawString(social, Font::Body,
                            D2D1::RectF(kScreenMargin, 660, qr ? 1080 : 1800, 935),
                            Theme::kTextSecondary);
        if (qr && !busy) {
            const float cell = 560.0f / (qr->getSize() + 8);
            const float x = 1190.0f, y = 350.0f;
            renderer.FillRounded(D2D1::RectF(x, y, x + 560, y + 560), 0, Theme::kPrimaryFill);
            for (int row = 0; row < qr->getSize(); ++row)
                for (int col = 0; col < qr->getSize(); ++col)
                    if (qr->getModule(col, row))
                        renderer.FillRounded(D2D1::RectF(x + (col + 4) * cell, y + (row + 4) * cell,
                                                         x + (col + 5) * cell,
                                                         y + (row + 5) * cell),
                                             0, Theme::kTextOnLight);
            renderer.DrawString(L"nextendo.network", Font::Body,
                                D2D1::RectF(x, y + 575, x + 560, y + 630), Theme::kTextSecondary);
        }
        DrawHints(renderer, {{Theme::kButtonA, L"A", Tr(Text::NextendoToggle)},
                             {Theme::kButtonX, L"X",
                              Tr(identity.pid    ? Text::HintSignOut
                                 : authorization ? Text::NextendoBrowser
                                                 : Text::HintSignIn)},
                             {Theme::kButtonY, L"Y", Tr(Text::NextendoFriends)},
                             {Theme::kButtonA, L"☰", Tr(Text::NextendoEnterReturn)},
                             {Theme::kButtonB, L"B", Tr(Text::HintBack)}});
        if (keyboard)
            keyboard->Draw(renderer, Clock::now());
        renderer.EndFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return true;
}
} // namespace EdenXbox::Ui
