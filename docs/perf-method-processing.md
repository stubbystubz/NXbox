# Maxwell repeated-register processing

## Change

`NXBOX_MAXWELL_REPEAT_WRITES` defaults on. Set
`NXBOX_MAXWELL_REPEAT_WRITES=0` before engine construction to restore the previous
non-incrementing ordinary-register loop. On console, put that line in
`nxbox_env.txt` and restart the title; the existing runtime environment loader
accepts it without a launcher change. The existing
`NXBOX_GPU_FRONTEND_FASTPATH=0` also bypasses this optimization.

The DMA parser already batches consecutive ordinary registers, uses an execution
mask/run-length table, and preserves a concrete Maxwell type for dispatch. Sink
consumption already hoists shadow-mode selection and merges dirty flags locally.
The remaining repeated-register handler ran shadow and dirty processing for every
payload word, even though these registers have no intermediate observer.

The new handler commits the final register value and marks both dirty-table flags
once if *any* payload differs from the original value. It scans earlier words
only when the final value restores the original. Track/TrackWithFilter retain the
last shadow value; Replay consumes the existing shadow value once. Empty packets
keep their previous behavior. No allocation or new persistent cache is involved.

Program-base registers retain scalar processing because shader discovery observes
intermediate writes. Detailed `NXBOX_GPU_METHOD_PROFILE=1` recording also retains
the scalar path and its exact counters. Macro entry, active-macro validation, CB,
inline uploads/indexes, draw dispatch, and other side effects retain their existing
handlers. No DMA drain boundary or renderer code changes.

## Host replay and profiling

Run from the repository root:

```sh
TMPDIR=/private/tmp CXX=clang++ python3 tools/nxbox/benchmarks/maxwell_methods.py \
  --ab --frames 1000 --repeat 7
TMPDIR=/private/tmp CXX=clang++ python3 tools/nxbox/benchmarks/maxwell_methods.py \
  --ab --breakdown --frames 500 --repeat 1
```

The runner compiles extracted production method bodies with `clang++ -O3`.
Both switch settings use the same executable, alternate A/B order, and use wall
clock time. The synthetic stream has 3,000 draws and 60,000 methods per frame:
incrementing state, redundant state, four-word non-incrementing state, vertex
first/count, and traditional begin/end draws. Warmup is 50 frames. Profiling-off
runs have no per-draw trace hashing. Generated source/binaries are temporary and
removed automatically.

This is a reduced-register/null-rasterizer fixture, not a captured MK8D pushbuffer
or a full emulator build. `--breakdown` additionally installs the production
method collector over an exclusive Maxwell clock and reports exclusive DMA and
Maxwell time. Its per-scope clocks materially inflate execution time; use the
ordinary profiling-off replay for throughput comparisons.

Measurements on the shared Mac, 2026-10-21 (seven alternating runs, ns/draw),
after the full test suite finished and load fell to 2.82:

| Recording phase profiling | Opt-out median (range) | Enabled median (range) |
| --- | ---: | ---: |
| Off | 131.446 (130.621–131.965) | 120.528 (119.389–120.904) |
| On, no method collector | 260.308 (258.446–261.386) | 253.743 (252.859–255.098) |

The profiling-off median is **8.3% lower**, with non-overlapping ranges. Earlier
runs at load 36–46 had large overlapping ranges and were discarded as speedup
estimates. No console millisecond saving can be inferred from the host numbers.

One separately instrumented breakdown (ns/draw):

| Exclusive scope | Opt-out | Enabled |
| --- | ---: | ---: |
| DMA decode/dispatch outside Maxwell scopes | 150.251 | 150.835 |
| Maxwell scopes, including draw stub and profiling overhead | 475.148 | 471.698 |
| Shadow/dirty subdivision inside Maxwell | 101.704 | 97.610 |

Shadow/dirty was the largest named method subdivision in this synthetic replay;
individual begin/end method subdivisions were about 36 ns/draw each. These are
instrumented measurements, not additive estimates of the uninstrumented timings.

## Equivalence checks

`--ab` first compares a draw-by-draw digest of register state, shadow registers,
dirty flags, draw indexedness and instance count, plus terminal state, with both
switch settings. This verification is outside the timed throughput runs. The
3,000-draw replay produces trace `11207792181610875607` with both settings.

The sanitizer fixture additionally compares exact arrays and flags against
independent scalar `CallMethod` execution and against the switch-disabled bulk
path. Cases cover all four shadow modes, empty/1/2/4/16/256-word packets,
unchanged values, changing values, restored values, shared dirty-table entries,
and preexisting dirty flags. It samples ordinary registers across the register
space. Existing tests cover side-effect dispatch, macro boundaries, CB data,
DMA packet splits, and the independent legacy sink oracle.

```sh
cd tests/port
TMPDIR=/private/tmp CXX=clang++ python3 -m unittest -q
```

The full suite needs its existing host prerequisites: Python 3.10 or newer,
FontTools, and pkg-config visibility of fmt/OpenSSL. On this Mac, select the
Homebrew Python and include `/opt/homebrew/lib/pkgconfig` and the OpenSSL
pkg-config directory. FontTools was reused from the existing local tool cache;
no project dependency was added.

Validation: the complete suite finished with **1,011 tests, OK (45 skipped)**.
The A/B replay and exact scalar/optimized/opt-out comparisons passed under
ASan/UBSan. Formatting checks and `git diff --check` also passed.

## Still unverified

Console CPU time, MSVC code generation, real MK8D packet frequency, frame pacing,
and D3D12 calls/descriptors need console validation. The host verifies engine
state and null-rasterizer draws; it does not execute D3D12. Guest-memory reads in
`Step`, actual macro HLE/JIT execution costs, and renderer binding work are not
represented by this replay. Macro HLE/JIT code is outside this task's owned files
and was not modified. Achieving the approximately 14 ms GPU-thread target is
not established by this change.
