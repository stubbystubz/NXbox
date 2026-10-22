# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-level checks (plus a host model) for the dual-mapped JIT code cache (NXBOX_JIT_DUALMAP=1)."""

import glob
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
X64 = ROOT / "src/dynarmic/src/dynarmic/backend/x64"
BLOCK = (X64 / "block_of_code.cpp").read_text()
HEADER = (X64 / "block_of_code.h").read_text()
POOL = (X64 / "constant_pool.cpp").read_text()
UNWIND = (X64 / "exception_handler_windows.cpp").read_text()
PATCH_PATH = ROOT / ".patch/xbyak/0001-exec-delta-dualmap.patch"
PATCH = PATCH_PATH.read_text()


def body(source: str, signature: str) -> str:
    """Text of the brace block that follows the first occurrence of `signature`."""
    start = source.index(signature)
    depth, i = 0, source.index("{", start)
    begin = i
    while True:
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        i += 1
        if depth == 0:
            return source[begin:i]


def dual_branch(function_body: str) -> str:
    """The `if (dual_map)` statement of a function body (block or single line)."""
    start = function_body.index("if (dual_map)")
    rest = function_body[start:]
    if rest.startswith("if (dual_map) {"):
        return body(rest, "if (dual_map)")
    return rest.split("\n", 2)[1]


class FlagTests(unittest.TestCase):
    def test_flag_is_on_by_default_and_read_once(self):
        fn = body(BLOCK, "bool DualMapRequested()")
        self.assertIn("static const bool requested", fn)
        self.assertIn('std::getenv("NXBOX_JIT_DUALMAP")', fn)
        self.assertIn("value == nullptr || !(value[0] == '0' && value[1] == '\\0')", fn)
        self.assertEqual(BLOCK.count("std::getenv"), 1)

    def test_member_defaults_to_the_protect_path(self):
        self.assertIn("bool dual_map = ConfigureWx();", HEADER)
        fn = body(BLOCK, "bool BlockOfCode::ConfigureWx()")
        self.assertIn("bool mapped = false;", fn)
        self.assertIn("return mapped;", fn)

    def test_configure_runs_before_the_constant_pool(self):
        self.assertLess(HEADER.index("bool dual_map = ConfigureWx();"), HEADER.index("ConstantPool constant_pool;"))
        self.assertLess(HEADER.index("size_t committed_size = 0;"), HEADER.index("bool dual_map = ConfigureWx();"))

    def test_wx_mode_is_logged_with_a_reason(self):
        fn = body(BLOCK, "bool BlockOfCode::ConfigureWx()")
        self.assertIn('"NXBOX JIT_WX mode={} reason={}"', fn)
        self.assertIn('"dualmap" : "protect"', fn)


class FallbackTests(unittest.TestCase):
    def test_every_failure_has_a_reason_and_falls_back(self):
        fn = body(BLOCK, "uint8_t* TryAllocDualMap(size_t size)")
        for reason in (
            "flag_off",
            "xbyak_unpatched",
            "create_section_failed",
            "map_rw_failed",
            "commit_rw_failed",
            "map_rx_failed",
        ):
            self.assertIn(reason, fn)
        alloc = body(BLOCK, "uint8_t* alloc(size_t size) override")
        self.assertLess(
            alloc.index("TryAllocDualMap(size)"),
            alloc.index("DYNARMIC_VIRTUAL_ALLOC(nullptr, size, MEM_RESERVE"),
        )

    def test_views_are_released_on_partial_failure_and_on_free(self):
        fn = body(BLOCK, "uint8_t* TryAllocDualMap(size_t size)")
        self.assertEqual(fn.count("CloseHandle(section)"), 3)
        self.assertIn("UnmapViewOfFile(rw)", fn)
        free = body(BLOCK, "bool FreeDualMap(uint8_t* rw)")
        for call in ("UnmapViewOfFile(views.rx)", "UnmapViewOfFile(rw)", "CloseHandle(views.section)"):
            self.assertIn(call, free)

    def test_proven_probe_calls_are_the_ones_used(self):
        fn = body(BLOCK, "uint8_t* TryAllocDualMap(size_t size)")
        for call in (
            "CreateFileMappingFromApp",
            "PAGE_EXECUTE_READWRITE",
            "MapViewOfFileFromApp(section, FILE_MAP_WRITE",
            "MapViewOfFile3FromApp",
            "PAGE_EXECUTE_READ",
            "FILE_MAP_EXECUTE | FILE_MAP_READ",
        ):
            self.assertIn(call, fn)


