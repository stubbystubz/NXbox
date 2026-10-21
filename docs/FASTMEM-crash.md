# UWP fastmem boot faults

## Round 5: build 0.3.819 / fm6 — alias/backing lock coupling

### Capture comparison (complete files)

Round 4 is present. Neither fm6 capture contains an unhandled exception, abort,
or repeated PC/target pair. Both have one initialization `Present complete`,
not a game frame. `present_age_ms=0` is also the heartbeat's sentinel when
`last_present_tick_ms` has never been set; it is not evidence of ongoing frames.
Both watchers eventually report the process gone. These captures establish
failure to reach gameplay, but do not establish that the process remained alive
indefinitely or explain its eventual exit.

| Capture | AVs | Unique PCs / exact targets / 4 KiB pages | Last renderer evidence | Game frames |
| --- | ---: | --- | --- | --- |
| `r-mk8d-fm6` | 5 | 5 / 5 / 2 | 14.313532 s: syncpoints pending=0, signaled=1 | none |
| `r-wonder-fm6` | 5,993 | 5,993 / 2,694 / 196 | 15.259981 s: submission 2 | none |
| `r-mk8d-mp70` | 0 | 0 / 0 / 0 | 234.608641 s: submission 91,200 | 45/45 sampled windows, median game fps 25.7 |
| `r-wonder-mp70` | 0 | 0 / 0 / 0 | 205.694549 s: submission 120,120 | 39/39 sampled windows, median game fps 26.2 |

The first AV in each fm6 run is the intentional protected native startup probe.
All remaining recorded AV PCs are JIT addresses and all targets are inside the
alias arena; no recorded native backing fault or faulting instruction inside the
VEH follows them. The `handler_stack` frames are the observer's stack, not PCs
of nested faults. Successful faults handled by the regional VEH can precede the
observer in handler order, so their absence is not a complete fault census.

MK8D's `eden.txt:181–182` ends with submission 4 (upload/copy and query
report/predicate copy) and syncpoint completion. The same sequence exists in
`mp70:179–180` at 2.648791/2.650041 s and is followed by gameplay; it is not by
itself a failed GPU fence. The final HID producer logs at 14.308592 s, NVDRV at
14.242281 s. The four JIT write faults are **cpu0, tid=1896**, in 15 ms at ticks
137858031–137858046: page `0x1d9526f0000`, offsets 0, 8, 0x18, 0x28,
at four distinct PCs. CPU0's last record is `crash.txt:129` (handler frame 6).
The heartbeat is a different thread, **worker tid=7432**: 26 records, ending at
tick 137867859 (`:139`), 9.813 s after the final JIT AV. Memory settles near
1,589 MiB. Its progress does not prove CPU or GPU progress.

Wonder's first JIT fault is **cpu2 tid=6456**, tick 137654406. It corresponds
to the single backing-view error 8 at `eden.txt:185` (16.225070 s), followed by
successful private recovery and disabled *new* fastmem emission at `:186`.
The failing backing address is `0x250ed360000`, backing offset `0x8a360000`,
size 64 KiB; the alias target is `0x262b1360000`. CPU2 records 718 JIT AVs,
CPU0 tid=6080 records 4,134, CPU1 tid=6528 records 1,140. Their final records:

| Producer | Last crash.txt line | Tick | Meaning |
| --- | ---: | ---: | --- |
| cpu2 / 6456 | 55,195 | 137687187 | final handler frame after a JIT AV |
| cpu0 / 6080 | 60,032 | 137690109 | final handler frame after a JIT AV |
| cpu1 / 6528 | 60,042 | 137690125 | final handler frame after a JIT AV |
| heartbeat worker / 7784 | 60,046 | 137694281 | heartbeat after stall gap=3735 ms |
| lifecycle worker / 1912 | 60,045 | 137694281 | Suspending |

The final AV is 35.719 s after private recovery's first JIT fault. The hottest
exact target (`0x262b64f6110`) appears 25 times, always at different PCs;
maximum identical PC/target count is one. This is not an observed unchanged-PC
retry loop, including after private recovery. Eden does resume at 44.683308 s,
but its last 24 lines are the service-event inventory in `CreateEvent`, not GPU
submissions or a resumed render loop. Eden categories are not thread IDs;
only the crash records identify native tids. The worker role assigned to the
startup probe, heartbeat, and lifecycle callback does not mean GPU thread.

