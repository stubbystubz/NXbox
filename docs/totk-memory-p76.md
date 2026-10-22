# TotK memory: p76

Changes are local and uncommitted on `p76/totk-memory`. No network, console,
commit, push or deployment was used. This is a host-tested candidate mitigation;
there is no post-change Xbox capture or MSVC/UWP build yet.

Searched `docs/` for memory notes; read `memory-budget.md`,
`guest-memory-budget.md`, `nxbox-botw-memory.md`, `DBSZ-jit-cache.md`,
`FASTMEM-crash.md`, `cpu-clean-page-table.md`, `d3d12-upload-cpu.md` and
`d3d12-methods-upload-performance.md`. Reviewed p60, `e8f0d58eb` (already in this branch), including its accounting,
async decode admission, shader gauges and pressure watermarks. Those policies
are preserved. The larger DBSZ JIT capacity and the existing small dispatch/link
representations are also preserved.

## Evidence: what the missing 1.8 GB contains

Baseline: build **1.0.8.845**, local capture
`~/.local/share/nxbox/artifacts/compat-2026-10-18/r-totk-mem/`.
`diag.txt:893` is the supplied final breakdown. Values below are MiB
(bytes / 1,048,576), rather than decimal MB.

| Field | Baseline MiB |
| --- | ---: |
| Process / granted limit | 5001.34 / 5120 |
| Guest committed | 1658.81 |
| GPU textures / RGBA8 subset | 1241.94 / 889.06 |
| GPU buffers / upload / readback | 138.69 / 96 / 16 |
| Texture scratch / shader IR | 48.34 / 34.81 |
| Unattributed | 1732.39 |

`summary.txt` records median 19.3 fps, a `bad allocation` on CPU0, and process
exit around +124 seconds. Four `JIT_WX mode=dualmap reason=default` records in
`eden.txt` accompany four 201,326,592-byte A64 caches. Before this change,
`TryAllocDualMap` creates pagefile sections without `SEC_RESERVE` and
`ConfigureWx` sets `committed_size = maxSize_`. Consequently **768 MiB of unique
JIT section commitment is missing from the old breakdown**, including the idle
fourth core's 192 MiB. RW and RX are aliases of the same section; counting both
would invent another 768 MiB. This establishes a substantial source allocation,
not an independently measured 768 MiB contribution to the UWP usage metric.

The last `GAME_STALL` reports code cursors/high-water marks of 174,224,567,
120,614,711 and 138,674,522 bytes on cores 0–2; core 3 reports no emission.
The old section capacities greatly exceed these cursors. Their sum is about
413.43 MiB. Full section commitment is therefore plainly avoidable for this run.

**The entire 1732.39 MiB cannot be assigned from this capture.** After the known
768 MiB code sections, roughly 964.39 MiB remains for JIT metadata, fiber/native
stacks, allocator retention, compiler allocations, resources outside the main
context, and opaque driver state. The new counters distinguish important pieces
on the next run; there is no evidence to call all of that remainder driver memory.

Source audit:

- JIT block descriptors, incoming-link tables and A64 fastmem-patch tables retain
  allocated hash-table capacity. Link-site vectors have separate allocations.
  They now use a counting allocator on UWP. Clears can retain buckets; the counter
  follows allocation/deallocation instead of multiplying live element counts.
  Range interval tree nodes and their nested descriptor sets are also counted.
  Constant maps and profiling/disk-cache bookkeeping remain outside the counter.
- Each `FiberImpl` owns two 512 KiB stacks on UWP: 1 MiB of allocated stack
  capacity per object, including thread-fiber wrappers. The new gauge counts
  these until destruction. It is not a measurement of touched stack pages or OS
  thread-stack commitment; native thread stacks remain residual.
- D3D12 buffers already use placed allocations in 16 MiB pages, charged once at
  heap granularity, including free slots and fence-retained pages. Textures use
  committed resources and existing allocation-info byte accounting. Switching
  all textures to placed heaps would require fragmentation/fence validation and
  is not implemented speculatively here.
- Upload/readback allocations and retained buffer/texture scratch already have
  p60 gauges. The early capture reaches 727,056,384 upload bytes, versus
  100,663,296 in the final sample: there is a large transient, already attributed.
  Completed unleased staging pages already trim under pressure; live leases are
  intentionally protected.
- Descriptor rings/caches now report nominal slot address span (capacity times
  descriptor increment). It is diagnostic only: D3D12 does not specify that this
  equals backing allocation bytes. Ancillary presenter/blit/recorder heaps and
  driver metadata remain residual. No arbitrary descriptor shrink was made.
- Command allocators are recycled through bounded slots and reset after their
  fence completes, including the recorder path. This is not an unbounded list of
  never-reset allocators. Their internal driver storage has no byte query here.
- The capture's native D3D12 renderer does not construct the Mesa GL renderer.
  Loaded libraries are not evidence of a gigabyte-scale live Mesa shader cache.
