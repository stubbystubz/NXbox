# p91 — D3D12 draw-cost and memory-pressure comparison

Worktree: `~/.local/share/nxbox/w-p91`, branch `p91/jadyx-draw-cost`, based on
`rc/next`. Changes remain uncommitted. No push, network, gh, build/package,
PC or console operation was performed. No measured Xbox speedup is claimed.

Ideas studied from Jadyx's public source, JulianDr14/jadyx, GPL-3.0

Read PERF-COMMON, COMMON and `docs/perf-gpu-thread-p74.md` first. Studied local
reference history through `a46b3e7`, including `git show` for every requested
commit. Implementations below use NXbox's existing resource ownership, completion
snapshot, staging ring and UWP memory telemetry. No reference code, names,
comments or tests were imported. No shader, root layout or pipeline hash changed.

## Coverage before this patch

Paths in this table are relative to `src/`; anchors refer to the resulting tree.
“Have” describes the intended operation, not identical implementation or tuning.

| Reference / purpose | Before | NXbox evidence and decision |
| --- | --- | --- |
| `8052aad`: track pipeline-key register dependencies | Have | `video_core/renderer_d3d12/d3d12_state_tracker.cpp:28` registers native graphics dependencies; `:108` consumes them into a generation. `d3d12_state_cache.h:13` separately compares shader hashes, formats, topology, indexed state and tessellation enables. Our key builder is native HLSL/D3D12, not their Vulkan-derived key; do not import its register exclusion table. |
| `b0506a5`: reuse previous resolved pipeline | Have | `video_core/renderer_d3d12/d3d12_pipeline_cache.cpp:1470` checks exact tracked inputs and a successfully installed pipeline; `:1474` returns it without copying/hashing the key. Channel binding invalidates at `:2600`; compute/error paths clear the pointer. Existing verification and opt-outs remain. No speculative previous-pipeline mode added. |
| `6bc2de6`: dependency/reuse tests | Have | `tests/port/test_d3d12_state_tracker.py`, `d3d12_state_cache_test.cpp`, `test_d3d12_pipeline_generation_lookup.py` exercise dependencies, channel/lifecycle invalidation, pending PSOs, failed lookups, verification and opt-outs. Existing native-key tests apply rather than importing tests for a different key. |
| `2c9143a`: native calls on recording thread by default | Have | `video_core/renderer_d3d12/d3d12_context.cpp:1122` enables deferred recording unless explicitly disabled. Existing recorder has owned argument/descriptor snapshots and ordered replay. Keep our measured default. |
| `37f0f4c`: persist drawn pipelines every 600 frames | Have, shorter persistence window | `video_core/renderer_d3d12/d3d12_disk_pipeline_cache.cpp:206` records foreground first use, including prewarmed hits; `:455` journals on a worker, coalescing bursts for 100 ms. It survives `_Exit`, without waiting 600 frames or writing on the GPU thread. |
| `52b1e4b`: remember across 16 sessions | Have, different retention | `video_core/renderer_d3d12/d3d12_disk_pipeline_cache.cpp:216` merges current order with the prior order; `:430` reads/merges journal sessions. Previously used recipes remain ranked across short launches instead of aging out after a fixed count. Keep this existing policy. |
| `e73d9a2`: identical descriptor tables within a list | Have | `video_core/renderer_d3d12/d3d12_descriptor.cpp:376` checks previous, handle-key and full-key caches; `:427` finishes a table with reuse before allocation. Exact order/layout/count and submission tick participate. |
| `ef12d95`: return unused descriptor-ring reservation | Have equivalent allocation avoidance | `video_core/renderer_d3d12/d3d12_descriptor.cpp:441` returns previous tables; actual slot allocation is at `:461`, after all reuse checks. Reservations are capacity checks, not consumption of a table that must be rewound. Keep our design. |
| `f077ca7`: invalidate when offline slots are recycled | Have | `video_core/renderer_d3d12/d3d12_descriptor.cpp:297` advances source generation on frame reset, `:584` on source-heap rollover; `:398` requires current tick and generation for handle reuse. Full-key tables retain resources and immutable GPU copies independently. |
| `4e3fbec`: table reuse counters | Have | `video_core/renderer_d3d12/d3d12_descriptor.cpp:442` counts `DescriptorTableReuses`; profiler emits `descriptor_table_reuses` in `d3d12_context.cpp`. |
| `51f524a`: root CBVs instead of per-draw CBV descriptors | Have | `video_core/renderer_d3d12/d3d12_root_signature.h:28` accounts for our 64-DWORD budget; `d3d12_rasterizer.cpp:1761` binds root addresses, `:2200` reuses unchanged addresses. Our layout permits up to 22 CBVs. Do not introduce their 16-slot layout or another root signature. |
| `19c6e19`: stream short buffers and zero-pad readable tail | Partial | `video_core/renderer_d3d12/d3d12_buffer_cache.cpp:504` validates the synchronized range and `:540` copies an observable alignment tail; `:553` version-caches copies. `:449` handles streamed uniform padding. `d3d12_rasterizer.cpp:1762` uses a 64-KiB zero binding for missing slots. These copies own the alignment tail, not necessarily the entire shader reach past a short binding. HLSL declares a 64-KiB cbuffer (`hlsl_emit_context.cpp:197`); root CBVs have no declared view extent. Their full reach-padding fix is not present. No root binding change is made here: adding padding would address rendering correctness, not supply a demonstrated CPU reduction. |
| `4261a18`: submit every 512 draws | Have, tuned differently | `video_core/renderer_d3d12/d3d12_flush_policy.h:41` defaults to 128, with existing measured Odyssey/Wonder/MK8D improvements. Query-safe cadence tests already exist. Do not replace measured native tuning with 512. |
| `a3ef553`: small texture-heap growth under pressure | Missing texture-heap pool; different allocation model | `video_core/renderer_d3d12/d3d12_texture_cache.cpp:205` uses committed textures through `Context::CreateResource`; `d3d12_context.cpp:2290` requests the actual allocation, without speculative texture-heap padding. There is no pressure-driven exact-size **heap** proliferation to fix. Keep exact committed textures; our placed **buffer** pages are already 16 MiB (`:2136`). A placed texture allocator is not introduced by this patch. |
| `928205c`: wait for memory only near exhaustion | Partial | Existing `CreateBuffer` tries free placed slots first, but committed allocation and buffer growth still waited solely at a soft target. Implemented actual process-headroom gating at `d3d12_context.cpp:2105`, `:2297`, `:2476`; native allocation failure still retires and retries. |
| `df633d2`: retain recently emptied texture heaps | No corresponding texture heaps | Committed textures have no shared empty-heap state. Existing texture cache owns image lifetime and GC; buffer heap retirement still requires all resource leases to end (`d3d12_context.cpp:2151`). Do not add texture heap bookkeeping without a placed allocator. The relevant transfer-page churn is addressed below. |
| `f297f85`: retain recently used transfer buffers during ordinary pressure | Partial | Ring reuse already required fence completion and no CPU lease, but `d3d12_texture_cache.cpp:1735` and `d3d12_buffer_cache.cpp:113` requested immediate trimming at the critical cache watermark. Implemented a 16-submission recent-use window at `d3d12_context.cpp:2512` when process headroom permits. |
| `2130496`: transfer reuse/miss/headroom diagnostics | Partial | Existing `staging_requests`, `staging_fast_requests`, `staging_new_resources`, `staging_collections`, `forced_submits_memory_pressure`, `forced_submits_allocation_retry` and `forced_submits_staging_pressure` are emitted at `d3d12_context.cpp:713` and `:756`. No separate busy/no-size histogram or new process-memory log was added; existing counters suffice for the A/B below. |
| `c0e68e0`: keep constant-offset locals outside PS shared pool | Have equivalent private storage; no PS pool | `shader_recompiler/backend/hlsl/emit_hlsl.cpp:294` declares per-invocation `lmem`; `emit_hlsl_context_get_set.cpp:131` accesses it. Constant-offset spills never enter a shared UAV pool, so cannot contend for its slots. |
| `fe218ef`: adaptive PS local pool up to 128 MiB | Missing mechanism, intentionally not ported | Native HLSL has no shared pool to resize, slot allocator, overflow slot or pool-readback stall. Introducing one would change shader semantics and add contention/memory. Keep private storage; this does not prove the host driver is free of private-array bugs. |
| `6662024`: GPU pool regression | Not applicable | Their test exercises SPIR-V/Mesa, bitmap acquisition/release, helper lanes and pool replacement. Those mechanisms do not execute in NXbox's native HLSL renderer; importing its test would test another architecture. Xbox visual checks below remain necessary. |
| `3c3fe2a`: document pool behavior | Not applicable | The native renderer has no such pool policy; this report records the architectural reason for skipping it. |

