# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise bounded guest-thread accounting independently of the Windows SDK."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class GuestThreadProfileTests(unittest.TestCase):
    def test_accounting_windows_overflow_and_failure(self):
        source = r"""
#include <cassert>
#include <stdexcept>
#include <string_view>
#include "core/hle/kernel/nxbox_thread_profile.h"
using Kernel::NxboxThreadProfile::Profile;
using namespace std::chrono_literals;
int main() {
    const char* option = std::getenv("NXBOX_THREAD_PROFILE");
    assert(Kernel::NxboxThreadProfile::Enabled() ==
           (option && std::strcmp(option, "1") == 0));
    const auto begin = Profile::Clock::time_point{};
    Profile p{begin}, other{begin};
    p.Run(begin, begin + 250us);
    p.Run(begin, begin + 500us);
    // Complete on another host core: only the guest-owned profile is updated.
    p.Svc(0x18, begin + 1s, begin + 1020ms);
    p.Svc(0x18, begin, begin + 100us);
    p.Svc(999, begin, begin + 20us);
    p.Sync(0x1234, begin + 1s, begin + 1019ms);
    p.Sync(0x1234, begin, begin + 50us);
    p.Sync(0, begin, begin + 10us);
    for (unsigned i = 1; i <= 20; ++i) p.Sync(i, begin, begin + 1us);
    unsigned reports = 0, sync_entries = 0;
    p.Poll([](auto...) { assert(false); }, begin + 4999ms);
    p.Poll([&](std::string_view kind, auto key, const auto& c, auto interval) {
        ++reports;
        assert(interval == 6000);
        if (kind == "run") {
            assert(c.calls == 2 && c.total_us == 750 && c.max_us == 500);
        } else if (kind == "svc") {
            if (key == 0x18) assert(c.calls == 2 && c.total_us == 20100 && c.max_us == 20000);
            else assert(key == 128 && c.calls == 1 && c.total_us == 20);
        } else if (kind == "sync_object") {
            ++sync_entries;
            if (key == 0x1234) assert(c.calls == 2 && c.total_us == 19050);
        } else {
            assert(kind == "sync_overflow" && c.calls == 6 && c.total_us == 6);
        }
    }, begin + 6s);
    assert(reports == 20 && sync_entries == 16);
    p.Poll([&](auto, auto, const auto& c, auto interval) {
        assert(c.calls == 0 && interval == 5000);
    }, begin + 11s);
    other.Poll([&](auto, auto, const auto& c, auto) { assert(c.calls == 0); }, begin + 11s);
    p.Svc(1, begin, begin + 1s);
    p.Poll([](auto...) { throw std::runtime_error("logger unavailable"); }, begin + 16s);
    p.Poll([&](auto, auto, const auto& c, auto) { assert(c.calls == 0); }, begin + 21s);
    assert(Profile::Elapsed(begin + 1s, begin) == 0);
}
"""
        with tempfile.TemporaryDirectory() as temp:
            cpp = Path(temp) / "profile.cpp"
            binary = Path(temp) / "profile"
            cpp.write_text(source)
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    f"-I{ROOT / 'src'}",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            for option in (None, "0", "1", "true"):
                env = os.environ.copy()
                env.pop("NXBOX_THREAD_PROFILE", None)
                if option is not None:
                    env["NXBOX_THREAD_PROFILE"] = option
                subprocess.run([str(binary)], env=env, check=True, timeout=30)

    def test_wiring_preserves_guest_ownership_and_wait_identity(self):
        core = (ROOT / "src/core/hle/kernel/physical_core.cpp").read_text()
        wait = (ROOT / "src/core/hle/kernel/svc/svc_synchronization.cpp").read_text()
        finalize = (ROOT / "src/core/hle/kernel/k_thread.cpp").read_text()
        self.assertIn("NxboxThreadProfile::Enabled() && !thread->nxbox_thread_profile", core)
        self.assertIn("profile->Run(run_start", core)
        self.assertIn("profile->Svc(svc_number", core)
        self.assertIn("nxbox_thread_profile.reset()", finalize)
        self.assertIn("R_SUCCEEDED(res) && *out_index >= 0", wait)
        self.assertIn("objs[*out_index]", wait)
        self.assertIn("profile->Sync(object", wait)
        for path, label in (
            ("kernel_helpers.cpp", "event->GetReadableEvent()"),
            ("vi/application_display_service.cpp", "name=DisplayVsync"),
        ):
            text = (ROOT / "src/core/hle/service" / path).read_text()
            self.assertIn("THREAD_PROFILE event_object=", text)
            self.assertIn(label, text)


if __name__ == "__main__":
    unittest.main()
