# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Audit opcode dispatch and validate real IR emission with optional DXC.

Dispatch coverage is distinct from implementation coverage. ``--report`` prints
both; ``--strict`` rejects remaining backend gaps (but not frontend operations
that Vulkan/GLSL also require lowering before emission).
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/shader_recompiler/backend/hlsl"
OPCODES = ROOT / "src/shader_recompiler/frontend/ir/opcodes.inc"
# These are explicitly unsupported, not counted as implemented or emulated.
BACKEND_GAPS = {
    "GetPatch",
    "SetPatch",
    "SR_WScaleFactorXY",
    "SR_WScaleFactorZ",
}


def without_literals(source):
    """Keep offsets stable while hiding braces inside comments/string literals."""
    return re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match.group()),
        source,
        flags=re.DOTALL,
    )


def emitters():
    definitions = {}
    for path in BACKEND.glob("emit_hlsl*.cpp"):
        source = path.read_text()
        masked = without_literals(source)
        for match in re.finditer(r"\bvoid\s+Emit(\w+)\s*\([^{}]*?\)\s*\{", masked):
            depth = 1
            end = match.end()
            while depth:
                depth += (masked[end] == "{") - (masked[end] == "}")
                end += 1
            name = match[1]
            if name in definitions:
                raise AssertionError(f"Duplicate emitter: {name}")
            body = masked[match.end() : end - 1].strip()
            definitions[name] = {
                "file": str(path.relative_to(ROOT)),
                "body": source[match.end() : end - 1],
                "rejected": body.startswith("throw NotImplementedException"),
            }
    return definitions


def coverage():
    opcodes = re.findall(r"^OPCODE\((\w+)", OPCODES.read_text(), flags=re.MULTILINE)
    definitions = emitters()
    missing = sorted(set(opcodes) - definitions.keys())
    rejected = sorted(name for name in opcodes if definitions.get(name, {}).get("rejected", False))
    lowered = [
        name
        for name in rejected
        if name == "Join" or name.startswith(("BoundImage", "BindlessImage"))
    ]
    gaps = sorted(set(rejected) - set(lowered))
    return {
        "total": len(opcodes),
        "dispatch_targets": len(opcodes) - len(missing),
        "implemented_emitters": len(opcodes) - len(missing) - len(rejected),
        "frontend_lowered": len(lowered),
        "backend_gaps": gaps,
        "missing_emitters": missing,
        "complete": not gaps and not missing,
    }


class HlslOpcodeCoverageTests(unittest.TestCase):
    def test_every_opcode_has_an_emitter(self):
        result = coverage()
        self.assertEqual(result["missing_emitters"], [])
        self.assertEqual(result["dispatch_targets"], result["total"])
        dispatch = (BACKEND / "emit_hlsl.cpp").read_text()
        self.assertIn('#include "shader_recompiler/frontend/ir/opcodes.inc"', dispatch)
        self.assertIn("Invoke<&Emit##name>", dispatch)

    def test_no_blanket_not_implemented_stubs(self):
        for name, definition in emitters().items():
            self.assertNotIn("NotImplemented();", definition["body"], name)
            if definition["rejected"]:
                self.assertIn(f"HLSL {name}", definition["body"], name)

    def test_backend_gaps_are_explicit_and_do_not_expand(self):
        result = coverage()
        self.assertEqual(set(result["backend_gaps"]), BACKEND_GAPS)
        documentation = (BACKEND / "README.md").read_text()
        for name in BACKEND_GAPS:
            self.assertIn(name, documentation)
        # A rejection is not success: the report must remain incomplete until fixed.
        self.assertFalse(result["complete"])

    def test_lowered_texture_opcodes_match_reference_contract(self):
        reference = ROOT / "src/shader_recompiler/backend/glsl"
        reference_sources = "\n".join(path.read_text() for path in reference.glob("*.cpp"))
        for name, definition in emitters().items():
            if name.startswith(("BoundImage", "BindlessImage")):
                self.assertTrue(definition["rejected"], name)
                self.assertIn(f"Emit{name}(", reference_sources)
                self.assertIn("TexturePass", definition["body"])

    def test_ir_samples_and_optional_dxc(self):
        binary = os.environ.get("NXBOX_HLSL_IR_TEST_BINARY")
        if not binary:
            self.skipTest("Set NXBOX_HLSL_IR_TEST_BINARY to the hlsl-ir-tests CMake executable")
        dxc = shutil.which(os.environ.get("DXC", "dxc"))
        profiles = {
            "vertex": "vs_6_0",
            "packed10": "vs_6_0",
            "fragment": "ps_6_0",
            "compute": "cs_6_0",
            "waves": "cs_6_0",
            "resources": "ps_6_0",
            "arithmetic": "cs_6_0",
            "msaa": "ps_6_0",
            "msaa_array": "ps_6_0",
            "msaa_depth": "ps_6_0",
            "msaa_array_depth": "ps_6_0",
            "numeric": "cs_6_0",
            "global": "cs_6_6",
            "geometry": "gs_6_0",
            "atomic64": "cs_6_6",
            "atomic64_fallback": "cs_6_0",
            "fragment_atomic64_native": "ps_6_6",
            "fragment_atomic64_fallback": "ps_6_0",
            "shared64": "cs_6_6",
            "helper": "ps_6_6",
        }
        with tempfile.TemporaryDirectory(prefix="nxbox-hlsl-coverage-") as directory:
            result = subprocess.run(
                [binary], cwd=directory, check=False, capture_output=True, text=True, timeout=120
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Numeric opcodes emitted: 189", result.stdout)
            for name, profile in profiles.items():
                source = Path(directory) / f"hlsl_{name}.hlsl"
                self.assertTrue(source.is_file(), name)
                if not dxc:
                    continue
                compiled = subprocess.run(
                    [
                        dxc,
                        "-WX",
                        "-T",
                        profile,
                        "-E",
                        "main",
                        str(source),
                        "-Fo",
                        str(source.with_suffix(".dxil")),
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=240,
                )
                self.assertEqual(
                    compiled.returncode, 0, name + ": " + compiled.stdout + compiled.stderr
                )
                self.assertGreater(source.with_suffix(".dxil").stat().st_size, 0, name)
            if not dxc:
                self.skipTest("IR emission passed; DXC unavailable (set DXC to validate DXIL)")
            # Native 64-bit instructions must not accidentally validate as SM 6.0.
            rejected = subprocess.run(
                [
                    dxc,
                    "-WX",
                    "-T",
                    "cs_6_0",
                    "-E",
                    "main",
                    str(Path(directory) / "hlsl_atomic64.hlsl"),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertNotEqual(rejected.returncode, 0, "Wide atomics accepted by SM 6.0")


if __name__ == "__main__":
    if "--report" in sys.argv or "--strict" in sys.argv:
        report = coverage()
        print(json.dumps(report, indent=2))
        sys.exit(1 if "--strict" in sys.argv and not report["complete"] else 0)
    unittest.main()