This is not a claim to have added every reference mechanism. In particular,
placed texture pooling, adaptive shared PS local memory, full shader-reach
padding for short root-CBV bindings and additional transfer histograms remain
absent. The first two require architectures not present here; the latter two
are separate correctness/diagnostic gaps, not demonstrated CPU wins.
The draw CPU caches and persisted first-use set were already present; no new
pipeline/descriptor cache or invalidation rule was needed.

## Changes and correctness boundaries

1. **Avoid cache-target fence drains when the process has room.** At an existing
   soft-target miss, compare the requested bytes against live UWP process commit
   headroom, preserving 256 MiB. A buffer heap miss tests the complete 16-MiB heap,
   not just the buffer's requested range. An unknown/zero limit, failed telemetry,
   or non-UWP host retains the old wait policy. Check subtraction with saturation;
   no addition can wrap. GC targets still apply and completed empty buffer pages
   still trim. The device allocation is authoritative: `E_OUTOFMEMORY` still
   unconditionally retires, trims and retries. Do not skip resource transitions,
   guest readback waits or GPU retention. This only removes an optional memory
   reclamation wait, not a dependency wait.
2. **Keep hot transfer pages through critical-cache sweeps.** If process usage is
   below the reserve boundary, only pages not used for at least 16 submission
   ticks are trim candidates. GPU completion **and** sole ring ownership are still
   mandatory. Each request refreshes `page.tick`; retaining a page does not reset
   an occupied cursor or permit early reuse. Actual exhaustion and telemetry
   failure use immediate eligible-page trimming. Existing 64-MiB ring capacities,
   oversized dedicated leases and hard ring-capacity recovery are unchanged.
   Reuse still preserves the original upload/readback/default heap domains.

