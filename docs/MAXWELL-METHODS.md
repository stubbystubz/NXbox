# Maxwell method hot path

Baseline: `b00e5b842bb22599c991c59a145f45ec6d455dba`, branch
`p32/maxwell-methods`. Changes are deliberately uncommitted.

## What the console profiles establish

Re-running `profile_rank.py` on the supplied captures gives:

| Capture | Draws/frame | Maxwell methods, ms/frame | DMA pusher, ms/frame |
| --- | ---: | ---: | ---: |
| MK8D, 34 windows | 980 | 5.36 | 2.83 |
| Odyssey, 34 windows | 1,012 | 10.51 | 3.46 |

These are exclusive phase measurements, not the cost of one register write.
Dividing by draws includes all state writes, shadow/dirty processing, uploads,
queries and dispatch in that phase. The captures do not provide a method stream
or enough method counts to identify a single cause for the entire 5–10 ms.
The host replay below isolates frontend overhead; it does not establish an
Xbox frame-rate improvement or explain all the console time.

## Audit and changes

- `src/video_core/engines/maxwell_3d.cpp:488`: scalar `CallMethod` mixed an
  instrumentation boundary with register processing. The fallback loop in
  `CallMultiMethod` called it for every word, even though the bulk call already
  owned the same phase. `FrameProfile::Enter` already avoids redundant clocks
  for a nested identical phase, but still incurs the per-word TLS checks,
  scope bookkeeping and virtual `Sink::Enter`/`Leave` calls. The new private
  `CallMethodImpl` retains validation, shadow RAM, dirty writes and all method
  side effects. Bulk fallback invokes that implementation under one outer
  scope; scalar sends still establish their own boundary. Exceptions unwind
  the outer scope normally. Host assembly also confirms that Clang can inline
  the smaller scoped wrapper into `DmaPusher::CallMethod`, and the scope-free
  implementation tail-branches to `ProcessMethodCall`. Previously that dispatch
  required a call followed by scope cleanup. This compiler observation is
  specific to the host harness, not a claim about MSVC's Xbox code generation.
- `src/video_core/engines/maxwell_3d.cpp:360`: changed-register dirty marking
  tested both destination bits before setting them and branched on duplicate
  flags. Both bits are now set unconditionally; setting an already-set bit is
  idempotent. The unchanged-register early return still avoids all dirty-table
  loads. The two tables, shared flags and changed-then-restored writes remain
  observable exactly as in scalar execution. The sink already accumulates its
  own flags once per drain (`:302`); that optimization is retained.
- `src/video_core/engines/maxwell_3d.cpp:533`: the previous ordinary
  non-incrementing bulk optimization applied only the last value. A packet
  `original, changed, original` could leave dirty flags clear even though
  scalar processing sets them. Bulk ordinary writes now retain intermediate
  dirty and shadow transitions. This is a correctness repair relative to the
  previous shortcut, covered against independent scalar calls in every
  shadow mode. Specialized CB/upload/index bulk behavior is unchanged.
- `src/video_core/macro.h:243` and `src/video_core/macro.cpp:1388`: compiled
  programs and HLE hash matching were already cached, and dispatch already
  used `std::visit`. Repeated execution still probed `unordered_flat_map`.
  A one-entry cache now returns the compiled program directly for the same
  method. Misses still take the map/compile path. Any insertion or erasure
  invalidates the pointer first, because flat-map mutations can move entries.
  HLE selection, parameter refresh, interpreter/JIT fallback and variant
  dispatch remain intact. No cache-hit timing benefit is included in the
  register/draw replay numbers below.
- `src/video_core/dma_pusher.cpp:203`, `:214`, `:233`, `:286`, `:317`:
  non-incrementing packets already use `CallMultiMethod`, incrementing CB
  writes already use `CallCBDataIncrementing`, and incrementing ordinary state
  runs already use the four-argument `CallMultiMethod`. Maxwell dispatch
  already retains the final concrete type. The parser is retained, including
  chunk boundaries, IncreaseOnce, pending counts and DMA segment addresses.
  It benefits from the corrected bulk implementation without a new parser
  shortcut or reading future payload words across an engine side effect.

NXbox history was checked with `git log` for these paths. In particular,
`f8c40264e`, `65807a21b`, `72389f742`, `921211e31` and `da0afc104` had already
introduced ordinary-state runs, dispatch masking, frontend instrumentation
changes, and direct inline state sinks. This patch builds on those paths;
it does not reintroduce batching that was already present. Full ancestral
`git blame` is unavailable in this partial clone without fetching missing
promisor objects.

There is no per-register `getenv`, lock, atomic counter or `std::function`
lookup on the ordinary path. Environment values are latched. Detailed
`MethodScope` timing remains opt-in (`NXBOX_GPU_METHOD_PROFILE=1`). Disabled
submission tracing returns before its atomic counters; draw logging remains
in `DrawManager::ProcessDraw` (`draw_manager.cpp:277`). DMA synchronization
locks at `dma_pusher.cpp:83` and `:118` are command-list memory waits, outside
method parsing. They were not removed. Shader discovery retains its hooks
on program-region writes.

## Reproducible Mac replay

```sh
python3 tools/nxbox/benchmarks/maxwell_methods.py \
  --revision b00e5b842bb22599c991c59a145f45ec6d455dba --frames 5000 --repeat 7
python3 tools/nxbox/benchmarks/maxwell_methods.py --frames 5000 --repeat 7
python3 tools/nxbox/benchmarks/maxwell_methods.py --sanitize --frames 3 --repeat 1
```