- `fastmem=false` is logged on all four cores. Canonical guest backing and sparse
  storage already have live commitment scans. No second full host DRAM copy was
  found; sparse guest virtual mappings are not physical copies of all guest RAM.

## Changes and expected savings

| Change | Expected effect for this capture |
| --- | --- |
| Demand-committed dual-map JIT sections | Reserve the original capacity with `SEC_RESERVE`, commit the RW prefix initially up to 16 MiB, and grow in page-aligned chunks of at least 16 MiB. Both aliases observe the same committed pages. Estimated final commitment is 176 + 128 + 144 + 16 = **464 MiB**, about **304 MiB saved** versus 768 MiB. Exact commitment depends on emission high-water marks and lookahead. |
| Explicit native ASTC → BC3 | `NXBOX_ASTC_TO_BC=1` selects the existing BC3 encoder and resource path. For eligible full blocks, 4 bytes/texel becomes 1 byte/texel. If all measured RGBA8 bytes were eligible ASTC, savings would be **666.80 MiB** and BC storage would add about 222.27 MiB. This is an upper bound: RGBA render targets/native images, block padding, heap alignment and BC fallbacks reduce it. |
| Additional attribution | Code, metadata, dispatch arrays and fiber capacities reduce the arithmetic residual. Attribution alone saves **0 MiB**. Descriptor payload is excluded from that subtraction. |

The two modeled reductions total about **970.8 MiB**, conditional on the ASTC
share and matching workload. They are not measured process savings. Compression
still needs a temporary RGBA decode before BC3 encoding, and can increase CPU
work, reload latency or transient compiler/decoder pressure. Section commitment
is retained at its high-water mark until destruction; reserved sections cannot
be decommitted as ordinary private code pages. This change does not shrink code
cache capacity or alter invalidation behavior.

If reserving/mapping/initially committing a dual-map section fails, its partial
handles/views are released and the existing private demand-committed W^X path is
used. A later growth failure follows the existing fatal JIT allocation path;
accounting changes only after successful commitment. `NXBOX_JIT_EAGER_COMMIT=1`
restores full commitment for a controlled baseline. It is latched and works even
with memory diagnostics disabled. Desktop JIT allocation behavior is unchanged.

The BC3 setting is applied after global/per-title settings and before renderer
creation. Shared `CalculateConvertedSizeBytes`, `ConvertImage`,
`TranscodedAstcSize` and D3D12 `Image` format selection consume the same setting,
including async uploads and sRGB selection. It does not relabel RGBA data as BC3.
Unset preserves configured `astc_recompression` (the existing equivalent, where
`Bc3` is value 2); exact `0` forces uncompressed. The new environment opt-in is
native-D3D12-only. `ASTC_STORAGE recompression=2 env_bc=true native=true` confirms
the environment selection. BC3 is lossy and remains explicitly opt-in. Existing
native BC texture/array sampling workarounds and allocation fallbacks remain;
visual correctness on this title still requires the console comparison.

### Eviction and the 5 GiB budget

The capture reports `texture_expected=335544320` and
`texture_critical=469762048` (320/448 MiB) even at pressure level 2. This is
consistent with p60: with 1241.94 MiB texture allocation alone, adding headroom
then subtracting the pressure reserve still exceeds the watermark caps. They
are soft GC thresholds, not an allocation ceiling. Aggressive GC permits costly
ASTC reloads, while retaining decoding images, recent images (10 ticks in
aggressive mode), dirty-image writeback and fence leases. The 2996 evictions prove
activity, not reclamation of the currently required working set. Compression
reduces the resources behind these protections without discarding guest contents.
No new priority/dirty-image exception or arbitrary hard texture cap is added.

## New MEMORY_BREAKDOWN fields

- `jit_code_committed`: successful UWP code backing commitment, once per section
  or private cache. Includes constant-pool/prelude space and unused committed tail.
- `jit_code_reserved`: single-view code capacity; informational, never added to
  commitment again.
- `jit_metadata`: requested allocator bytes for block/link maps, nested link-site
  vectors, range interval nodes/sets and A64 fastmem-patch maps. Includes allocated buckets and allocation
  padding requested by the container, excludes underlying malloc overhead.
- `jit_dispatch`: embedded A32/A64 dispatch array capacities for live emitters.
- `fiber_stacks`: both owned fiber stack arrays; capacity rather than residency.
- `descriptor_payload`: nominal address span of main descriptor allocator rings
  and CPU caches. Not included in `known` or subtracted from `unattributed`.

The first, third, fourth and fifth fields join p60's nonoverlapping `known` sum;
`unattributed` remains saturating process usage minus that sum. These are
approximate concurrent samples and mixed allocation/commit measures, not a
stop-the-world reconciliation of the OS account. `driver_bytes=unknown` remains
honest. Gauge destruction removes capacity; allocator deallocation removes table
storage; diagnostic opt-out leaves allocation policy operational.

## Host validation