Both are default-on UWP memory-management optimizations, with one shared escape
hatch: `NXBOX_D3D12_ALLOCATION_HEADROOM=0` restores both old pressure behaviors;
unset or `1` enables them. The option latches on first pressure decision;
telemetry is read anew at each decision. No allocation, lock, counter or clock
was added to the ordinary per-draw path. Memory telemetry runs only where the old
code was already about to wait/trim under pressure.

No new rendering experiment was enabled. Existing previous-pipeline reuse is
exact-input reuse, not a guess; existing root CBVs predate p91. Existing
`NXBOX_D3D12_PIPELINE_LOOKUP_FASTPATH=0`,
`NXBOX_D3D12_PIPELINE_KEY_FASTPATH=0`, `NXBOX_D3D12_DEFERRED_RECORDING=0`,
`NXBOX_D3D12_PSO_FIRST_USE_ORDER=0` and `NXBOX_D3D12_FLUSH_DRAWS=0` are unchanged.

Persisted pipeline order already lives beside the shader cache under LocalState:
`<ShaderDir>/d3d12_cache/<title-id>/first-use-v<schema>.txt`, selected at
`d3d12_pipeline_cache.cpp:2307` and `d3d12_disk_pipeline_cache.cpp:424`.
No second persisted set or per-frame GPU-thread disk write was introduced.

## Expected gain and priority