Microsoft's [MapViewOfFile3FromApp contract](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3fromapp)
requires an exact placeholder base/size match for replacement. The observed
64 KiB request is aligned and error 8 means insufficient memory/resources; the
logs do not establish an Xbox placeholder/view quota, a boundary mismatch, or
which resource was exhausted. No quota is invented or retried here. Existing
private recovery already stops new section attempts and preserves canonical
addresses and old aliases. The healthy mp70 runs establish the page-table
baseline, not an isolated measurement of every other build/config difference.

### Source defect addressed and regression

The regional allocator previously took its single recursive alias mutex before
classifying *every* read/write AV, even a foreign address. A backing fault and
`PrepareBackingPage` also waited for that mutex. Recursion only helps the same
thread: if an alias-operation owner waits for work needing a native backing
access on another thread, that access cannot finish and the owner cannot unlock.
Merely changing the VEH to `try_lock` leaves the native fallback waiting on the
same lock and does not break that dependency.

The allocator now classifies addresses before locking. Alias faults try the
metadata lock once and decline on contention, handing a known JIT access to
Dynarmic's existing SEH/callback/recompile path. Native backing faults and native
pointer preparation use a separate physical-backing mutex. Operations touching
both use alias -> backing order; clear/reclamation retain serialization with
materialization. Already-backed native preparation remains lock-free. A declined
contended alias need not have backing yet: the redirected native callback
prepares it before dereference. This is the explicit exception to the older
permission-fault preparation guarantee above. No inaccessible instruction is
reported resolved, and no alias is permanently disabled just for contention.

The new compiled host regression holds the production alias lock on one thread
until a native memory access on another completes. It executes production VEH,
SEH redirection, pointer preparation and private recovery, for fresh/reclaimed
regions, section/private backing and alias/native accesses (eight combinations).
It requires one alias redirection, accessible native backing, preserved writes,
normal alias materialization after contention, and no further section-map calls
or successful alias retries through 32 private-backing accesses. The existing
concurrent-fault harness now accepts a declined alias and performs native
preparation, matching the production dispatch contract.

Two negative controls independently restore (1) blocking alias acquisition and
(2) shared-lock native preparation. Each fails at the owner's two-second
watchdog; the fixed production paths complete. The test injects the owner's
wait dependency; it does not run Windows VM dispatch or prove that this exact
owner/wait cycle occurred on the console. The captures lack blocked-thread
stacks, lock ownership and regional dispositions, so **the console hang's root
cause remains unproven**. In particular this change does not claim all possible
VEH recursion, logging/heap-lock interactions, or physical allocation failure
are repaired. A matching console rerun is still required for first-frame
acceptance.

Validation on 2026-10-21: all 20 fastmem tests passed (including a focused run
with the final contention assertions). `python3 -m unittest discover -s tests/port`
ran 993 tests in 513.836 seconds: 947 passed, 44 skipped, and only the two known
`fontTools` import errors (`test_button_layout`, `test_xbox_button_icons`). Both
lock-order negative controls failed at the intended watchdog. Ruff check/format,
modified-line project clang-format and `git diff --check` passed. No commits,
Windows application build, or console rerun were made.

## Round 4: build 0.3.817 / fm5

### Complete capture analysis

Round 3 (`58469dd11`) is present in 0.3.817. Odyssey now reaches frames at
23.8 game fps (capture summary); MK8D has no new measurement. Wonder fastmem
still presents no frame. Its last Eden timestamp is 32.615 s. The crash logger
uses a different clock: its first/last JIT AV ticks span 16.141 s, so dividing
2,682 by 32 s is not a measured fault rate. There is one earlier intentional
native probe AV, 33.375 s before the first JIT AV.

`tools/nxbox/analyze_fastmem_faults.py` reads every line, counts PCs and target
4 KiB pages, and emits JSON (including the complete PC/page histograms):

```sh
python3 tools/nxbox/analyze_fastmem_faults.py \
  /Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/r-wonder-fm5 \
  /Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/r-ody-fm5 \
  /Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/r-wonder-nf5 \
  /Users/miguel/.local/share/nxbox/artifacts/compat-2026-10-18/r-wonder-vis
```