New tests execute production dual-map allocation/cleanup, configuration and
commit-growth methods with Windows API doubles, including initial failures,
growth failure, existing committed prefixes, maximum capacity, W^X fallback,
release, and the eager baseline. ASan/UBSan drivers use `-Wall -Wextra -Werror
-Wshadow -Wconversion`. Other new cases exercise real Boost flat-map allocations,
nested vectors, moves, retained buckets after clear, failed allocations and final
release, plus overlapping production range insertion/invalidation/clear; production fiber declarations and descriptor initialization; explicit
native-only opt-in; and production ASTC size calculations for layers, mips and
partial BC blocks. Existing dual-map patch/link and p60 GC/admission tests are
also run. API doubles do not establish Xbox section-policy support or execute JIT
code through a Windows RX alias.

Validation: **52 distinct focused tests: 50 passed, 2 skipped**. The focused
suite ran 51 tests in 26.794 s, `OK (skipped=2)`. After adding the diagnostic
opt-out case and exercising the tracked allocator in the UWP link test, the
final targeted run passed all 10 tests in 10.146 s. Both skips require an
unavailable cached Xbyak checkout; they are the existing patch/alias-model cases.
The new production API-double tests ran successfully.

Reproduce the complete focused set from the repository root:

```sh
python3 -B - <<'PYTEST'
import sys, unittest
sys.path.insert(0, "tests/port")
names = [
    "test_totk_memory_p76", "test_totk_memory_budget", "test_jit_dualmap",
    "test_jit_hot_paths", "test_dbsz_jit_capacity", "test_d3d12_descriptor_ranges",
    "test_d3d12_descriptor_setup", "test_d3d12_descriptor_ring",
    "test_d3d12_async_astc", "test_d3d12_host_memory_budget",
    "test_d3d12_staging_pressure",
]
result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromNames(names))
sys.exit(not result.wasSuccessful())
PYTEST
```

Changed C++ lines were formatted with the installed clang-format; the new Python
file and changed existing Python regions were formatted with Ruff. Ruff check
passes for `test_totk_memory_p76.py` and `test_jit_hot_paths.py`.
`test_jit_dualmap.py` retains its pre-existing PLW1510 at the unchanged subprocess
call in the cached-Xbyak host model. `git diff --check` passes. No claim is made
that Clang warning checks replace the required MSVC build.

## Exact console follow-up (pending; not performed)

Use the same title/update/save and input sequence as
`compat-2026-10-18/r-totk-mem/inputs.txt`, including the menu A presses and movement
beginning at +120 seconds. Keep the 5 GiB grant and renderer/settings identical.
Capture diagnostics, Eden log, crash log, frame timing and screenshots for at least
10 minutes and through the previous +124-second exit. Do not reuse a running
process between variants: switches are latched. Keep
`NXBOX_MEMORY_BREAKDOWN=1`, `NXBOX_ASTC_MEMORY_BUDGET=1` and
`NXBOX_TEXTURE_MEMORY_PRESSURE=1` for all three runs.

| New local capture name | Additional `LocalState/nxbox_env.txt` entries | Purpose |
| --- | --- | --- |
| `r-totk-p76-eager-rgba` | `NXBOX_JIT_EAGER_COMMIT=1`, `NXBOX_ASTC_TO_BC=0` | Instrumented baseline |
| `r-totk-p76-demand-rgba` | `NXBOX_JIT_EAGER_COMMIT=0`, `NXBOX_ASTC_TO_BC=0` | Isolate section commitment |
| `r-totk-p76-demand-bc3` | `NXBOX_JIT_EAGER_COMMIT=0`, `NXBOX_ASTC_TO_BC=1` | Combined candidate |

Required proof at matching gameplay points:

1. Eager → demand: `jit_code_committed` must drop, plausibly about 304 MiB near
   the old last sample; `jit_code_reserved` and guest commitment should remain
   comparable. Check `JIT_WX` reason (`demand_commit` or an explicitly logged
   fallback), actual `process` reduction and unchanged JIT capacity-clear rates.
   If process usage does not follow commitment, report that metric mismatch.
2. RGBA → BC3: `texture_rgba8` must fall, `texture_bc` must rise by a smaller amount,
   and `gpu_textures` plus `process` must fall. Compare scene-matched peaks and
   steady state; compare `gpu_upload`, scratch and decode peaks too. The 666.80
   MiB bound must not be presented as achieved savings without these samples.
3. `unattributed` should shrink from attribution even in the instrumented eager
   baseline. Code savings need not reduce that residual further because code is
   now subtracted explicitly. Inspect `jit_metadata`, `fiber_stacks` and remaining
   residual trends to distinguish retained CPU containers from opaque driver
   growth. A residual near 1 GiB still needs investigation; it is not solved by
   renaming the JIT allocation.
4. Confirm sustained gameplay, correct terrain/UI/alpha/sRGB textures, no stale
   async uploads, no allocation errors/device loss, and no unbounded growth after
   repeated area changes. Record frame/decode/reload cost at approximately the
   original 19 fps workload. Repeat other ASTC-heavy titles with the opt-in before
   considering a default change.

There is no console run proving these changes yet. The exact baseline and the
required comparison runs above make that remaining acceptance step explicit.