The per-draw reference ideas would rank first for MK8D/Wonder/Odyssey/TotK, but
NXbox already implements them. Highest remaining actionable overlap was memory
retirement, followed by transfer-page churn. These patches offer **0 ms/frame
established saving** and an expected **0 ms/frame direct benefit in windows with
no soft-target allocations or critical-cache trims**. They do not establish the
requested sub-16-ms GPU-thread budget.

For a pressured window the useful model is:

`wall_ms_saved/frame = avoided_optional_wait_ms/frame + avoided_page_creation_ms/frame - telemetry_cost_ms/frame`

Inspected the exact p74 artifact lines directly (local read only):

| Capture | Frames | Memory-pressure submits | Allocation-retry submits | Staging-pressure submits | Staging requests | New staging resources |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `r-mk8d-rc90/diag.txt:4731` | 149 | 0 | 0 | 0 | 45991 | 90 |
| `r-wonder-all-on/diag.txt:1916` | 150 | 0 | 0 | 0 | 37458 | 107 |

For these windows specifically, the predicted direct optional-wait saving is
**0.000 ms/image**: no such submits occurred. New staging resources are only
0.604/0.713 per image; the logs do not identify which are avoidable critical-trim
recreations rather than needed size-class growth. Their creation time is not
isolated. The pressure-path changes therefore cannot honestly be presented as
a multi-ms improvement in those steady scenes. Assigning a positive numeric
saving to their staging component would fabricate its cost/eligibility. The p74
MK8D/Wonder `fence_wait` totals are 4.159/3.865 ms/image wall; the zero pressure
submit counts give no basis for attributing that time to these optional waits.
That total must not be claimed as a saving or as actual CPU time.
Page creation can also affect upload/texture preparation. No Odyssey/TotK
matched phase capture was supplied. Expect greater benefit during resource
turnover/loading if the A/B counters demonstrate churn; do not extrapolate
Jadyx's transfer-allocation milliseconds to our staging rings.

## Host validation

The final D3D12 group passes. No full MSVC/UWP compile can run on this Mac.
Strict pure-policy compilation includes
`-Wshadow -Wconversion -Werror` and ASan/UBSan; inherited D3D API doubles use
`-Wshadow -Werror` because they have existing conversion warnings. New production
arithmetic is explicitly 64-bit, with distinct local names and no new narrowing.

New/extended executable fixtures cover:

- Saturated headroom arithmetic, reserve/exact-fit boundary, maximal integers,
  missing telemetry, zero limit, live usage changes and latched unset/0/1 switch.
- Production committed creation with both optional wait outcomes, exact allocation
  accounting and an injected OOM that still performs unconditional retry.
- Production placed allocation, free-slot reuse, growth, heap isolation, live
  leases, opt-out/no-placed paths and failed placed allocation rollback, with and
  without headroom; check the **heap** size passed to the decision.
- Production staging collection/request: recent reuse, age 15/16 boundary, busy
  GPU page, CPU lease, last-use refresh, emergency override, completed-page
  lifetime, cursor reset, ring cap and oversized fallback. A 32-frame simulated
  recurring critical sweep creates 32 pages with immediate trimming versus 1
  with the recent-use policy, for identical transfer extents; this is a resource
  creation count, not an Xbox time measurement.

The initial group run overlapped edits: 328 tests, 3 skips, 3 errors in 237.172 s.
Two view-cast extraction errors used a module loaded before its new adapter stub
was added; the subsequent isolated 4-test run passes. The third was pre-existing:
`test_profile_without_present_has_finite_rates_and_visibility` lacked current
frame-generation/display fields in its `FrameProfile` double. Independently
reproduced using `git show HEAD:.../d3d12_context.cpp` without altering production
files (`/tmp/p91-baseline-visibility.log`). Added the missing fixture fields and
namespace alias; all 6 guest-visibility tests then pass. This is a fixture repair,
not a change to profiling behavior.

Final verification after the fixture repair and all production/test edits:

```text
python3 -m unittest discover -s tests/port -p 'test_d3d12*.py'
Ran 330 tests in 191.764s
OK (skipped=3)
```