| Capture | Lines read | Recorded AVs | Unique PCs | Unique targets | Target pages |
| --- | ---: | ---: | ---: | ---: | ---: |
| `r-wonder-fm5` | 26,930 | 2,682 | 2,682 | 1,292 | 108 |
| `r-ody-fm5` | 119,714 | 11,811 | 11,811 | 4,091 | 214 |
| `r-wonder-nf5` | 2,397 | 0 | 0 | 0 | 0 |
| `r-wonder-vis` | 1,736 | 0 | 0 | 0 | 0 |

The fastmem counts each include one protected native startup probe. All targets
are inside the respective 512 GiB arena. Wonder has 1,219 reads and 1,463 writes;
roles are worker=1 (probe), cpu0=2,072, cpu1=571, cpu2=38. Its hottest pages are
`0x1edbf640000` (570 faults), `0x1fe4f729000` (252), `0x1ee1e692000`
(190), `0x1ee8ee00000` (140), and `0x1fe52620000` (106). Its hottest exact
target, `0x1edbf640a30`, occurs 17 times at distinct PCs. No PC or PC/target pair
repeats in either fastmem capture. This is not a recorded same-instruction retry
loop; it is consistent with Dynarmic invalidating and re-emitting guest blocks.
It does not prove that every AV reached that fallback: successful lazy alias
materialization can also resume an instruction.

Wonder's backing is `0x1dc3a000000`, its arena is `0x1dd3a000000`, and the
actual ready log says **`backing_size=0x100000000`**, four GiB, not `0x100000`.
The example targets are arena offsets `0x1085629388` and `0x213cb58521`.
Being in that reservation does not establish whether the guest page is unmapped,
rasterizer-protected, or backed by a regional alias record. The captures do not
include those records or the handlers' dispositions. The `+0x3043d7`,
`+0x306ec6`, `+0x3087d7` frames are the logger's handler stack, not the interrupted
JIT's caller stack. All four JIT unwind tables report successful registration.
No runtime allocator fallback warning or unhandled/abort record is present.
The earlier fm4 `bad allocation` failure is not reproduced in this capture.

Both no-fastmem Wonder captures reach frames: nf5 reports median 27.2 game fps,
vis 27.9. Odyssey's many AVs coexist with sustained rendering. Neither the AV
count nor the final first-chance record proves Wonder's terminal exception.

### SEH fallback ABI defect and fix

The regional VEH **must** return `EXCEPTION_CONTINUE_SEARCH` for a hole or an
unresolvable alias. That hands the data AV to Dynarmic's registered x64 SEH
handler; it does not itself terminate the process. A known fastmem instruction
uses `FastmemCallback` to find its native fallback/resume addresses, mark the
guest location against future fastmem emission, and invalidate its block. SEH
pushes the resume address onto the saved JIT stack and redirects the saved PC to
the native callback. It returns `ExceptionContinueExecution` (the SEH disposition,
not the VEH constant). No guest page is manufactured to satisfy a hole, and an
unresolved access must never be retried unchanged.

The generated SEH wrapper violated the Windows x64 calling convention: before
calling its C++ lambda it reserved only an eight-byte alignment slot. Its callee
owns **32 bytes of home space** in addition to stack alignment. An allowed spill
of the second register argument overwrites the wrapper's return PC; later home
slots overwrite its caller's stack. This defect is on the precise path used
when the VEH declines a guest access. The portable allocator tests previously
extracted and called only the lambda, bypassing the generated wrapper entirely.

The wrapper now reserves/restores **40 bytes** (32 home + 8 alignment). Its
argument forwarding, disposition return, saved guest context redirection, and
allocator behavior are otherwise unchanged. This repairs a concrete fallback
stack-corruption defect shared by the Windows handler. The console logs do not
prove that a home-slot spill caused Wonder's terminal exit; a matching console
retest is still required before claiming that game fixed.

Regression coverage:

- A compiled emitter model runs the production wrapper operations, including
  argument forwarding and the production lambda, with Windows x64 stack rules.
  The callee writes all four home slots, as the ABI permits. Both handled and
  declined AVs preserve the wrapper return PC and stack; handled AVs redirect
  the saved JIT PC/SP correctly. Restoring only the old eight-byte reservation
  makes this fail at the return-PC sentinel (negative control).