class NoProtectTests(unittest.TestCase):
    def test_dual_branches_never_protect(self):
        for signature in (
            "void BlockOfCode::EnableWriting()",
            "void BlockOfCode::DisableWriting()",
            "void BlockOfCode::PrepareWriteRange(",
            "void BlockOfCode::ClearCache()",
        ):
            with self.subTest(function=signature):
                branch = dual_branch(body(BLOCK, signature))
                for forbidden in ("ProtectMemory", "VIRTUAL_PROTECT", "VirtualProtect", "VIRTUAL_ALLOC", "VirtualAlloc"):
                    self.assertNotIn(forbidden, branch)

    def test_disable_writing_flushes_the_rx_range(self):
        branch = dual_branch(body(BLOCK, "void BlockOfCode::DisableWriting()"))
        self.assertIn("FlushInstructionCache(GetCurrentProcess(), getCode() + range.begin", branch)
        self.assertIn("RecordWriteRange(append_begin, size_)", branch)
        self.assertIn("writing = false;", branch)

    def test_prepare_write_range_records_exact_executable_offsets(self):
        fn = body(BLOCK, "void BlockOfCode::PrepareWriteRange(")
        self.assertIn("static_cast<const u8*>(address) - getCode()", fn)
        self.assertNotIn("- top_", fn)
        self.assertIn("RecordWriteRange(offset, offset + size)", dual_branch(fn))
        record = body(BLOCK, "void BlockOfCode::RecordWriteRange(")
        self.assertIn("dual_map ? 0 : JitPageSize() - 1", record)

    def test_write_pointer_is_only_used_by_the_protect_path_and_delta_setup(self):
        for line in BLOCK.splitlines():
            if re.search(r"\btop_\b", line) and not line.lstrip().startswith("//"):
                self.assertTrue(
                    any(t in line for t in ("ProtectMemory", "VIRTUAL_ALLOC", "VirtualAlloc", "DualMapExecView", "setExecDelta")),
                    line,
                )


class AddressAuditTests(unittest.TestCase):
    def test_stores_into_emitted_code_use_the_writable_view(self):
        self.assertIn("code.Writable(&target_constant)", POOL)
        self.assertNotRegex(POOL, r"\n\s+target_constant = constant;")
        for name in ("unwind_info_w", "rfuncs_w"):
            self.assertIn(name, UNWIND)
        self.assertIn("memcpy(code.Writable(unwind_code)", UNWIND)
        self.assertIn("code.Writable(code.AllocateFromCodeSpace(sizeof(UNW_EXCEPTION_INFO)))", UNWIND)
        self.assertIn("memset(Writable(ret), 0, alloc_size)", BLOCK)

    def test_published_addresses_stay_executable(self):
        self.assertIn("reinterpret_cast<u8*>(unwind_info) - code.getCode()", UNWIND)
        self.assertIn("RtlAddFunctionTable(rfuncs, 1, reinterpret_cast<DWORD64>(code.getCode()))", UNWIND)
        self.assertIn("const u8* const end_ptr = getCode() + maxSize_;", BLOCK)
        self.assertNotIn("&top_[maxSize_]", BLOCK)

    def test_no_backend_file_stores_through_a_code_pointer(self):
        for path in X64.glob("*.cpp"):
            text = path.read_text()
            self.assertNotRegex(text, r"\*\s*code\.getCurr\s*(<[^>]*>)?\(\)\s*=[^=]", path.name)
            self.assertNotRegex(text, r"memcpy\(\s*code\.getCurr", path.name)

    def test_dual_map_member_is_uwp_only(self):
        start = HEADER.index("#ifdef DYNARMIC_UWP_APPCONTAINER\n    struct WriteRange")
        end = HEADER.index("#endif", start)
        self.assertIn("bool dual_map = ConfigureWx();", HEADER[start:end])
        self.assertIn("return false;", body(HEADER, "bool IsDualMapped() const"))