Full output: `/tmp/p91-d3d12-verified.log`. Individually reran the touched
headroom (2), staging-pressure (1), buffer-pressure-reuse (1), view-casts (4)
and guest-visibility (6) tests: all pass. Also ran the affected allocation
neighbors outside the D3D12 filename group:

```text
python3 -m unittest discover -s tests/port -p 'test_recorder.py'
Ran 2 tests in 1.462s — OK
python3 -m unittest discover -s tests/port -p 'test_t7_buffers.py'
Ran 4 tests in 2.195s — OK
```

Changed C++ ranges formatted with local clang-format; changed Python files
formatted with cached ruff. `git diff --check` passes. No commit, push, staged
files, downloaded tool or build/package ID. Console timing, image equivalence
and the MSVC C4456/C4457/C4459/C4244 checks remain unverified.

## Exact console acceptance run for the tester

Build/package run ID: **none**; no console operation was performed here. Build
this uncommitted patch through the owner's normal path. Use the same binary for
both env settings, full restart between runs, unchanged settings/save/camera,
60 s warm-up then 60 s capture. Repeat A/B/B/A for each title:

1. MK8D: reproduce `r-mk8d-rc90`, same race/lap segment.
2. Wonder: reproduce `r-wonder-all-on`, same level/camera.
3. Odyssey: same save and repeatable kingdom traversal, including a resource
   turnover segment, then a stationary steady segment.
4. TotK: same save and repeatable area traversal/menu transitions; retain the
   same existing rendering/shader switches in both runs.

Use the following common environment in every run:

```text
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_THREAD_PROFILE=1
NXBOX_FRAME_GEN=0
NXBOX_FRAME_PACING=0
NXBOX_P73_CRITICAL_TRACE=0
NXBOX_D3D12_DEFERRED_RECORDING=1
NXBOX_D3D12_FLUSH_DRAWS=128
```

A: `NXBOX_D3D12_ALLOCATION_HEADROOM=0`.
B: `NXBOX_D3D12_ALLOCATION_HEADROOM=1`.
Remove `NXBOX_RECORD`/`NXBOX_RECORD_DELAY`. Keep synchronization, resolution,
shader-cache contents and other rendering flags identical. Do not enable
SUBMIT_TRACE or synchronous validation in timing runs. Do not force memory
pressure by changing console storage/system settings.

For every `D3D12_FRAME_PROFILE` window, normalize totals by `frames` and compare
per draw as a content control. Under demonstrated pressure, success requires:

- `forced_submits_memory_pressure / frames` falls on headroom-eligible allocation
  misses; `fence_wait_cpu_ms / frames` and `fence_wait_wall_ms / frames` fall for
  those avoided waits. Do not add both wait fields to the same ledger.
- `staging_new_resources / staging_requests` falls during recurring critical-cache
  sweeps; `upload_allocation_cpu_ms / frames` and/or the containing texture/upload
  scopes should fall. `uniform_snapshot_new_resources` is a separate domain;
  snapshot-ring trim behavior was not changed at its call site.
- `forced_submits_allocation_retry` must not materially increase; zero remains the
  desired steady value. No new corruption, crash, device removal or unexplained
  sustained growth in memory. Record process usage/limit with the owner's normal
  memory diagnostics if either approaches the reserve boundary.
- No required decrease in `pipeline_lookup_cpu_ms`, `descriptor_setup_cpu_ms`,
  `descriptor_copies`, `root_cbv_binds` or draws: these mechanisms already existed.
  A window with zero relevant pressure events can validly show zero speedup.

Track approximate producer CPU with p74's ledger:
`(sum(all *_cpu_ms) + gpu_queue_idle_wall_ms) / frames * gpu_thread_cpu_pct / 100`.
Report medians/ranges, pressure counts and thread-profile CPU, separating wall
wait reduction from actual CPU reduction. Preserve the environment, diag and
screenshots. Outside timing, compare HUD/text/materials, bloom, image/storage
feedback and transitions in all four games, plus save/menu/resume and near-limit
recovery. Host ownership tests do not prove Xbox rendering or MSVC compilation.