- A compiled integration test sends read/write faults on an untracked arena page
  through the production VEH, SEH lambda, `FastmemCallback`, and `ShouldFastmem`.
  VEH declines, SEH redirects once to the recorded callback/resume pair, the
  guest block is invalidated, and re-emission rejects fastmem for that marker.
  The hole remains inaccessible and the healthy session stays enabled. This
  covers the same callback/recompile policy used by desktop Eden; it does not
  execute Windows exception dispatch or simulate the guest memory callback.

Validation on 2026-10-21: all 19 fastmem tests passed. The full command
`python3 -m unittest discover -s tests/port` ran 991 tests in 539.916 seconds:
945 passed, 44 skipped, and only the two known `fontTools` import errors
(`test_button_layout`, `test_xbox_button_icons`). Ruff check/format, modified-line
clang-format, `git diff --check`, and the old-frame negative control passed.
No commits, Windows application build, or console rerun were made.


## Round 3: build 0.3.813 / fm4

### The arena-base AV is the intentional startup probe

The three `crash.txt:6` worker writes have the same ownership:

| Capture | Backing base | Write target = alias arena base | Target − backing base |
| --- | --- | --- | --- |
| `r-mk8d-fm4` | `0x29440000000` | `0x29540000000` | `0x100000000` |
| `r-ody-fm4` | `0x234dc000000` | `0x235dc000000` | `0x100000000` |
| `r-wonder-fm4` | `0x10f6a000000` | `0x1106a000000` | `0x100000000` |

Backing size is 4 GiB (`0x100000000`), with a separately reserved 512 GiB
alias arena immediately following it in these captures. The address is both
one-past-backing and arena offset zero. It is **not a backing overrun, guard-page
write, DMA/memset span, or last-section boundary failure**. During
`ProbeFaultHandling`, offset zero is a tracked 4 KiB read-only alias of backing
offset `0x1000`. The probe deliberately writes `0xA5` through it after reclaiming
the region. VEH prepares backing, declines the protected alias, and the leaf
probe's SEH guard catches the AV. Dynarmic SEH is not involved in this native
probe. `role=worker` is the crash logger's default role, not a GPU-thread identity.

Static inspection of the matching package establishes this attribution:
`async-real/pkg-39529147740/NXbox_game_0.3.813.0_x64.appx`, emulator revision
`c2a8ee53424524248e22501b6b1aa9066bd20a1a`. Extracted `nxbox.exe` SHA-256 is
`6e24fb4aff3e6815632b42a4e70956e4a730e9d1edaac8cd74d96b1a83b7cc0c`.
LLVM disassembly at RVA `0x431408` is `88 01`, `mov byte ptr [rcx], al`, in
the read/write leaf `ProbeMemoryAccess`. Its caller at `0x430480` loads the alias
base and write value `0xA5`; the recorded return address `0x430485` checks that
the probe returned false. REA tools were unavailable in this session; this is
instruction/source matching without PDB function symbols, not a runtime trace.

All three runs subsequently report `fault_probe=passed` and a resolved ordinary
backing write (`eden.txt:6`, `:5`, `:6`, respectively). MK8D's backing offset is
`0x1a7e88`, Odyssey's `0x11a3048`, Wonder's `0x1272c0`; all report reserved
state `0x2000`, protection zero, and `resolved=true`. No unresolved terminal
backing AV is recorded. The processes continue for 25.437 / 26.797 / 31.078
seconds after the arena-base probe AV.

### The fatal worker path materializes memory while only comparing addresses

The actual recorded terminations are `active_exception what=bad allocation`
followed by `SIGABRT` (`crash.txt:95–96`, `:96–97`, `:100–101`). All share
the same stack: `nxbox.exe+0x50a5a5`, `+0x9f8092`, `+0x6da185`, and onward.
The immediate preceding allocator evidence is:

| Capture / eden.txt line | First failed backing view offset | Time | Result |
| --- | --- | --- | --- |
| `r-mk8d-fm4:164–165` | `0xa8790000` | 25.724 s | map error 8; private recovery disables emission |
| `r-ody-fm4:144–145` | `0x3e5f0000` | 26.857 s | map error 8; private recovery disables emission |
| `r-wonder-fm4:166–167` | `0xbbd50000` | 31.285 s | map error 8; private recovery disables emission |

