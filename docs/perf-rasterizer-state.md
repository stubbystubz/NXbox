# Per-draw rasterizer bookkeeping

The p56 starting tree already caches framebuffer lookup by render-target serial,
attachment formats/extents by a pinned framebuffer snapshot, dynamic graphics
state by input generations, and idle query slots. Those are existing behavior,
not gains from this change. Descriptor setup is owned by p53 and remains unchanged;
its audit and measurements are in [perf-descriptor-setup.md](perf-descriptor-setup.md).

The supplied console costs (query state 0.63, attachment setup 0.56, resource
bindings 0.56, draw setup 0.54 and geometry setup 0.42 ms/frame) cover much more
than the bookkeeping measured here. This patch does not establish a console gain.

## Changes and correctness boundaries

`NXBOX_D3D12_RASTERIZER_STATE_FASTPATH` is **on by default**. Set it to `0` before
launching the emulator to restore the per-draw operations below.

- Graphics uniform-size arrays contain either all zeroes for an absent stage or
  all 65536 for a present stage. They are zero-initialized and written only in
  this draw setup. Compare the first element and fill only on that transition.
  Shader masks still update every draw. `SetUniformBuffersState` still executes:
  a channel switch must install these values into the newly selected cache state.
  Guest ranges still get clamped by the buffer cache; this does not cache guest
  buffer addresses, sizes, contents, or validity.
- Keep the five immutable shader-info pointers already obtained in the first
  stage loop and reuse them in the three following stage loops outside descriptor
  setup. Their lifetime is only the current draw. Pipeline/channel changes on
  the next draw obtain fresh pointers; there is no cross-draw pointer cache.
- Call the native query runtime's `EnableCounter` directly for the draw's z-pass
  counter. The shared cache's `CounterEnable` dispatches to `CounterStreamer`,
  whose start/pause methods only forward that same type and boolean. The runtime
  still checks that no draw query is open and validates the counter index.
  Reports, resets, closes, pipeline statistics, and stream output are unchanged.
- `PauseHostConditionalRendering` returns without recording a null binding when
  there is no logical predicate. Only this runtime installs non-null native
  predication. Other helpers only clear it, and a fresh command list starts with
  none. Ending a predicate clears native state **before** clearing the logical
  flag. With a logical predicate present, pause and resume always execute: a
  command-list reset or an external helper clear cannot invalidate a cached
  assumption that the native predicate remains bound.

The last item intentionally changes the raw API stream by removing redundant
`SetPredication(nullptr, 0, EQUAL_ZERO)` calls. Equivalence is the resulting state,
not an identical count of null calls. No descriptor bytes or binding addresses
change. A predicate-free draw now emits no predication packet from the runtime.

Both `UpdateRenderTargets` calls remain. The shared texture cache already uses
Maxwell dirty flags for attachment discovery, but must still prepare/mark images
and synchronize aliases. The second call after `FillImageViews` repairs the target
modification ordering used by bloom mip chains. Register equality alone cannot
prove this work unnecessary. Buffer validation, resource transitions, written
image publication and geometry setup are also retained; no safe generation proof
for skipping them was added in the owned files.

## Host replay

```sh
cd tests/port
scratch=$(mktemp -d /tmp/p56-state.XXXXXX)
TMPDIR="$scratch" python3 -m unittest -q test_d3d12_rasterizer_state
# Remove scratch after testing (xcrun may leave its cache database there).
# Repeatable timing, without the full suite compiling concurrently:
python3 test_d3d12_rasterizer_state.py --bench
```

The fixture extracts and compiles the current production metadata loop,
`CountStageTables`, switch parser, counter-enable draw branch, shared query-cache
counter dispatch, native streamer's start/pause methods, and runtime enable/
pause/resume/end methods. It uses the production `RecordStream` and `EncodeCall`
transport for predication packets. It does not compile the complete Windows
rasterizer. Native resources/device, Maxwell registers, shader descriptors and
command-list state are fixtures. The parallel command-state shadow layer and
D3D12 driver are absent.

The synthetic MK8D-like workload has 3,580 draws/frame, 73 recurring materials,
eight-draw runs, two or three active stages, 3 CBVs and 3–6 SRVs per active stage,
sparse UAVs, and changing z-pass enable. This is not a captured MK8D draw stream.
The three stage consumers are represented by loops using the production pointer
selection expression; actual texture/buffer binding work is excluded.

ASan/UBSan correctness runs rebuild an independent expected mask/size array and
compare every element on every draw with the switch unset, `1`, and `0`. They also exercise
active/inactive predicates, operation changes, new command-list state, external
predication clears, predicate ending, and rejection of counter enable inside an
open draw. The optimized fixture compares checksums between modes and replays
all encoded packets, checking the resulting predication state. This proves the
exercised host state, not rendered correctness on the console.

Timing uses clang++ C++20 `-O3`, ten warm-up frames, 120 measured frames, median
producer ns/draw, with replay outside the timed interval. The switch is set in a
fresh process for each mode. `--bench` repeats five times per mode, alternating
execution order, and reports the median of those per-process medians plus their
range. Temporary source/binaries are automatically removed. Switch `0` is the
reference path in the current source, not a separately built historical emulator.

| Combined host bookkeeping replay | Switch `0` | Switch `1` |
| --- | ---: | ---: |
| Median ns/draw | 50.31 | 34.63 |
| Sample range, ns/draw | 49.85–51.68 | 33.51–35.80 |
| State checksum | 11795261920 | 11795261920 |

This is 15.68 ns/draw (31.2%) in this narrow host fixture, including avoided null
packet encoding. These five samples ran after the full suite stopped compiling.
They are local Apple clang 21 arm64 macOS timings, not a prediction of Xbox
ms/frame. No gain is attributed to attachment or geometry setup.

## Port validation

On this Mac, a shell started in `tests/port` selects Xcode Python 3.9 and the
devkitPro `pkg-config`. The existing suite needs Python 3.10+ (`itertools.pairwise`),
host fmt/OpenSSL, and its existing `fontTools` dependency. Use the installed
Homebrew tools and cached fontTools, without adding project dependencies:

```sh
cd tests/port
PATH="/opt/homebrew/bin:$PATH" \
PYTHONPATH=/Users/miguel/.cache/uv/archive-v0/JNxAtyjfuxFcSQC1 \
TMPDIR="$scratch" python3 -m unittest -q
```

The complete suite passed: **1,016 tests, 45 skipped**, in 770.169 seconds, using
Homebrew Python 3.14.8, host pkg-config, and the cached fontTools package. The
initial run selected Xcode Python 3.9/devkitPro pkg-config and failed ten existing
environment-dependent tests; no renderer test failed in that run.

The validation runner creates `scratch` with Python `TemporaryDirectory` and
removes it after the suite, including xcrun's temporary cache database. C++ edits
were formatted with clang-format on the changed ranges, Python with ruff;
`git diff --check` is clean.

## Console A/B

Compare the same warmed MK8D scene with the switch unset versus
`NXBOX_D3D12_RASTERIZER_STATE_FASTPATH=0`, fully restarting between runs. Use the
existing 179-frame window and compare query/resource-binding wall time and total
emulator GPU-thread CPU time, keeping descriptor switches unchanged. Check query
results/conditional rendering, HUD, transitions, and scenes using stream output.
Raw null-predication counts should fall; that alone is not a performance result.

Windows/UWP compilation, native D3D12 validation, rendered output, query results
on actual hardware and console wall-time improvement remain unverified. No
emulator build, commit, push or console deployment was performed by this work.
