# UWP fastmem boot faults

## Finding

There is a reproducible gap in the lazy alias-to-native fallback: a write to a
read-only (or inaccessible) alias was rejected **before** its linear backing was
materialized. Dynarmic then redirected the instruction to a native callback,
which dereferenced that still-unmapped backing. This creates the observed shape
of a JIT write AV followed by a host write AV. The old startup probe did not test
exception delivery at all; a successful API probe and a non-null VEH registration
token were sufficient to advertise fastmem as ready.

This identifies and fixes a source-level fault-chain defect. It is **not proof of
the terminal cause of all three Xbox runs**: a functioning backing VEH should
also resolve the second AV. The supplied logs omit backing/arena base addresses,
handler dispositions, and matching symbols for the executable offsets. No Xbox
retest or matching-build symbolication was performed here. In particular, it
would be incorrect to claim that the logs prove Dynarmic ran before the VEH.

## Evidence

- `r-mk8d-fm2/crash.txt:81` records a JIT write AV at `0x2a08033733a`, target
  `0x2c2ca150000`; line 91 records a host write AV at
  `nxbox.exe+0x508f54`, target `0x2a147350000`. These are first-chance records,
  which alone do not establish that either exception was unhandled.
- `r-wonder-fm2/crash.txt:83` starts simultaneous CPU write faults; lines 108,
  110, and 119 record subsequent host writes at `nxbox.exe+0x508f54` or
  `nxbox.exe+0x508894`. Multiple cores encounter the same class of sequence.
- `r-ody-fm2/crash.txt:294` records a later worker-thread write AV in
  `VCRUNTIME140_APP.dll+0x10e33`; line 324 explicitly records it as unhandled.
  Its caller is `nxbox.exe+0x507f4d`. This is also a native-memory-access failure,
  but it cannot be attributed to Dynarmic's SEH from this stack.
- At commit `084983cf5`, `src/common/host_memory.cpp:864` tested alias access
  permissions and returned false before `EnsureRegion` at line 868.
  A fresh or reclaimed RO alias therefore reached the native fallback without
  creating its backing section. `src/core/memory.cpp:823` obtains the native
  pointer and line 834 writes through it with `memcpy`.
- The original readiness decision at
  `src/common/host_memory.cpp:572` only called `InstallHandler()` after
  `InitArena()`. `InitArena()` explicitly mapped every byte it touched; it did
  not issue a demand fault. The original mocks likewise called `Fault(backing)`
  explicitly before checking that fallback memory was usable, masking this gap.

Paths beginning with `r-` refer to
`/Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/`.

Microsoft documents that vectored handlers run before frame-based exception
handling: [Vectored Exception Handling](https://learn.microsoft.com/en-us/windows/win32/debug/vectored-exception-handling).
The handler is therefore allowed to decline a protected alias and let Dynarmic
redirect it; reordering SEH registration would not repair missing native backing.
The section creation already uses `PAGE_READWRITE | SEC_COMMIT`; this is not a
missing `MEM_COMMIT` on a successfully mapped section.

## Changes

- `src/common/host_memory.cpp:942` prepares the physical region before declining
  a protected or previously disabled alias. Alias mapping failures still disable
  that alias and let Dynarmic use its native callback. Reclamation does not undo
  the guarantee on the next alias fault. Alias permissions are never widened to
  make a protected write succeed.
- `src/common/host_memory.cpp:780` adds a live fault probe **after** installing
  the VEH: native backing write, lazy alias read, protected alias write followed
  by native fallback, then a host read after reclaim. The protected write must
  be declined. All probe sections and views are released on success.
- `src/common/host_memory.cpp:197` contains the probe's leaf SEH guard. An AV
  that the VEH cannot resolve rejects fastmem instead of escaping the probe.
  `src/common/host_memory.cpp:596` also catches allocator failures, removes the
  regional handler, releases the arena, and installs the existing private
  demand-commit page-table backing. Compilers without Windows SEH reject this
  optional fastmem path. `NXBOX_FASTMEM=1` remains required.
- `src/dynarmic/src/dynarmic/backend/x64/exception_handler_windows.cpp:192`
  preserves the exception record across argument setup. Its callback now only
  accepts data AVs in the search phase, declines malformed/execute/unwind and
  other exception records, and checks the fallback target before modifying the
  saved stack/PC. The generated wrapper returns that disposition rather than
  unconditionally returning `ExceptionContinueExecution`.
- The ready log now includes `fault_probe=passed`, `backing_base`, and
  `arena_base`, so another console capture can establish target ownership.

## Regression coverage and validation

`tests/port/test_uwp_fastmem.py` compiles the production allocator, VEH, and the
production Dynarmic SEH callback against `tests/port/uwp_fastmem.cpp` mocks. The
access model checks whether a view is actually accessible before dispatching an
AV, runs VEH before the SEH fallback, and rejects any request to retry an
unresolved access. It covers:

- JIT write AV on untouched RO backing, SEH context redirection, then a native
  write that requires **no second fault or mapping call**;
- the same sequence after reclamation and after an alias mapping failure;
- successful explicit section APIs but failed mapping inside the VEH, and
  successful registration with no VEH delivery: both select page-table mode;
- failure injection across startup APIs, cleanup, concurrent faults, mirrors,
  partial protections, and unsupported exports;
- SEH rejection of non-data exceptions without modifying PC/SP.

A negative control restoring only the old permission-before-backing ordering
made the direct sequence fail at `assert(Mock::Accessible(backing, true))`.
With the completed live probe, that same control selects page-table mode after
detecting a second backing fault during native fallback. The new ordering passes
the probe and the direct sequence.
The native probe helper itself uses Windows SEH; the macOS mocks model its
dispatch and catch behavior rather than executing Windows exception machinery.

Validation on 2026-10-20: all seven fastmem tests pass. The full command
`python3 -m unittest discover -s tests/port` ran 953 tests in 497 seconds:
907 passed, 44 skipped, and only the two known environmental `fontTools` import
errors (`test_button_layout`, `test_xbox_button_icons`). The leaf probe also
passed a Clang syntax check targeting `x86_64-pc-windows-msvc`; Ruff and
`git diff --check` passed. No Windows application build or console execution was
performed, and no commits were created.

Console acceptance remains a separate measurement: run MK8D, Odyssey, and Wonder
with `NXBOX_FASTMEM=1`, retain the new address/probe log, and verify first-frame
presentation and continued execution. This patch does not claim recovery from
arbitrary native memory corruption or exhausted physical memory; a JIT callback
is only a valid recovery target for a known fastmem instruction.