These are in-range, 64 KiB-aligned backing sections. Microsoft defines error 8
as [ERROR_NOT_ENOUGH_MEMORY](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-).
The logs do not identify the exhausted kernel resource or establish a particular
section/view quota. Successful private recovery is followed by many more failed
section-view attempts; the end of each `eden.txt` is truncated. The exact final
allocation failure is therefore not present in that file.

At RVA `0x9f808d` the binary calls the `GetPointerSilent` wrapper, then iterates
backward in page-sized steps and writes the continuity count. This matches
`DeviceMemoryManager::TrackContinuityImpl`, a device-address-space metadata
walk, not a GPU memory copy. `GetPointerSilent` tail-jumps to `GetPointerImpl`
at `0x50a360`. The canonical-pointer branch calls backing preparation at
`0x50a41a`; a false result branches to the `std::bad_alloc` throw at `0x50a586`
(return address `0x50a5a5`). This explains the shared fatal stack and descending
failed backing addresses. The caller returning to `0x6da185` matches nvdrv's
`Container::OpenSession` heap preallocation: a 32 MiB threshold at `0x6da0a0`,
device-address reservation, `HeapMapper` construction, then the locked continuity
walk. It scans the application's largest normal heap range before GPU rendering.
This is the nvdrv session/service worker operation, not a GPU/DMA buffer write;
the captures do not supply a native thread name beyond `worker`. The separate
forward `DeviceMemoryManager::Map` walk uses the same pointer query, so both
metadata paths acquired the unwanted materialization side effect from round 2.

### Fix and regression coverage

- Added `GetPointerForAddressTranslation`, which shares canonical/debug/rasterizer
  address resolution but never prepares physical backing. Both device mapping
  walks use it. Its pointer is only for address arithmetic; dereferencing callers
  retain `GetPointer`/`GetPointerSilent` and preparation before native access.
- After the first successful private recovery from a runtime section failure,
  untouched/reclaimed regions use private backing directly. Existing sections,
  aliases, contents, and canonical addresses remain intact. This prevents repeated
  failed section allocations and warning floods after fallback. New fastmem
  emission remains disabled; unavailable aliases decline to Dynarmic's existing
  callback/recompile path. Startup probes still require real section support.
- Startup logs now identify the expected protected-alias write before issuing it,
  and the ready log includes the exact backing size.
- A compiled regression runs the production reverse continuity walk and production
  pointer resolution over 2 MiB with both allocation paths unavailable. It requires
  zero backing allocations, unchanged fastmem state, and correct continuity counts.
  It also checks debug/rasterizer/unmapped translation and verifies that actual
  native pointer requests still prepare backing and explicitly report failure.
- Boundary tests write across two 64 KiB boundaries, into a short final region,
  and through the last byte of backing, using production VEH dispatch. They cover
  section and private backing, out-of-range/hole rejection, retention of an existing
  alias's bytes, reclamation, and no further failed section calls after recovery.
  Restoring eager preparation only for the translation query makes the production
  continuity regression fail with `std::bad_alloc` (negative control).

Validation on 2026-10-20: all 17 fastmem tests passed.
`python3 -m unittest discover -s tests/port` ran 984 tests in 515.911 seconds:
938 passed, 44 skipped, and only the two known `fontTools` import errors
(`test_button_layout`, `test_xbox_button_icons`). Ruff check/format,
`git diff --check`, and the eager-preparation negative control passed. Hashes
confirmed that the five implementation/test files were unchanged during the
final suite run. Modified C++ lines were formatted with the project's clang-format
configuration. No commits, Windows application build, or console rerun were made.
Console first-frame acceptance remains unmeasured. If both section and private
backing allocation fail during a real native access, page-table translation cannot
manufacture those bytes: preparation still reports failure before dereference.
This change removes allocation from metadata queries and preserves the existing
guarded JIT fallback; it does not claim recovery from physical memory exhaustion.

## Build 0.3.809: what the console captures establish

Both fm3 runs pass the p29 live fault probe before creating the CPU JITs.
The final recorded accesses are **JIT writes into the alias arena followed by
native writes into linear physical backing**, on CPU0 in both games:

