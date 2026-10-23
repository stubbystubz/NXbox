// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG // The checks below must run in Release test builds too.
#include "eden_uwp/setup_policy.h"
#include <cassert>
#include <map>
#include <stdexcept>

using namespace EdenXbox;
namespace Sources = FirmwareManager::Sources;
int main() {
  const auto file = [](const char *path) {
    return Sources::Entry{path, false, false, 1, 0};
  };
  const auto directory = [](const char *path, bool reparse = false) {
    return Sources::Entry{path, true, reparse, 0, 0};
  };
  std::map<std::filesystem::path, std::vector<Sources::Entry>> tree{
      {"local",
       {directory("local/empty"), directory("local/firmware"),
        directory("local/.firmware_extract-1"), directory("local/eden")}},
      {"local/firmware",
       {file("local/firmware/A.NCA"), directory("local/firmware/b.nca")}},
      {"local/eden", {directory("local/eden/keys")}},
      {"local/eden/keys", {file("local/eden/keys/prod.keys")}},
      {"usb1",
       {directory("usb1/random"), directory("usb1/denied"),
        directory("usb1/link", true), directory("usb1/__MACOSX")}},
      {"usb1/random",
       {file("usb1/random/Firmware.ZIP"), file("usb1/random/TITLE.KEYS"),
        file("usb1/random/._fake.nca"), file("usb1/random/not-keys.txt"),
        file("usb1/random/unsupported.rar"), file("usb1/random/titles.zip")}},
      {"usb2",
       {directory("usb2/backup"), directory("usb2/games"),
        directory("usb2/eMMC")}},
      {"usb2/backup",
       {file("usb2/backup/rawnand.bin"), file("usb2/backup/rawnand.bin.01")}},
      {"usb2/eMMC", {file("usb2/eMMC/00"), file("usb2/eMMC/01")}},
      {"usb2/games",
       {file("usb2/games/Game.NSP"), file("usb2/games/Game.XCI"),
        file("usb2/games/Game.NSZ")}},
  };
  Sources::IO io;
  std::vector<std::filesystem::path> visited;
  io.list = [&](const auto &path) {
    visited.push_back(path);
    if (path == "usb1/denied")
      throw std::runtime_error("denied");
    return tree[path];
  };
  const auto candidates = Setup::Scan(io, {"local", "usb1", "usb2", "usb1"});
  const auto has = [&](Setup::Row row, const char *path) {
    return std::count_if(
        candidates.begin(), candidates.end(),
        [&](const auto &item) { return item.row == row && item.path == path; });
  };
  assert(candidates.size() == 7);
  assert(has(Setup::Row::Firmware, "local/firmware") == 1);
  assert(has(Setup::Row::Keys, "local/eden/keys/prod.keys") == 1);
  assert(has(Setup::Row::Keys, "usb1/random/TITLE.KEYS") == 1);
  assert(has(Setup::Row::Firmware, "usb1/random/Firmware.ZIP") == 1);
  assert(has(Setup::Row::Firmware, "usb1/random/titles.zip") == 0);
  assert(has(Setup::Row::Firmware, "usb2/backup/rawnand.bin") == 1);
  assert(has(Setup::Row::Firmware, "usb2/eMMC/00") == 1);
  assert(has(Setup::Row::Games, "usb2/games") == 1);
  for (const auto *skipped :
       {"usb1/link", "usb1/__MACOSX", "local/.firmware_extract-1",
        "local/firmware/b.nca"})
    assert(std::find(visited.begin(), visited.end(), skipped) == visited.end());
  assert(Setup::Scan(io, {"absent"}).empty());
  // Deep trees stop after eight descents, without depending on host
  // permissions.
  std::size_t visits = 0;
  io.list = [&](const auto &path) {
    ++visits;
    return std::vector<Sources::Entry>{{path / "deep", true, false, 0, 0}};
  };
  assert(Setup::Scan(io, {"root"}).empty());
  assert(visits == 9);

  Setup::State state;
  state.Install();
  assert(state.phase == Setup::Phase::Ready);
  state.Select();
  assert(state.phase == Setup::Phase::Picking);
  assert(!state.Continue());
  state.Back();
  assert(state.phase == Setup::Phase::Ready);
  state.Confirm();
  assert(state.phase == Setup::Phase::Confirm);
  assert(!state.Continue());
  state.Back();
  state.Select();
  state.Confirm();
  state.Install();
  assert(state.phase == Setup::Phase::Installing);
  state.Back();
  state.Select();
  state.Confirm();
  assert(state.phase == Setup::Phase::Installing);
  assert(!state.Continue());
  state.Finish();
  assert(state.phase == Setup::Phase::Result);
  state.Select();
  state.Back();
  assert(state.Continue());
  assert(state.phase == Setup::Phase::Done);
  // No readiness prerequisite: all three rows may still be missing.
  Setup::State empty;
  assert(empty.Continue());
  Setup::State scanning;
  scanning.phase = Setup::Phase::Scanning;
  assert(!scanning.Continue());
  scanning.Finish();
  assert(scanning.Continue());
}
