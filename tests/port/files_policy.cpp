// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <iostream>
#include <set>
#include "eden_uwp/files_policy.h"
using namespace EdenXbox::Files;
int main() {
    unsigned checks = 0;
    auto check = [&](bool value) {
        ++checks;
        if (!value) {
            std::cerr << "Failed check " << checks << '\n';
            std::abort();
        }
    };
    check(Normalize(L"games/./one/../two") == L"games/two");
    check(Normalize(L"games\\one") == L"games/one");
    check(Normalize(L"games/..") == L"");
    for (auto path :
         {L"../games", L"/games", L"\\server", L"C:games", L"games/../../a", L"games/a:stream",
          L"games/NUL.txt", L"games/a.", L"games/a ", L"games/a?", L"games/COM1.txt"})
        check(!Normalize(path));
    check(!ValidName(L""));
    check(!ValidName(L"a/b"));
    check(!ValidName(std::wstring(L"a\0b", 3)));
    check(ValidName(L"Pokémon 日本語.nsp"));
    check(Join(L"", L"games") == L"games");
    check(Within(L"games/Title", L"GAMES/title/a"));
    check(!Within(L"games/Title", L"games/Title2/a"));
    std::vector<Entry> entries{
        {L"b.nsp", false, 10, 3}, {L"z", true, 0, 1}, {L"A.nsp", false, 30, 2}, {L"a", true, 0, 2}};
    SortEntries(entries, Sort::Name);
    check(entries[0].name == L"a" && entries[1].name == L"z" && entries[2].name == L"A.nsp");
    SortEntries(entries, Sort::Size);
    check(entries[0].directory && entries[1].directory && entries[2].size == 30);
    SortEntries(entries, Sort::Date);
    check(entries[0].name == L"a" && entries[2].name == L"b.nsp");
    unsigned unit = 0;
    check(SizeNumber(0, unit) == L"0" && unit == 0);
    check(SizeNumber(1023, unit) == L"1023" && unit == 0);
    check(SizeNumber(1024, unit) == L"1.0" && unit == 1);
    check(SizeNumber(1536, unit, true) == L"1,5" && unit == 1);
    check(SizeNumber(10ull << 30, unit) == L"10.0" && unit == 3);
    check(ClashName(L"game.nsp", false, 2) == L"game (2).nsp");
    check(ClashName(L"folder.ext", true, 2) == L"folder.ext (2)");
    check(ClashName(L".hidden", false, 2) == L".hidden (2)");
    const std::set<std::wstring> names{L"games/game.nsp", L"games/game (2).nsp"};
    auto exists = [&](const auto& name) { return names.count(name) != 0; };
    auto plan = PlanTransfer(0, L"games/game.nsp", 1, L"games", false, false, false, exists);
    check(plan && plan->destination == L"games/game (3).nsp" && !plan->replace && !plan->move);
    plan = PlanTransfer(0, L"games/game.nsp", 1, L"games", false, true, true, exists);
    check(plan && plan->replace && plan->move);
    check(!PlanTransfer(0, L"games/game.nsp", 0, L"games", false, true, true, exists));
    check(!PlanTransfer(0, L"games/folder", 0, L"games/folder/sub", true, false, false, exists));
    check(!PlanTransfer(0, L"games/folder", 0, L"GAMES/FOLDER/sub", true, false, false, exists));
    check(!PlanTransfer(0, L"", 1, L"games", true, false, false, exists));
    check(!PlanTransfer(0, L"games/game.nsp", 1, L"../games", false, false, false, exists));
    check(!PlanTransfer(0, L"games/game.nsp", 1, L"games", true, false, true, exists));
    check(PlanTransfer(0, L"games/folder", 1, L"games/folder/sub", true, true, false, exists)
              .has_value());
    std::cout << checks << " file policy checks passed\n";
}