| Capture / crash.txt line | PC | Write target | Ownership / offset |
| --- | --- | --- | --- |
| `r-mk8d-fm3:100` | `0x1fdcf33733a` (JIT) | `0x19ea1b90000` | arena + `0x2124b90000` |
| `r-mk8d-fm3:110–111` | `nxbox.exe+0x509314` | `0x17d1e990000` | backing + `0xa1990000` |
| `r-ody-fm3:316` | `0x2ab7340b620` (JIT) | `0x24c86fe1e28` | arena + `0x2165fe1e28` |
| `r-ody-fm3:326–327` | `nxbox.exe+0x509314` | `0x22a829e1e28` | backing + `0x619e1e28` |

The raw final first-chance records are:

```text
r-mk8d-fm3/crash.txt:110 role=cpu0 event=first_chance code=0xc0000005 pc=0x7ff6bf2b9314 access=1 target=0x17d1e990000
r-ody-fm3/crash.txt:326 role=cpu0 event=first_chance code=0xc0000005 pc=0x7ff706ea9314 access=1 target=0x22a829e1e28
```

Ownership uses `eden.txt:1`: MK8D backing `0x17c7d000000`, arena
`0x17d7d000000`; Odyssey backing `0x22a21000000`, arena `0x22b21000000`.
Both native destinations are inside the 4 GiB backing reservation, rather than
outside it or inside the alias arena. The records do **not** contain VM state or
protection: whether those bytes were committed section views, reserved
placeholders, or guarded/protected pages cannot be determined from these logs.
An address in the arena likewise does not prove that an alias view was mapped.

The matching executable was extracted from
`async-real/pkg-39516376269/NXbox_game_0.3.809.0_x64.appx`.
Its metadata identifies emulator revision `b00e5b842bb22599c991c59a145f45ec6d455dba`;
its `host_memory.cpp` includes the p29 ordering fix. LLVM disassembly at RVA
`0x509314` gives `48 89 0b`, `mov qword ptr [rbx], rcx`: an 8-byte native store,
consistent with the inlined `Memory::Write<u64>` callback's `memcpy`. Neither
native destination crosses a page boundary. Executable SHA-256:
`6019304a661f277d450b3980d021e74c55fc977b94529873b10560535795bcbb`.
REA's configured analysis providers were unavailable; this instruction check
used LLVM directly and did not recover matching function symbols.

The earlier `r-mk8d-fm2/crash.txt:81,91–92` has the same JIT-write/native-write
shape, with the native instruction at the older RVA `0x508f54`. fm3 therefore
rules out "the startup fault probe never ran" as an explanation. Its intentional
protected probe write is itself visible at `crash.txt:6` in both games; that
first-chance exception is expected and the application continues afterwards.

**The terminal exception remains unproven.** Neither fm3 capture contains an
`event=unhandled` record or the regional handler's disposition. MK8D continues
heartbeats and records `Suspending` at lines 122–125 after the last fault.
Odyssey records 21 JIT fastmem faults and continues execution between earlier
faults. Both `watcher.log` files confirm that `nxbox.exe` disappeared before the
capture ended; the process exit is established, its terminal exception is not.
First-chance records alone cannot establish which access killed either
process. No console rerun, second-chance dump, or frame acceptance was obtained
in this worktree. In particular, these captures do not prove an RO lazy alias,
a placeholder replacement failure, or a pre-installation thread fault. The
backing VEH does not filter host PCs or thread roles.

## Reproduced callback gap and changes

The p29 guarantee covered a **tracked** alias whose protection rejected the
write. It did not cover an alias with no regional mapping record: `Fault()`
returns false before materializing backing in that case. A native callback can
still resolve a valid canonical page-table pointer and dereference untouched
physical backing. This reproduces a JIT-to-native first-chance sequence, but a
working backing VEH should satisfy the second fault; it is not proof of fm3's
terminal cause.

- `Memory::Impl::PrepareNativePointer` now calls
  `HostMemory::PrepareBackingPage` before returning canonical, debug, or
  rasterizer callback pointers. Preparation follows rasterizer invalidation.
  It explicitly creates the regional backing outside exception dispatch, even
  after reclamation, without requiring a prior alias fault.
