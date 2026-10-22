# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute T22's C++ routing, account, social ABI and mock-HTTP policy on the host."""

import base64
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r"""


#include "core/hle/service/nextendo/auth.h"
#include "core/hle/service/nextendo/crypto.h"
#include "core/hle/service/nextendo/friends.h"
#include "core/hle/service/nextendo/token.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
using namespace Service::Nextendo;
const Config enabled{true, "192.0.2.1", "198.51.100.2", "test-client"};
const Identity identity{42,
                        "Mock account",
                        "MOCK-FC",
                        "mock-nex-token",
                        "profile-a",
                        "mock-access-token",
                        "mock-refresh-token",
                        EpochSeconds() + 3600,
                        EpochSeconds() + 86400};
const auto now = Authorization::Clock::time_point{};
Authorization Flow() {
  return {"test-client", std::string(43, 'v'), std::string(43, 'c'),
          std::string(32, 's'), now};
}
std::string Callback() {
  return std::string(CallbackUri) +
         "?code=mock%2Bcode&state=" + std::string(32, 's');
}
int main(int argc, char **argv) {
  assert(argc == 2);
  std::string mode = argv[1];
  if (mode == "routing") {
    for (auto host :
         {"nncs2-lp1.n.n.srv.nintendo.net", "nncs2-a.n.n.srv.nintendo.net",
          "g2b309e01-lp1.s.n.srv.nintendo.net",
          "g2df33d01-lp1.p.srv.nintendo.net"}) {
      auto route = ResolveHost(enabled, host);
      assert(route.destination == Destination::Nat);
      assert(route.address == enabled.nat_address);
    }
    for (auto host :
         {"g2df33d01-lp1.s.n.srv.nintendo.net", "g2-smash.s.n.srv.nintendo.net",
          "api.accounts.nintendo.com", "a.nintendo.net", "a.nintendowifi.net",
          "a.nintendo.co.jp", "demonware.net", "a.demonware.net",
          "nintendo.net", "nintendo.com"}) {
      auto route = ResolveHost(enabled, host);
      assert(route.destination == Destination::Main);
      assert(route.address == enabled.server_address);
    }
    assert(
        ResolveHost(enabled, "G2B309E01-LP1.S.N.SRV.NINTENDO.NET.").address ==
        enabled.nat_address);
    assert(ResolveHost(enabled, "NNCS2-A.N.N.SRV.NINTENDO.NET.").address ==
           enabled.nat_address);
    for (auto host : {"nintendo.net.evil.test", "evilnintendo.net",
                      "nextendo.network", "203.0.113.3", "example.org"}) {
      auto route = ResolveHost(enabled, host);
      assert(route.destination == Destination::Native);
      assert(route.address == host);
    }
    for (auto host : {"nintendo.fr", "a.nintendo.co.uk", "nintendo-europe.com",
                      "nintendoswitch.com.cn", "nintendo.pt"}) {
      assert(ResolveHost(enabled, host).destination == Destination::Blocked);
    }
    for (const std::string host :
         {"", ".", "a..b", "host/url", "https://nintendo.net",
          "nintendo.net:443", "nintendo.net..", "nintendo.net@evil.test",
          "a b"})
      assert(ClassifyHost(host) == Destination::Blocked);
    assert(ClassifyHost(std::string("nintendo.net\0.evil.test", 23)) ==
           Destination::Blocked);
    assert(ClassifyHost(std::string(64, 'a') + ".org") == Destination::Blocked);
    assert(ClassifyHost(std::string(255, 'a')) == Destination::Blocked);
    assert(ClassifyHost(std::string(63, 'a') + ".org") == Destination::Native);
  }
  if (mode == "configuration") {
    assert(!Config{}.enabled);
    assert(!Config{}.RoutesReady());
    assert(enabled.RoutesReady());
    for (auto address :
         {"", "127.0.0.1", "0.0.0.0", "224.0.0.1", "255.255.255.255",
          "169.254.1.1", "256.0.0.1", "01.2.3.4", "1.2.3", "1.2.3.4.5",
          "1234.1.1.1", "nextendo.network", "1.2.3.", ".1.2.3", "1a.2.3.4"})
      assert(!IsServerAddress(address));
    assert(IsServerAddress("192.0.2.1"));
    assert(IsServerAddress("198.51.100.2"));
    auto broken = enabled;
    broken.nat_address = broken.server_address;
    assert(!broken.RoutesReady());
    assert(ResolveHost(broken, "a.nintendo.net").destination ==
           Destination::Blocked);
    broken = enabled;
    broken.server_address.clear();
    assert(ResolveHost(broken, "a.nintendo.com").destination ==
           Destination::Blocked);
    assert(ResolveHost(broken, "a.example.org").destination ==
           Destination::Native);
    broken.enabled = false;
    assert(ResolveHost(broken, "a.nintendo.com").address == "a.nintendo.com");
  }
  if (mode == "auth-success") {
    auto flow = Flow();
    bool stored = false;
    int calls = 0;
    assert(flow.AuthorizeUrl().starts_with(
        "https://nextendo.network/api/oauth/authorize?"));
    assert(flow.AuthorizeUrl().find("code_challenge_method=S256") !=
           std::string::npos);
    assert(flow.AuthorizeUrl().find("scope=identity+friends+presence+game."
                                    "matchmaking&app=nxbox&state=") !=
           std::string::npos);
    auto http = [&](const HttpRequest &request) {
      ++calls;
      if (calls == 2) {
        assert(request.url == "https://nextendo.network/api/nex-token");
        assert(request.method == "GET" && request.body.empty());
        assert(request.headers.at("Authorization") ==
               "Bearer mock-access-token");
        auto game = identity;
        game.pid = 84;
        game.username = "NEX account";
        game.friend_code = "NEX-FC";
        game.nex_token = "fresh-nex-token";
        return HttpResponse{200, game};
      }
      assert(calls == 1);
      assert(request.url == "https://nextendo.network/api/oauth/token");
      assert(request.method == "POST");
      assert(request.headers.empty());
      assert(request.content_type == "application/x-www-form-urlencoded");
      assert(request.body.find("code=mock%2Bcode") != std::string::npos);
      assert(request.body.find("code_verifier=" + std::string(43, 'v')) !=
             std::string::npos);
      assert(request.body.find(
                 "redirect_uri=http%3A%2F%2F127.0.0.1%3A17822%2Fcallback") !=
             std::string::npos);
      auto oauth = identity;
      oauth.pid = 0;
      oauth.nex_token.clear();
      oauth.nex_expires_at = 0;
      return HttpResponse{200, oauth};
    };
    auto store = [&](const Identity &value) {
      stored = true;
      assert(value.pid == 84 && value.username == "NEX account");
      assert(value.friend_code == "NEX-FC" &&
             value.nex_token == "fresh-nex-token");
      assert(value.access_token == identity.access_token);
      assert(value.refresh_token == identity.refresh_token);
      assert(value.access_expires_at == identity.access_expires_at);
      assert(value.nex_expires_at == identity.nex_expires_at);
      assert(value.profile_id == "profile-b");
      return true;
    };
    assert(flow.Complete(Callback(), now, "profile-b", http, store) ==
           AuthResult::Ready);
    assert(calls == 2 && stored);
    assert(flow.Complete(Callback(), now, "profile-b", http, store) ==
           AuthResult::Ready);
    assert(calls == 2);
  }
  if (mode == "phone-auth") {
    const auto state = PhoneState(std::string(22, 'r'), "172.16.0.2");
    assert(state == std::string(22, 'r') + "~172.16.0.2~17822");
    for (auto ip : {"10.0.0.1", "172.31.255.254", "192.168.1.2"})
      assert(IsPrivateIPv4(ip));
    for (auto ip : {"127.0.0.1", "172.15.1.1", "172.32.0.1", "192.169.1.1",
                    "10.01.0.1", "10.0.0.256", "10.0.0.1.2", "10.0.0", "10.0.0.", ""})
      assert(!IsPrivateIPv4(ip));
    assert(PhoneState("short", "10.0.0.1").empty());
    assert(PhoneState(std::string(22, '~'), "10.0.0.1").empty());
    assert(PhoneState(std::string(22, 'r'), "8.8.8.8").empty());
    const auto target = "/callback?code=phone%2Bcode&state=" + UrlEncode(state);
    assert(PhoneCallbackCode(target, state) == "phone+code");
    for (const auto &bad : {target + "&state=duplicate", target + "&code=duplicate",
                            target + "&error=denied", target + "#fragment",
                            std::string("/other?code=x&state=") + state,
                            std::string("/callback?code=%00&state=") + state})
      assert(!PhoneCallbackCode(bad, state));
    assert(!PhoneCallbackCode(target, "wrong"));
    Authorization flow("client", std::string(43, 'v'), std::string(43, 'c'), state,
                       now, PhoneCallbackUri);
    assert(flow.AuthorizeUrl().find("redirect_uri=" + UrlEncode(PhoneCallbackUri)) !=
           std::string::npos);
    assert(flow.Complete(std::string(CallbackUri) + target.substr(9), now,
                         "profile-a", {}, {}) == AuthResult::InvalidCallback);
    int calls = 0;
    assert(flow.Complete(std::string(PhoneCallbackUri) + target.substr(9),
        now + std::chrono::minutes(9), "profile-a", [&](const HttpRequest &request) {
          if (++calls == 1) {
            assert(request.body.find("redirect_uri=" + UrlEncode(PhoneCallbackUri)) !=
                   std::string::npos);
            assert(request.body.find("code=phone%2Bcode") != std::string::npos);
          }
          return HttpResponse{200, identity};
        }, [](const Identity &) { return true; }) == AuthResult::Ready);
    assert(calls == 2);
    Authorization expired("client", std::string(43, 'v'), std::string(43, 'c'), state,
                          now, PhoneCallbackUri);
    assert(expired.Complete(std::string(PhoneCallbackUri) + target.substr(9),
        now + std::chrono::minutes(10), "profile-a", {}, {}) == AuthResult::Expired);
  }
  if (mode == "auth-rejection") {
    int calls = 0;
    auto http = [&](const HttpRequest &) {
      ++calls;
      return HttpResponse{200, identity};
    };
    auto store = [](const Identity &) {
      assert(false);
      return true;
    };
    for (const auto &callback :
         {std::string("https://evil.test/callback?code=x&state=") +
              std::string(32, 's'),
          std::string(CallbackUri) + "?code=x&state=wrong",
          Callback() + "&state=duplicate", Callback() + "#fragment",
          std::string(CallbackUri) + "?code=%ZZ&state=" + std::string(32, 's'),
          std::string(CallbackUri) + "?code=%00&state=" + std::string(32, 's'),
          std::string(CallbackUri) + "?state=" + std::string(32, 's'),
          std::string(CallbackUri) + "?malformed",
          Callback() + "&co%64e=duplicate"}) {
      auto flow = Flow();
      assert(flow.Complete(callback, now, "profile-a", http, store) ==
             AuthResult::InvalidCallback);
    }
    auto flow = Flow();
    assert(flow.Complete(Callback(), now, "", http, store) ==
           AuthResult::InvalidCallback);
    flow = Flow();
    assert(
        flow.Complete(std::string(CallbackUri) +
                          "?error=access_denied&state=" + std::string(32, 's'),
                      now, "profile-a", http, store) == AuthResult::Rejected);
    assert(calls == 0);
    assert(UrlDecode("a+b%2Bc") == "a b+c");
    assert(UrlEncode("a +&/") == "a%20%2B%26%2F");
    assert(!UrlDecode("%"));
    assert(!UrlDecode("%G0"));
    assert(!UrlDecode("%0G"));
    assert(!UrlDecode("%7F"));
    assert(!UrlDecode("\n"));
    assert(!Authorization("", std::string(43, 'v'), std::string(43, 'c'),
                          std::string(32, 's'), now)
                .AuthorizeUrl()
                .size());
    assert(!Authorization("client", "short", std::string(43, 'c'),
                          std::string(32, 's'), now)
                .AuthorizeUrl()
                .size());
  }
  if (mode == "auth-expiry") {
    auto http = [](const HttpRequest &) {
      assert(false);
      return HttpResponse{};
    };
    auto store = [](const Identity &) {
      assert(false);
      return true;
    };
    auto flow = Flow();
    assert(flow.Complete(Callback(), now + std::chrono::minutes(5), "profile-a",
                         http, store) == AuthResult::Expired);
    flow = Flow();
    flow.Cancel();
    assert(flow.Complete(Callback(), now, "profile-a", http, store) ==
           AuthResult::Cancelled);
  }
  if (mode == "auth-failures") {
    for (unsigned status : {0u, 301u, 400u, 401u, 403u, 429u, 500u}) {
      auto flow = Flow();
      assert(flow.Complete(
                 Callback(), now, "profile-a",
                 [&](const HttpRequest &) {
                   return HttpResponse{status, identity};
                 },
                 [](const Identity &) {
                   assert(false);
                   return true;
                 }) == AuthResult::Rejected);
    }
    for (auto invalid :
         {Identity{}, Identity{42, "", "", "", "profile-a"},
          Identity{42, "", "", "token\r\nInjected", "profile-a"},
          Identity{42, "", "", std::string(16385, 'a'), "profile-a"}}) {
      auto flow = Flow();
      assert(
          flow.Complete(
              Callback(), now, "profile-a",
              [&](const HttpRequest &) { return HttpResponse{200, invalid}; },
              [](const Identity &) {
                assert(false);
                return true;
              }) == AuthResult::Rejected);
    }
    auto flow = Flow();
    int calls = 0;
    auto http = [&](const HttpRequest &) -> HttpResponse {
      ++calls;
      throw std::runtime_error("mock timeout");
    };
    assert(flow.Complete(Callback(), now, "profile-a", http,
                         [](const Identity &) { return true; }) ==
           AuthResult::Rejected);
    assert(flow.Complete(Callback(), now, "profile-a", http,
                         [](const Identity &) { return true; }) ==
           AuthResult::Rejected);
    assert(calls == 1);
    flow = Flow();
    assert(flow.Complete(
               Callback(), now, "profile-a",
               [](const HttpRequest &) { return HttpResponse{200, identity}; },
               [](const Identity &) { return false; }) ==
           AuthResult::StorageFailed);
  }
  if (mode == "nex-policy") {
    for (unsigned status : {0u, 301u, 400u, 401u, 403u, 404u, 500u}) {
      auto flow = Flow();
      int calls = 0;
      bool saved = false;
      auto result = flow.Complete(
          Callback(), now, "profile-a",
          [&](const HttpRequest &) {
            return HttpResponse{++calls == 1 ? 200u : status, identity};
          },
          [&](const Identity &value) {
            saved = true;
            assert(value.nex_token == identity.nex_token);
            return true;
          });
      assert(calls == 2);
      assert(saved == (status == 404));
      assert(result ==
             (status == 404 ? AuthResult::Ready : AuthResult::Rejected));
    }
    for (int field = 0; field < 3; ++field) {
      for (const auto &token : {std::string{}, std::string("bad\r\ntoken"),
                                std::string(16385, 'x')}) {
        auto bad = identity;
        if (field == 0)
          bad.access_token = token;
        if (field == 1)
          bad.refresh_token = token;
        if (field == 2)
          bad.nex_token = token;
        auto flow = Flow();
        assert(flow.Complete(
                   Callback(), now, "profile-a",
                   [&](const HttpRequest &) { return HttpResponse{200, bad}; },
                   [](const Identity &) {
                     assert(false);
                     return true;
                   }) == AuthResult::Rejected);
      }
    }
    auto bad = identity;
    bad.nex_expires_at = EpochSeconds();
    assert(!FetchNexToken(
        bad, [&](const HttpRequest &) { return HttpResponse{200, bad}; },
        EpochSeconds()));
    bad = identity;
    bad.access_token = std::string(16385, 'x');
    assert(NexTokenRequest(bad).url.empty());
    assert(SocialRequest("/api/friends", bad, "client").url.empty());
  }
  if (mode == "refresh") {
    assert(!SessionNeedsRefresh(identity));
    auto expiring = identity;
    expiring.access_expires_at = 1301;
    expiring.nex_expires_at = 90000;
    assert(!SessionNeedsRefresh(expiring, 1000));
    expiring.access_expires_at = 1300;
    assert(SessionNeedsRefresh(expiring, 1000));
    expiring.access_expires_at = 999;
    assert(SessionNeedsRefresh(expiring, 1000));
    expiring = identity;
    expiring.nex_expires_at = EpochSeconds();
    assert(SessionNeedsRefresh(expiring));
    int requests = 0, saves = 0;
    Identity stored = identity;
    auto result = RefreshSession(
        identity, "test-client",
        [&](const HttpRequest &request) {
          ++requests;
          if (requests == 1) {
            assert(request.url == "https://nextendo.network/api/oauth/token");
            assert(request.method == "POST" && request.headers.empty());
            assert(request.body == "grant_type=refresh_token&refresh_token="
                                   "mock-refresh-token&client_id=test-client");
            auto rotated = identity;
            rotated.access_token = "new-access";
            rotated.refresh_token = "rotated-refresh";
            return HttpResponse{200, rotated};
          }
          assert(requests == 2 && saves == 1);
          // The old NEX token stays valid until a replacement arrives.
          assert(stored.refresh_token == "rotated-refresh" &&
                 stored.nex_token == identity.nex_token);
          assert(request.url == "https://nextendo.network/api/nex-token");
          assert(request.headers.at("Authorization") == "Bearer new-access");
          auto game = identity;
          game.nex_token = "new-game-token";
          return HttpResponse{200, game};
        },
        [&](const Identity &value) {
          ++saves;
          stored = value;
          return true;
        },
        [] { assert(false); });
    assert(result == AuthResult::Ready && requests == 2 && saves == 2);
    assert(stored.refresh_token == "rotated-refresh" &&
           stored.nex_token == "new-game-token");
    assert(stored.profile_id == identity.profile_id);
    for (unsigned status : {0u, 301u, 400u, 401u, 403u, 500u}) {
      bool cleared = false;
      result = RefreshSession(
          identity, "test-client",
          [&](const HttpRequest &) { return HttpResponse{status, {}}; },
          [](const Identity &) {
            assert(false);
            return true;
          },
          [&] {
            cleared = true;
            stored = {};
            SetIdentity({});
          });
      assert(result == AuthResult::Rejected);
      assert(cleared == (status == 400 || status == 401));
      if (cleared)
        assert(stored.refresh_token.empty() && runtime.identity.pid == 0);
    }
    requests = saves = 0;
    result = RefreshSession(
        identity, "client",
        [&](const HttpRequest &) {
          auto rotated = identity;
          rotated.refresh_token = "keep-rotated-even-if-nex-fails";
          return HttpResponse{++requests == 1 ? 200u : 500u, rotated};
        },
        [&](const Identity &value) {
          ++saves;
          stored = value;
          return true;
        },
        [] { assert(false); });
    assert(result == AuthResult::Rejected && saves == 1);
    assert(stored.refresh_token == "keep-rotated-even-if-nex-fails");
    requests = 0;
    result = RefreshSession(
        identity, "client",
        [&](const HttpRequest &) {
          ++requests;
          return HttpResponse{200, identity};
        },
        [](const Identity &) { return false; }, [] { assert(false); });
    assert(result == AuthResult::StorageFailed && requests == 1);
  }
  if (mode == "runtime") {
    Configure({});
    SetIdentity(identity, [](const Identity &, std::uint64_t,
                             std::string_view) { return "mock-id-token"; });
    assert(!IsEnabled());
    assert(GetIdentity("profile-a").pid == 0);
    assert(GetIdToken("profile-a", 1, "1.0").empty());
    Configure(enabled);
    assert(IsEnabled());
    assert(GetIdentity("profile-a").pid == 42);
    assert(GetIdentity("profile-b").pid == 0);
    assert(GetIdToken("profile-a", 1, "1.0") == "mock-id-token");
    assert(GetIdToken("profile-b", 1, "1.0").empty());
    assert(RouteHost("a.nintendo.net").address == enabled.server_address);
    auto broken = enabled;
    broken.nat_address.clear();
    Configure(broken);
    assert(GetIdentity("profile-a").pid == 0);
    assert(GetIdToken("profile-a", 1, "").empty());
    Configure(enabled);
    SetIdentity(identity);
    assert(GetIdToken("profile-a", 1, "").empty());
    SetIdentity({});
    assert(GetIdentity("profile-a").pid == 0);
  }
  if (mode == "session-race") {
    assert(SameSession(identity, identity));
    assert(!SameSession(Identity{}, Identity{}));
    auto replacement = identity;
    replacement.nex_token = "mock-new-session";
    assert(!SameSession(replacement, identity));
    replacement = identity;
    replacement.access_token = "rotated-access";
    assert(!SameSession(replacement, identity));
    replacement = identity;
    replacement.refresh_token = "rotated-refresh";
    assert(!SameSession(replacement, identity));
    replacement = identity;
    replacement.profile_id = "profile-b";
    assert(!SameSession(replacement, identity));
    replacement = identity;
    replacement.pid = 43;
    assert(!SameSession(replacement, identity));
  }
  if (mode == "social") {
    for (auto path : {"/api/friends", "/api/online-status", "/api/presence",
                      "/api/nex-session"}) {
      auto request = SocialRequest(path, identity, "client");
      assert(request.url.starts_with("https://nextendo.network/api/"));
      assert(request.headers.at("Authorization") == "Bearer mock-access-token");
      assert(request.headers.at("X-Nextendo-Client-Id") == "client");
    }
    assert(SocialRequest("/api/friends", identity, "").headers.size() == 1);
    for (auto path : {"/api/oauth/token", "https://evil.test/api/friends",
                      "/api/friends/../token"})
      assert(SocialRequest(path, identity, "client").url.empty());
    assert(SocialRequest("/api/friends", Identity{}, "client").url.empty());
    assert(
        SocialRequest("/api/friends", identity, "bad\r\nclient").url.empty());
    auto bad = identity;
    bad.access_token = "bad\r\ntoken";
    assert(SocialRequest("/api/friends", bad, "client").url.empty());
    Configure(enabled);
    SetIdentity(identity);
    QueuePresence({"profile-b", 2, 10});
    assert(!runtime.presence.pending);
    QueuePresence({"profile-a", 3, 10});
    assert(!runtime.presence.pending);
    QueuePresence({"profile-a", 2, 10});
    assert(runtime.presence.pending && runtime.presence.program_id == 10);
    runtime.presence.pending = false;
    QueuePresence({"profile-a", 2, 10});
    assert(!runtime.presence.pending);
    QueuePresence({"profile-a", 1, 10});
    assert(runtime.presence.pending);
    runtime.friends.push_back({84, "Mock friend", 2, 10});
    assert(GetFriends("profile-a").size() == 1);
    assert(GetFriends("profile-b").empty());
    auto bytes = SerializeFriend(runtime.friends[0], 10);
    assert(bytes.size() == 512);
    assert(bytes[0] == 84 && bytes[0x10] == 84);
    assert(bytes[0x18] == 'M');
    assert(bytes[0x58] == 2);
    assert(bytes[0x5c] == 1);
    assert(bytes[0x128] == 1);
    assert(bytes[0x120] == 0);
    assert(SerializeFriend(runtime.friends[0], 11)[0x5c] == 0);
    auto long_name = runtime.friends[0];
    long_name.name = std::string(31, 'a') + "é";
    auto unicode = SerializeFriend(long_name, 10);
    assert(unicode[0x18 + 31] == 0);
    assert(SerializeFriend(FriendEntry{}, 10)[0x128] == 0);
    Configure({});
    assert(GetFriends("profile-a").empty());
    assert(!runtime.presence.pending);
    assert(runtime.friends.empty());
  }
  if (mode == "crypto") {
    assert(PkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
           "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    assert(Base64Url("").empty());
    assert(Base64Url("f") == "Zg");
    assert(Base64Url("fo") == "Zm8");
    assert(Base64Url("foo") == "Zm9v");
    auto random = RandomUrl(32);
    assert(random.size() == 43);
    assert(random != RandomUrl(32));
    const auto hex = RandomHex(16);
    assert(hex.size() == 32);
    assert(hex.find_first_not_of("0123456789abcdef") == std::string::npos);
    bool rejected = false;
    try {
      IdTokenSigner signer("invalid mock PEM");
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    assert(rejected);
    auto expired = identity;
    expired.nex_expires_at = 10;
    assert(IdTokenClaims(expired, "", 10, "nonce").empty());
    assert(IdTokenClaims({}, "", 10, "nonce").empty());
    assert(IdTokenClaims(identity, "", -1, "nonce").empty());
    assert(IdTokenClaims(identity, "", 10, "").empty());
    assert(JsonString("\"\\\n") == "\"\\\"\\\\\\u000a\"");
    std::string pem;
    if (const auto path = std::getenv("T22_TEST_SIGNING_KEY_FILE")) {
      std::ifstream file(path);
      pem.assign(std::istreambuf_iterator<char>(file), {});
    }
    IdTokenSigner signer(pem);
    std::cout << signer.Sign(
        IdTokenClaims(identity, "3.0.4", 1000, "mock-nonce"));
  }
}
"""


class NextendoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="nxbox-t22-", dir="/tmp")
        cls.addClassCleanup(cls.build.cleanup)
        cls.directory = Path(cls.build.name)
        cls.source = cls.directory / "nextendo.cpp"
        cls.source.write_text(SOURCE)
        cls.binary = cls.directory / "nextendo"
        openssl = next(
            (
                p
                for p in (Path("/opt/homebrew/opt/openssl@3"), Path("/usr/local/opt/openssl@3"))
                if p.is_dir()
            ),
            None,
        )
        command = [
            "clang++",
            "-std=c++20",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-missing-field-initializers",
            "-I",
            str(ROOT / "src"),
        ]
        if openssl:
            command += ["-I", str(openssl / "include"), "-L", str(openssl / "lib")]
        cls.coverage = bool(os.environ.get("NXBOX_T22_COVERAGE"))
        if cls.coverage:
            command += ["-fprofile-instr-generate", "-fcoverage-mapping"]
        command += [str(cls.source), "-lcrypto", "-pthread", "-o", str(cls.binary)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        if cls.coverage:
            cls.addClassCleanup(cls.save_coverage)

    @classmethod
    def save_coverage(cls):
        target = Path(os.environ["NXBOX_T22_COVERAGE"])
        target.mkdir(parents=True, exist_ok=True)
        profile = cls.directory / "coverage.profdata"
        subprocess.run(
            [
                "xcrun",
                "llvm-profdata",
                "merge",
                "-sparse",
                *map(str, cls.directory.glob("*.profraw")),
                "-o",
                str(profile),
            ],
            check=True,
        )
        result = subprocess.run(
            ["xcrun", "llvm-cov", "export", str(cls.binary), "-instr-profile=" + str(profile)],
            check=True,
            capture_output=True,
            text=True,
        )
        data = json.loads(result.stdout)
        files = [
            entry for entry in data["data"][0]["files"] if "/service/nextendo/" in entry["filename"]
        ]
        (target / "coverage.json").write_text(json.dumps(files, indent=2))
        result = subprocess.run(
            [
                "xcrun",
                "llvm-cov",
                "report",
                str(cls.binary),
                "-instr-profile=" + str(profile),
                "-ignore-filename-regex=nextendo.cpp",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        (target / "coverage.txt").write_text(result.stdout)

    def run_case(self, mode):
        environment = dict(os.environ)
        if self.coverage:
            environment["LLVM_PROFILE_FILE"] = str(self.directory / (mode + "-%p.profraw"))
        return subprocess.run(
            [str(self.binary), mode],
            check=True,
            capture_output=True,
            text=True,
            env=environment,
            timeout=20,
        ).stdout

    def test_ordered_hostname_routes_and_nintendo_blocking(self):
        self.run_case("routing")

    def test_default_off_and_fail_closed_configuration(self):
        self.run_case("configuration")

    def test_mock_http_authorization_and_single_use_code(self):
        self.run_case("auth-success")

    def test_phone_state_callback_and_redirect_uri(self):
        self.run_case("phone-auth")

    def test_callback_origin_csrf_duplicate_fields_and_encoding(self):
        self.run_case("auth-rejection")

    def test_authorization_expiry_and_cancellation(self):
        self.run_case("auth-expiry")

    def test_mock_http_errors_malformed_account_and_vault_failure(self):
        self.run_case("auth-failures")

    def test_nex_endpoint_fallback_only_on_404_and_token_validation(self):
        self.run_case("nex-policy")

    def test_refresh_rotates_persists_before_nex_and_clears_rejected_session(self):
        self.run_case("refresh")

    def test_account_profile_isolation_and_mode_switch(self):
        self.run_case("runtime")

    def test_late_rejection_cannot_invalidate_a_replacement_session(self):
        self.run_case("session-race")

    def test_social_api_allowlist_presence_and_friend_wire_layout(self):
        self.run_case("social")

    def test_pkce_rfc7636_and_rs256_envelope(self):
        token = self.run_case("crypto")
        header, payload, signature = token.split(".")
        decode = lambda text: json.loads(base64.urlsafe_b64decode(text + "=" * (-len(text) % 4)))
        self.assertEqual(
            decode(header), {"alg": "RS256", "kid": "nextendo-baas-key-1", "typ": "id_token"}
        )
        claims = decode(payload)
        self.assertEqual(claims["nnex"], "mock-nex-token")
        self.assertEqual(claims["exp"] - claims["iat"], 10800)
        self.assertEqual(claims["tv"], "3.0.4")
        self.assertTrue(claims["hm"])
        self.assertEqual(
            len(base64.urlsafe_b64decode(signature + "=" * (-len(signature) % 4))), 256
        )

    def test_imported_mock_key_signature_verifies(self):
        private = self.directory / "mock-private.pem"
        public = self.directory / "mock-public.pem"
        signature = self.directory / "mock-signature.bin"
        message = self.directory / "mock-message.txt"
        subprocess.run(
            [
                "openssl",
                "genpkey",
                "-algorithm",
                "RSA",
                "-pkeyopt",
                "rsa_keygen_bits:2048",
                "-out",
                str(private),
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["openssl", "pkey", "-in", str(private), "-pubout", "-out", str(public)],
            check=True,
            capture_output=True,
        )
        environment = dict(os.environ)
        environment["T22_TEST_SIGNING_KEY_FILE"] = str(private)
        if self.coverage:
            environment["LLVM_PROFILE_FILE"] = str(self.directory / "imported-%p.profraw")
        token = subprocess.run(
            [str(self.binary), "crypto"],
            check=True,
            capture_output=True,
            text=True,
            env=environment,
            timeout=20,
        ).stdout
        header, payload, raw_signature = token.split(".")
        message.write_bytes((header + "." + payload).encode())
        signature.write_bytes(
            base64.urlsafe_b64decode(raw_signature + "=" * (-len(raw_signature) % 4))
        )
        subprocess.run(
            [
                "openssl",
                "dgst",
                "-sha256",
                "-verify",
                str(public),
                "-signature",
                str(signature),
                str(message),
            ],
            check=True,
            capture_output=True,
        )
        message.write_bytes(message.read_bytes() + b"tampered")
        result = subprocess.run(
            [
                "openssl",
                "dgst",
                "-sha256",
                "-verify",
                str(public),
                "-signature",
                str(signature),
                str(message),
            ],
            capture_output=True,
        )
        self.assertNotEqual(result.returncode, 0)

    def test_winrt_session_restore_vault_fields_and_social_refresh_wiring(self):
        frontend = (ROOT / "src/eden_uwp/nextendo_online.cpp").read_text()
        # The vault keeps the refresh token and account metadata only; short-lived tokens
        # are fetched again at start.
        for field in ("refresh_token", "profile_id"):
            self.assertRegex(frontend, rf'root\.SetNamedValue\(\s*L"{field}"')
            self.assertIn(f'L"{field}"', frontend[frontend.index("nx::Identity ParseIdentity") :])
        for field in ("access_token", "nex_token", "access_expires_at", "nex_expires_at"):
            self.assertNotRegex(frontend, rf'root\.SetNamedValue\(\s*L"{field}"')
        save = frontend[frontend.index("bool SaveAccount") : frontend.index("void SignOut")]
        self.assertLess(save.index("account = identity"), save.index("vault.Add"))
        startup = frontend[frontend.index("void Initialize") : frontend.index("bool SetEnabled")]
        # Startup must not block the library on the network; the worker refreshes first.
        self.assertNotIn("EnsureFreshSession", startup)
        self.assertIn("std::jthread(PollSocial)", startup)
        enable = frontend[frontend.index("bool SetEnabled") : frontend.index("bool SaveAccount")]
        self.assertNotIn("EnsureFreshSession", enable)
        # A rejected social call only forces a refresh; only the refresh endpoint signs out.
        self.assertNotIn("RemoveAccount(&identity)", frontend)
        self.assertEqual(frontend.count("Reauthorize(identity)"), 3)
        self.assertIn("if (social_worker.joinable())\n        return;", startup)
        self.assertIn("retry_after = tick + std::chrono::seconds(60)", frontend)
        self.assertIn("nx::SessionNeedsRefresh(identity, now)", frontend)
        self.assertRegex(frontend, r"nx::RefreshSession\(\s*identity")
        self.assertRegex(frontend, r"SaveAccount,\s*\[\]\s*\{\s*RemoveAccount\(\);")
        self.assertIn("!EnsureFreshSession()", frontend[frontend.index("void PublishPresence") :])
        self.assertIn("!EnsureFreshSession()", frontend[frontend.index("bool RefreshFriends") :])
        self.assertIn("nx::IsEnabled() && EnsureFreshSession()", frontend)

    def test_live_hooks_and_vault_do_not_depend_on_console_identity(self):
        dns = (ROOT / "src/core/hle/service/sockets/sfdnsres.cpp").read_text()
        self.assertEqual(dns.count("Nextendo::RouteHost(host)"), 2)
        self.assertIn("GetAddressInfo(route.address, service)", dns)
        self.assertIn("GetAddressInfo(route.address, /*service*/ std::nullopt)", dns)
        account = (ROOT / "src/core/hle/service/acc/acc.cpp").read_text()
        self.assertIn("LoadNextendoIdTokenCache(system, ctx, user_id)", account)
        frontend = (ROOT / "src/eden_uwp/nextendo_online.cpp").read_text()
        self.assertIn("PasswordVault", frontend)
        self.assertIn("AllowAutoRedirect(false)", frontend)
        self.assertNotIn("Diagnostic(", frontend)
        self.assertNotIn("LOG_", frontend)
        self.assertNotIn("prod.keys", frontend)
        self.assertNotIn("GetConsoleKeys", frontend)
        for filename in (ROOT / "src/core/hle/service/nextendo").glob("*.h"):
            content = filename.read_text().lower()
            self.assertNotIn("https://dauth", content)
            self.assertNotIn("https://accounts.nintendo", content)

    def test_nifm_client_id_fits_the_four_byte_guest_buffer(self):
        # nn::nifm::ClientId is a u32 passed in 4-byte pointer buffers. With an 8-byte id,
        # GetClientId and IsAnyInternetRequestAccepted returned NIFM 112 and MK8D stayed offline.
        header = (ROOT / "src/core/hle/service/nifm/nifm.h").read_text()
        source = (ROOT / "src/core/hle/service/nifm/nifm.cpp").read_text()
        self.assertIn("const u32 client_id;", header)
        self.assertIn("ctx.GetWriteBufferSize() < sizeof(client_id)", source)
        self.assertIn("buffer.size() < sizeof(client_id)", source)
        self.assertIn("u32 requested_client{};", source)

    def test_linked_nextendo_account_reports_an_online_license(self):
        account = (ROOT / "src/core/hle/service/acc/acc.cpp").read_text()
        self.assertIn("Nextendo::GetIdentity(user_id.RawString()).pid ? 1 : 0", account)
        self.assertIn("rb.Push<u32>(license_kind);", account)

    def test_signed_out_nextendo_keeps_the_local_account_available(self):
        # With Nextendo on and nobody signed in, CheckAvailability failed and Sparking Zero said
        # "no saved data" over an existing save. Only the id token may fail for a signed-out account.
        account = (ROOT / "src/core/hle/service/acc/acc.cpp").read_text()
        check = account[account.index("void CheckAvailability(HLERequestContext& ctx) {\n        // Nextendo"):]
        check = check[: check.index("\n    }\n")]
        self.assertNotIn("ResultUnknown", check)
        account_id = account[account.index("    void GetAccountId(HLERequestContext& ctx) {\n        if (Nextendo"):]
        account_id = account_id[: account_id.index("\n    }\n")]
        self.assertIn("identity.pid) {", account_id)
        self.assertNotIn("ResultUnknown", account_id)

    def test_nsd_fills_the_environment_placeholder(self):
        # MK8D asks nsd for g2b309e01-%.s.n.srv.nintendo.net. Returned unchanged, the name failed
        # DNS (EAI_AGAIN) and the game showed 0056-2306 before reaching the Nextendo NAT route.
        source = (ROOT / "src/core/hle/service/sockets/nsd.cpp").read_text()
        policy = (ROOT / "src/core/hle/service/nextendo/policy.h").read_text()
        self.assertIn('Common::ReplaceAll(fqdn_in, "%", "lp1")', source)
        self.assertIn('host == "g2b309e01-lp1.s.n.srv.nintendo"', policy)

    def test_sfdnsres_runs_nsd_resolve_names_through_nsd(self):
        sfdnsres = (ROOT / "src/core/hle/service/sockets/sfdnsres.cpp").read_text()
        nsd = (ROOT / "src/core/hle/service/sockets/nsd.cpp").read_text()
        # GetHostByName and GetAddrInfo both resolve nncs1-%.n.n.srv.nintendo.net as nncs1-lp1.
        self.assertEqual(
            sfdnsres.count("parameters.use_nsd_resolve ? ResolveFqdn(requested) : requested"), 2
        )
        self.assertIn("std::string ResolveFqdn(const std::string& fqdn_in) {", nsd)
        self.assertNotIn("static std::string ResolveImpl", nsd)

    def test_ssl_peek_reads_into_the_whole_buffer(self):
        # ISslConnection::Peek hands the backend a zeroed size; using it as the length peeked 0
        # bytes and turned every waiting server record into an internal error.
        backend = (ROOT / "src/core/hle/service/ssl/ssl_backend_openssl.cpp").read_text()
        self.assertIn("SSL_peek_ex(ssl, data.data(), data.size(), out_size)", backend)
        self.assertNotIn("SSL_peek(ssl", backend)

    def test_friend_service_answers_mk8d_play_session_calls(self):
        friend = (ROOT / "src/core/hle/service/friend/friend.cpp").read_text()
        for name in ("DeclareOpenOnlinePlaySession", "AddPlayHistory"):
            self.assertIn(f'&IFriendService::{name}, "{name}"', friend)
            self.assertIn(f"void {name}(HLERequestContext& ctx)", friend)

    def test_ssl_connection_keeps_its_socket_after_handing_it_to_the_backend(self):
        # Moving the socket into the backend left ISslConnection::socket empty, so DoHandshake
        # returned ResultNoSocket (0xce7b) for MK8D's TLS connection to its game server.
        ssl = (ROOT / "src/core/hle/service/ssl/ssl.cpp").read_text()
        self.assertIn("backend->SetSocket(socket);", ssl)
        self.assertNotIn("backend->SetSocket(std::move(socket));", ssl)


if __name__ == "__main__":
    unittest.main()