The script extracts production method bodies from either the worktree or the
specified revision. Both builds use the same reduced register layouts and
hardware stubs from the existing port fixtures. The executed path includes
`DmaPusher::ProcessCommands`, both DMA dispatch helpers, Maxwell scalar/bulk
methods, actual shadow/dirty/sink processing, actual method dispatch, the
precomputed register-write-run initializer and actual `DrawManager` methods.
The null rasterizer counts calls and instances. Its draw entry is not inlined.
Unsupported hardware paths are stubbed; this is not a complete emulator build.
`--emit-source /tmp/replay.cpp` exposes the generated translation unit for review.

Each frame contains 20,000 payload methods, 1,000 draws, incrementing state
packets, unchanged state, repeated non-incrementing state, vertex first/count
and begin/end draw packets. There are 50 warm-up frames, with no timing of
construction or compilation. Every run verifies exactly 1,000 rasterizer draw
calls per measured frame and checks the pending method count and assertions.
The checksum includes the 50 warm-up frames. Timings divide the same elapsed
frontend time by payload methods and by draws respectively; ns/draw includes
state processing, not just the renderer callback.

`profile=1` uses the real scope classes and a small phase sink reproducing
same-phase suppression and clock reads at phase changes. It does not install
D3D12 or issue host GPU work. Detailed method profiling is forced off for both
measurements. No guest-memory traffic, shader compilation, texture/buffer-cache
work or macro/HLE workload is included. Offsets are synthetic; game captures
must still validate cache locality, macro mix and end-to-end console behavior.

Measurements: Mac arm64, Apple clang 21.0.0 (`clang-2100.1.1.101`), C++20 `-O3`,
assertions enabled, seven fresh-process samples per row, 5,000 measured frames
per sample. See the results below.

| Profiling | Before ns/method | After ns/method | Before ns/draw | After ns/draw | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| Off | 9.825 | 6.925 | 196.509 | 138.508 | 29.5% |
| Coarse phases | 16.803 | 13.540 | 336.050 | 270.797 | 19.4% |

Raw ns/draw samples (same order as the runs):

- before, profile=0: 195.276, 199.139, 195.207, 197.212, 196.509, 196.883, 194.515.
- before, profile=1: 342.816, 333.600, 332.749, 336.050, 371.502, 338.380, 334.514.
- after, profile=0: 132.347, 146.546, 138.508, 135.205, 136.242, 152.267, 147.713.
- after, profile=1: 264.534, 279.206, 270.797, 273.501, 283.030, 270.367, 270.584.

All 28 timed runs reported `methods/frame=20000`, `draws/frame=1000` and
`checksum=5050000`. The reduction is for the combined patch, not an attribution
of individual changes. Timings are host measurements and must not be scaled
linearly to a Series X/S CPU.


## Rollback and validation

`NXBOX_MAXWELL_METHODS=0` restores per-word scopes in the executable bulk
fallback and disables the last-macro lookup. It is latched when the first
engine is created; restart the application after changing it. On Xbox, add the
assignment to `LocalState\nxbox_env.txt`; the generic runtime environment loader
already imports arbitrary entries, so no launcher allowlist change is needed.
`NXBOX_GPU_FRONTEND_FASTPATH=0` also disables those two optimizations and retains the existing broader frontend rollback.
The dirty correctness repair and idempotent bit writes remain active: rollback
never restores the lost-dirty bug. `NXBOX_DMA_STATE_RUNS=0` remains the existing
rollback for precomputed DMA run lengths. `NXBOX_GPU_METHOD_PROFILE=1` enables
detailed diagnostic accounting and should stay off during throughput comparisons.

Validation includes randomized scalar/batch equivalence, all shadow modes,
shared dirty flags, changed-then-restored ordinary writes, repeated executable
side effects, macro parameter segments, CB boundaries, and 800 seeded DMA
streams split at arbitrary command-list boundaries. New checks assert one bulk
phase boundary, balanced unwinding after an exception, macro-cache hit/miss and
mutation invalidation, latched rollback values, and an integrated DMA → Maxwell
→ DrawManager → null-rasterizer replay under ASan/UBSan. Variant tests exercise
all HLE/interpreter/JIT alternatives with argument refresh and exceptions.

The full suite also exposed a pre-existing lifetime race in the asynchronous
fake manager in `tests/port/test_d3d12_fence_manager.py`. Its worker could call a
pure virtual function after derived destruction began. The fixture now stops
and joins the worker in its derived destructor, matching the existing native
D3D12 manager destructor. The wake-up test passed ten consecutive isolated
runs after the repair. No production fence behavior was changed.

Final validation on this worktree:

```text
python3 -m unittest discover -s tests/port
Ran 964 tests in 488.217s
FAILED (errors=2, skipped=44)
Differential cases: 651265
```

918 tests passed. The only errors are the two acknowledged environmental
`ModuleNotFoundError: No module named 'fontTools'` imports in
`test_button_layout` and `test_xbox_button_icons`; there are no assertion failures
or other runtime errors. The integrated replay passed ASan/UBSan both with the
new path enabled and with `NXBOX_MAXWELL_METHODS=0`. The frontend fixture also
passed independently with that rollback, and its normal runs exercise detailed
method profiling both disabled and enabled. `git diff --check` is clean.

