# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise guest language mapping, inheritance, and persistence without WinRT."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <fstream>
#include <sstream>
#include "eden_uwp/title_settings.h"
using namespace EdenXbox;
using Settings::Language;
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::pair<const char*, Language> cases[]{
        {"ja-JP", Language::Japanese}, {"en-US", Language::EnglishAmerican},
        {"en", Language::EnglishAmerican}, {"en-CA", Language::EnglishAmerican},
        {"en-GB", Language::EnglishBritish}, {"en-AU", Language::EnglishBritish},
        {"en-NZ", Language::EnglishBritish}, {"en-ZA", Language::EnglishBritish},
        {"es-MX", Language::SpanishLatin}, {"es-419", Language::SpanishLatin},
        {"es-US", Language::SpanishLatin}, {"es-AR", Language::SpanishLatin},
        {"es-ES", Language::Spanish}, {"es", Language::Spanish},
        {"pt-BR", Language::PortugueseBrazilian}, {"pt-PT", Language::Portuguese},
        {"fr-CA", Language::FrenchCanadian}, {"fr-FR", Language::French},
        {"zh-Hans", Language::ChineseSimplified}, {"zh-CN", Language::ChineseSimplified},
        {"zh-Hant", Language::ChineseTraditional}, {"zh-TW", Language::ChineseTraditional},
        {"zh-HK", Language::ChineseTraditional}, {"zh-MO", Language::ChineseTraditional},
        {"zh-Hans-TW", Language::ChineseSimplified},
        {"zh-Hant-CN", Language::ChineseTraditional}, {"zh-SG", Language::ChineseSimplified},
        {"de-DE", Language::German}, {"it-IT", Language::Italian},
        {"ko-KR", Language::Korean}, {"nl-NL", Language::Dutch},
        {"ru-RU", Language::Russian}, {"pl-PL", Language::Polish}, {"th-TH", Language::Thai},
        {"PT-br", Language::PortugueseBrazilian}, {"ES-latn-mx", Language::SpanishLatin},
        {"en-US-u-rg-gbzzzz", Language::EnglishAmerican},
        {"fr-FR-x-ca", Language::French}, {"", Language::EnglishAmerican},
        {"xx-XX", Language::EnglishAmerican}, {"english", Language::EnglishAmerican}};
    for (const auto& [tag, expected] : cases) assert(ConsoleLanguage::FromTag(tag) == expected);
    for (unsigned i = 0; i < ConsoleLanguage::kNames.size(); ++i)
        assert(ConsoleLanguage::Parse(std::to_string(i)) == static_cast<Language>(i));
    for (const auto bad : {"", "-1", "20", "99999999999", "1junk", "1.5", "+1", "auto"})
        assert(!ConsoleLanguage::Parse(bad));
    std::istringstream global("language_index=4\nlanguage_index=invalid\n");
    std::istringstream title("# language_index=0\n language_index = 0 \r\nlanguage_index=99\n");
    const auto g = TitleSettings::ReadLanguage(global), t = TitleSettings::ReadLanguage(title);
    const auto selected = ConsoleLanguage::Resolve("es-MX", g, t);
    assert(selected.language == Language::Japanese && selected.source == "title");
    const auto inherited = ConsoleLanguage::Resolve("es-MX", g, {});
    assert(inherited.language == Language::Italian && inherited.source == "setting");
    const auto automatic = ConsoleLanguage::Resolve("es-MX", {}, {});
    assert(automatic.language == Language::SpanishLatin && automatic.source == "xbox");
    assert(ConsoleLanguage::Resolve("en-US", {}, {}).language == Language::EnglishAmerican);
    const auto file = TitleSettings::File(argv[1], "01007EF00011E000");
    assert(TitleSettings::SaveLanguage(file, Language::Japanese));
    { std::ofstream out(file, std::ios::app);
      out << "# keep\nresolution_setup=5\nother=a=b\nlanguage_index=7\n"; }
    assert(TitleSettings::SaveLanguage(file, Language::PortugueseBrazilian));
    { std::ifstream in(file); assert(TitleSettings::ReadLanguage(in) == Language::PortugueseBrazilian); }
    assert(TitleSettings::SaveLanguage(file, {}));
    { std::ifstream in(file);
      const std::string body{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
      assert(body == "# keep\nresolution_setup=5\nother=a=b\n"); }
    { std::ifstream in(file); assert(!TitleSettings::ReadLanguage(in)); }
    assert(!TitleSettings::SaveLanguage({}, Language::Japanese));
    assert(!TitleSettings::SaveLanguage(file, static_cast<Language>(20)));
    assert(!TitleSettings::SaveLanguage(file, static_cast<Language>(-1)));
    assert(!TitleSettings::SaveLanguage(argv[1], Language::Japanese));
}
"""


class ConsoleLanguageTests(unittest.TestCase):
    def test_mapping_precedence_and_persistence(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-console-language-") as directory:
            source = Path(directory) / "driver.cpp"
            binary = Path(directory) / "driver"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary), directory], check=True, timeout=10)

    def test_diagnostic_names_match_eden_enum(self):
        enums = (ROOT / "src/common/settings_enums.h").read_text()
        names = re.search(r"ENUM\(Language,\s*([^;]+)\);", enums).group(1).split(",")
        header = (ROOT / "src/eden_uwp/console_language.h").read_text()
        table = re.search(r"kNames\{([^}]+)\}", header).group(1)
        self.assertEqual([name.strip() for name in names], re.findall(r'"([A-Za-z]+)"', table))