class XbyakPatchTests(unittest.TestCase):
    def test_patch_is_wired_into_the_dependency(self):
        cpm = (ROOT / "cpmfile.json").read_text()
        block = cpm[cpm.index('"xbyak": {') :]
        block = block[: block.index("}") + 1]
        self.assertIn('"0001-exec-delta-dualmap.patch"', block)

    def test_delta_is_applied_to_every_absolute_address_path(self):
        added = "\n".join(line[1:] for line in PATCH.splitlines() if line.startswith("+") and not line.startswith("+++"))
        self.assertIn("return top_ + execDelta_;", added)
        self.assertIn("&top_[size_] + execDelta_", added)
        self.assertIn("db(size_t(getCode()) + offset, jmpSize);", added)
        self.assertIn("#define XBYAK_NXBOX_EXEC_DELTA 1", added)
        # Stores keep using the write pointer: the patch does not touch db()/rewrite().
        self.assertNotIn("top_[size_++]", added)
        self.assertNotIn("uint8_t *const data", added)

    def test_every_absolute_address_site_reads_get_curr_or_get_code(self):
        # The patch relies on these sites; fail loudly if the pinned Xbyak is bumped.
        pristine = find_pristine_xbyak()
        if pristine is None:
            self.skipTest("no Xbyak source tree (set XBYAK_DIR to an unpatched xbyak-7.40.1 checkout)")
        text = (pristine / "xbyak/xbyak.h").read_text()
        self.assertIn("reinterpret_cast<const uint8_t*>(addr) - getCurr()", text)  # jmp/call(const void*)
        self.assertIn("disp -= (size_t)getCurr() + 4 + addr.immSize;", text)  # [rip + pointer]
        self.assertIn("disp = size_t(base_->getCurr());", text)  # absolute label
        self.assertIn("size_t remain = size_t(getCurr()) % x;", text)  # align
        self.assertIn("return mgr->getCode() + offset;", text)  # Label::getAddress


def find_pristine_xbyak():
    candidates = []
    if os.environ.get("XBYAK_DIR"):
        candidates.append(Path(os.environ["XBYAK_DIR"]))
    for pattern in (str(ROOT / ".cache/cpm/xbyak/*"), str(Path.home() / ".cache/CPM/xbyak/*")):
        candidates += [Path(p) for p in sorted(glob.glob(pattern))]
    for candidate in candidates:
        if (candidate / "xbyak/xbyak.h").is_file():
            return candidate
    return None


@unittest.skipUnless(shutil.which("clang++") and shutil.which("patch"), "needs clang++ and patch")
class HostModelTests(unittest.TestCase):
    def test_patch_applies_and_alias_model_passes(self):
        pristine = find_pristine_xbyak()
        if pristine is None:
            self.skipTest("no Xbyak source tree (set XBYAK_DIR to an unpatched xbyak-7.40.1 checkout)")
        with tempfile.TemporaryDirectory(prefix="nxbox-dualmap-") as directory:
            tree = Path(directory) / "xbyak-src"
            shutil.copytree(pristine, tree)
            if "XBYAK_NXBOX_EXEC_DELTA" not in (tree / "xbyak/xbyak.h").read_text():
                subprocess.run(["patch", "-p1", "-i", str(PATCH_PATH)], cwd=tree, check=True, capture_output=True)
            binary = Path(directory) / "xbyak-dualmap"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-DXBYAK64_GCC",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "-isystem",
                    str(tree),
                    str(ROOT / "tests/port/xbyak_dualmap.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            # x86-64 Linux executes the generated code through the RX alias; other hosts check bytes only.
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("xbyak dualmap: ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