- If creation of a fresh section or its backing view fails at runtime, the
  allocator tries `VirtualAlloc2FromApp` with `MEM_REPLACE_PLACEHOLDER` to create
  private RW backing at the same canonical address. No old section or data is
  replaced. Private regions cannot be aliased, disable fastmem emission, and
  return to placeholders on reclamation. Startup probes still reject failed
  section APIs. This follows Microsoft's documented
  [private placeholder replacement](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2fromapp)
  and [preserving free](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree)
  contracts; Xbox runtime support still needs console validation.
- A live section handle or `mapping.mapped` flag no longer suffices to claim an
  existing-view fault. The handler queries VM state/protection. Committed linear
  backing with incorrect protection can be restored to RW without discarding
  data. A backing view lost back to a placeholder can be remapped from its
  retained section handle, preserving its contents; unexpected alias state is
  declined for the native callback.
- Runtime regional API failures and unexpected mapped-view states latch
  fastmem emission off for the lifetime of this allocator/session. A32 and A64
  normal and exclusive emitters consult the same atomic flag. One transition
  line includes reason, PC, target, access, bases, and error:
  `NXBOX MEMORY fallback=page_table scope=new_emission fastmem=disabled ...`.
  Healthy guest protection/hole faults do not disable fastmem.
- This is gradual fallback: subsequent emissions use the page table (exclusive
  operations use callbacks); previously compiled instructions retain valid
  aliases and faulted instructions retain Dynarmic's normal callback/recompile
  handling. There is no unsafe cross-core cache teardown or backing relocation
  inside VEH. The existing backing and its contents stay alive.
- The first post-probe backing fault and unresolved backing faults now log PC,
  target, read/write access kind, offset, pre-resolution VM state/protection,
  and `resolved`. This supplies disposition evidence absent from fm3.

If both section and private backing allocation fail, or an existing region's
contents cannot be preserved, switching address translation does not restore
those bytes.
Preparation reports failure and the callback throws `std::bad_alloc` rather
than returning an unusable pointer or silently dropping a guest write. Existing
`noexcept` callback boundaries make that condition unrecoverable. This patch
does not claim crash-free recovery from physical backing exhaustion/corruption.

## Current regression coverage and validation

Reviewer follow-up: native pointer preparation now checks the cached buffer's
inline arena pointer before calling the allocator, so default page-table mode
makes no preparation call. Regional backing publishes an atomic `backed` flag
after materialization and clears it before reclamation; already-backed native
pointers return without locking. Pointers outside the physical backing range
pass through unchanged without disabling fastmem or throwing. Regression cases
cover the default call count, concurrent lock-free preparation of section and
private backing, re-preparation after reclamation, and foreign pointer boundaries.

Reviewer follow-up validation: all 15 fastmem tests passed.
`python3 -m unittest discover -s tests/port` ran 976 tests in 586.437 seconds:
930 passed, 44 skipped, and only the two known `fontTools` import errors
(`test_button_layout`, `test_xbox_button_icons`). Ruff check/format and
`git diff --check` passed. No commits were created.

The production allocator, VEH, callback pointer-preparation helper, A32/A64
session gates, and `ShouldFastmem` are compiled into the port mocks. Added cases
cover an untracked alias followed by native access to untouched/reclaimed
backing, unexpected backing/alias protection, a lost backing view with retained
section data, failed native preparation, and
session-wide emission gating, and in-place private recovery after both section
creation and backing-view mapping failures. Private recovery also covers partial
clear, whole-region reclamation, cleanup, and failure of both allocation paths.
The VM mock distinguishes original mapping
permissions from current protection and supports queries inside views.

Removing only native pointer preparation makes the untracked-alias test fail
at `assert(Mock::Accessible(backing, true))`; with the fix, the native write
needs no second fault or mapping call. All 12 fastmem tests passed. Both actual
A32/A64 config headers also passed a C++20 syntax/copyability check.

Validation on 2026-10-20: `python3 -m unittest discover -s tests/port` ran 973 tests
in 492.286 seconds: 927 passed, 44 skipped, and only the two known environmental
`fontTools` import errors (`test_button_layout`, `test_xbox_button_icons`).
Hashes confirmed that all 12 modified source/test files stayed unchanged during
this final run. Ruff check/format and `git diff --check` passed. No Windows
application build or console execution was performed, and no commits were created.

## Previous round (p29, retained for context)

### Finding

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

### Evidence

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

### Changes

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

### Regression coverage and validation

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
