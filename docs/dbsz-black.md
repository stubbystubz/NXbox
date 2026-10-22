# DBSZ black sky / rocks (#62)

Title: `010035F022078000`. Investigation on `p63/dbsz-black`.

## Status and evidence boundary

**The visual root cause is not established. No rendering fix is claimed.** The
available logs do not identify the draw producing a black pixel, its shaders, or
the textures bound to that draw. This change supplies a bounded, opt-in metadata
capture to obtain those missing inputs. Xbox execution and a Windows/UWP build
are still required. A shader or binding correction must follow that evidence;
globally weakening depth/stencil tests or replacing lighting samples is not a
verified fix.

The inspected reference image is
`compat-2026-10-18/r-dbsz-rc86/shots/shot-0190s.png`: the sky and large rock
surfaces are black, while Goku, the floor, and HUD remain visible. This differs
from the older image described in `dbsz-rendering-performance.md`; neither image
should stand in for every run.

Searched the supplied `r-dbsz-rc86`, `r-dbsz-diag`, `r-dbsz-poll`, `dbsz-live`,
`dbsz-fix`, `dbsz-rc`, `dbsz-final105`, `z-dbsz`, `w-dbsz`, `u-dbsz`, `s-dbsz`,
and `q-dbsz-np` logs and environment files. Older runs use
`eden_uwp_diag.txt` / `eden_log.txt`, rather than `diag.txt` / `eden.txt`.
Some older diagnostic files append multiple launches; a warning in one launch
cannot be assigned to another launch's screenshot.

Concrete observations:

- The late `r-dbsz-rc86` frame profiles report zero skipped draws, pending-PSO
  skips, shader translations, DXC compilations, and PSO compilations. The prior
  resource-conflict fix does not explain the remaining black image.
- Older `dbsz-fix`, `dbsz-rc`, and `dbsz-final105` diagnostics contain an SRV
  dimension 10 -> 9 warning for a 128x128, seven-layer, single-sample 2D resource.
  These are CubeArray -> Cube **view dimensions**, not a physical 3D texture.
  The current `AdaptSrvDimension` can expose the first six faces when the range
  begins at zero. Without the old view's base face or shader hash, the warning
  does not identify a skylight or a black-rock material. That warning is absent
  from `r-dbsz-rc86`.
- `r-dbsz-rc86/eden.txt:2315`, `2713`, `3152`, `3153`, `5589`, and `5867` report
  irreversible ASTC readback. The printed `type=1` is `ImageType::e2D`, not e3D.
  `Image::DownloadMemory` emits this warning when declining a reverse conversion;
  it is not evidence of a failed upload or an empty volume texture.
- No `NXBOX_D3D12_DEPTH_EQUAL_AS` or `HLSL_PRECISE_POSITION` result/configuration
  was found in these supplied diagnostic/environment files. The hypotheses in
  the task are not treated as successful tests or silently reapplied here.
- No supplied draw trace, pass manifest, or HLSL dump identifies the faulty pass.
  Zero API warnings alone would not establish correct shader arithmetic or
  populated G-buffer contents.

## Jadyx comparison

Read the reference checkout's `docs/xbox/xbox_deferred_lighting.md`,
`xbox_terrain_seams.md`, `xbox_rtv_bindings.md`, `xbox_srv_dimensions.md`, and
`xbox_geometry_passthrough.md`. These concern the same broad rendering paths,
but their confirmed game-specific diagnoses are not evidence of a DBSZ cause:

- Deferred lighting: isolate the first wrong producer, compare its inputs, then
  inspect interpolation / helper-lane continuation. Jadyx's confirmed fixes
  include NoPerspective interpolation and demote/return handling.
- Terrain seams: compressed texture logical dimensions can differ from physical
  block storage. This does not prove that DBSZ's black silhouettes are BC seams.
- RTV bindings: inactive overlapping slots must retain shader slot numbering;
  binding a duplicated physical target is different from sampling black data.
- SRV dimensions: inspect the physical resource **and** view range and shader
  declaration; a fallback view with a different dimension is not a legal substitute.
- Geometry passthrough: preserve per-vertex outputs and avoid duplicate depth
  conversion when lowering the guest geometry stage.

NXbox emits HLSL directly; Jadyx's SPIR-V/Mesa-specific fixes cannot simply be
transplanted. No source was copied from the reference checkout.

## Added diagnostic

`NXBOX_D3D12_LIGHTING_TRACE_FRAME=<positive integer>` selects one texture-cache
frame (`frame_tick + 1`, matching the existing draw trace). Unset, invalid, or
`0` disables it. Configuration is latched: restart NXbox after changing it.
Do not combine it with `NXBOX_D3D12_DRAW_TRACE_FRAME`; the older selector takes
precedence if both are supplied.

The selected frame produces:

| Record | Meaning |
| --- | --- |
| `D3D12_LIGHTING_BEGIN` | Attempt number and guest VS/PS hashes |
| `D3D12_LIGHTING_CBV` | Stage/register, guest cbuf address, source binding, offset and size |
| `D3D12_LIGHTING_SRV` | TIC/address, shader type, actual resolved SRV dimension/format/swizzle/range, physical resource, feedback replacement, and effective sampler |
| `D3D12_LIGHTING_DRAW` | Attempt -> submitted draw mapping, stencil references, budget flags |
| `D3D12_LIGHTING_TARGET` | Actual color/depth resource identity and attachment format, guest address/view range, render layer count, replacement flag, color write mask; slot -1 is depth |
| `D3D12_DRAW_TRACE` | Existing final PSO, viewport/scissor, draw parameters and shader hashes |

Stage indices are 0=VS, 1=TCS, 2=TES, 3=GS, 4=PS. `resource` is a process-local
host identity, useful for finding aliases; do not compare it across launches.
`mip`/`levels` retain the guest view selection, which adaptation preserves.
`srv_first`/`srv_layers` describe the resolved view, including cube widening.
For `bound=false`, the SRV is null: only `srv_dim` is meaningful; other SRV fields
are diagnostic sentinels, not the actual null descriptor's format.
CBV records describe the source before the existing zero-buffer fallback.
Target `view_*` fields describe the guest view. An extent-copy replacement binds
its temporary mip/layer zero; `render_layers` is the render area's layer count,
not a claim that a 3D RTV exposes only that many W slices.
Sampler state is reconstructed using the same `MakeSamplerDesc` as binding,
including when the sampler table is reused.

Attempts without a matching `LIGHTING_DRAW` were not submitted after binding.
Metadata is capped at 1024 attempts and 8192 CBV/SRV records. A submitted draw
with `binding_limit=true` or `attempt_limit=true` means the trace may be incomplete.
The existing draw trace separately caps at 3000 submitted draws. No GPU resources,
readbacks, copies, fences, waits, or pass dumps are added. The existing draw
trace's vertex readback is disabled in lighting-trace mode. The selected frame's
logging can perturb timing; exclude it from FPS comparisons.

The optional `WriteImageSrv` output records the descriptor **after** adaptation.
Recording `view.Description()` alone would misleadingly report the original
CubeArray/2DArray dimension when a different descriptor actually reaches D3D12.
Without the diagnostic output pointer the binding decision is unchanged.

## Exact next console check

Build/package this worktree through the existing Windows/UWP build workflow.
The Mac host tests do not compile the Windows renderer. Keep the title/update,
save, settings, and shader caches from `r-dbsz-rc86`; do not clear caches.

Put these lines in the installed package's `LocalState/nxbox_env.txt`, preserving
any unrelated existing overrides:

```text
NXBOX_RENDERER=d3d12
NXBOX_D3D12_LIGHTING_TRACE_FRAME=3000
NXBOX_D3D12_DUMP_HLSL=1
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_THREAD_PROFILE=1
NXBOX_GPU_METHOD_PROFILE=1
```

Leave the previous depth/precise-position experiments and sample-replacement
diagnostics unset. Do not enable a pass dump for this first run. Restart, select
DBSZ, and follow the same tutorial route as rc86: A every three seconds from
40 through 169 seconds after launch, then leave Goku/Vegeta idle until at least
220 seconds. Frame 3000 falls in the fight on the archived rc86 route (profile
windows reach 2874, 3004, and 3119 cumulative frames). Cache warmth and navigation
can shift that placement: confirm the captured draw list belongs to the fight,
rather than assuming elapsed time proves it. Take a screenshot at 190 seconds
and again after `D3D12_DRAW_TRACE frame=3000` appears.

Collect `eden_uwp_diag.txt`, `eden/log/eden_log.txt`, the emitted
`eden/log/gfx_*.hlsl` and `gfx_*.hash`, the screenshots, and the effective env
file. The HLSL dump is an existing compilation diagnostic; it does not invalidate
DXIL caches. Remove the two new capture controls and `NXBOX_GPU_METHOD_PROFILE`
after collecting the run.

Next analysis: join BEGIN/DRAW/targets/SRVs by frame and attempt; identify G-buffer
producers and the fullscreen consumers, then inspect their matching HLSL. Check
null/mismatched views, wrong formats/swizzles/ranges, reused resource identities,
comparison samplers, and unwritten attachments before changing shader math.
`NXBOX_GPU_METHOD_PROFILE=1` also splits the large Maxwell command cost on that
run. If bindings are legal, metadata alone cannot prove their texel contents:
use the identified pass/hash for a focused output/input capture next. This first
run is a diagnostic gate, **not** visual-fix acceptance.

## Fight CPU costs

Source: `r-dbsz-rc86/diag.txt` profile lines 4529, 4705, 4893, 5058, 5245, 5427.
Six late windows, **758 frames** total. Values below are the sum of each raw
counter divided by 758, not an average of differently sized windows.

| Recording-thread phase | ms/frame |
| --- | ---: |
| DMA pushbuffer processing | 5.858 |
| Maxwell3D methods | 5.377 |
| Descriptor setup | 2.231 |
| Buffer-cache binding | 2.113 |
| Texture-cache update | 1.529 |
| Draw recording | 1.461 |
| Submission | 1.358 |
| Pipeline lookup | 0.872 |
| Index FlushCaching | 0.801 |
| Macro execution | 0.791 |
| Fermi2D | 0.455 |

These named phase clocks are exclusive while installed on the recording thread;
they are not stack samples. Unclassified blocking can still fall into a phase
named `*_cpu_ms`. The DMA figure includes a transient 9.68 ms/frame window;
the other five are approximately 5.1-5.2 ms/frame. Detailed Maxwell method
profiling was disabled in the archived run, so its 5.377 ms cannot yet be
assigned to dirty flags, cbuf writes, draw state, or query methods.

Hardware GPU time is **6.721 ms/frame**. The separate recorder thread uses
**6.019 ms/frame**; that overlaps the producer and GPU. Fence-wait wall time is
2.125 ms/frame and GPU queue idle time is 8.267 ms/frame. Do not add these waits
or recorder CPU to the producer phases to predict FPS. `GAME_PRESENT` reports
roughly 23-26 fps in these windows. This is not evidence of a saturated GPU.

There are **49.43 ExecuteCommandLists/frame**, of which **42.37** are guest-fence
submissions, and about **5.875 MiB/frame** of buffer downloads. The existing
`NXBOX_D3D12_FENCE_BATCHING=1` is a concrete A/B for excessive tiny submissions;
the opportunity represented by guest-fence share of submit time is about
`1.358 * 42.37 / 49.43 = 1.16 ms/frame`, not a measured saving. Existing
`NXBOX_D3D12_ASYNC_DMA=1` independently tests DMA-only waits while preserving
CPU-required readbacks. See `D3D12-DBSZ-submission-pressure.md` for the already
implemented policies and lifetime tests. No additional default timing change is
justified here, and these opportunities do not explain a 40 ms frame alone.

One software Fermi2D blit/frame transfers only **16 guest bytes/frame**, yet
causes about one texture-download submit/frame. The 0.455 ms Fermi phase and
roughly 0.39 ms/frame `finish` wait are useful upper bounds, not proof that the
16-byte copy is redundant. Eliminating its coherence wait requires identifying
the source/destination and guest dependency; skipping it blindly can corrupt
lighting. Likewise the buffer download byte count includes async snapshots and
cannot all be removed as waste.

## The approximately 40-second gap

`r-dbsz-rc86/diag.txt:1210` records `worst_gap_ms=39356`. Between the last
three-frame presentation window at line 457 and resumed presentation at 1210,
the intervening zero-present windows at lines 520-1062 contain:

- 5.832 seconds aggregate guest JIT work (4.354 seconds emission, nested in JIT);
- 3.999 seconds aggregate I/O, maximum individual I/O 439 ms;
- zero shader-compile stalls and zero texture-cache GC calls.

GPU-thread samples are almost idle, and the D3D12 profile records no shader
translation, DXC, or PSO compilation during this gap. PSO prewarm completed
earlier at line 186: 246 records / 9.351 seconds wall, all DXIL disk hits,
zero failures. Its 34.996 seconds **aggregate driver work across four workers**
is not a later 35-second single-thread stall.

Memory rises from about 2.20 GiB to 3.50 GiB across the zero-present windows,
then reaches 3.95 GiB on resumed presentation, below the 5 GiB limit. Later
memory gets much tighter, but the early gap has no measured GC/renderer-pressure
cause. It precedes the first approximately 500-draw/frame fight windows; the
logs do not justify relabeling it as one continuous fight-load shader compile.
Guest initialization/compilation and I/O are observed, but do not account for all
39.356 seconds or identify a single guest function. There is no supported cheap
renderer fix for the remaining wall time from this evidence.

The separate `r-dbsz-poll/eden.txt` contains 19,862 completed AM/FS trace calls
(the actual records are in `eden.txt`, not its diagnostic file). Thread 82's
`OpenDataStorageByCurrentProcess` takes 5.319 seconds once at startup (line 213);
the second call takes 8 microseconds. Across that whole run, the repeated
friend-invitation pop and common-state queries on thread 82 total only about
46 milliseconds of handler time over 12,108 calls. That trace does not support
making those normal empty-channel polls sleep or suppressing them to remove a
40-second gap. Handler totals exclude guest IPC/scheduling overhead and are not
a complete accounting of the loading thread.

## Host validation

`test_dbsz_lighting_trace.py` executes the production budget under ASan/UBSan,
including disabled/wrong-frame selection, repeated rejected attempts, caps,
and independent renderer instances. Separate processes test strict frame parsing
(unset, empty, zero, negative, trailing garbage, whitespace, overflow, valid)
and latching. It also checks that trace mode suppresses
vertex readbacks and records the resolved descriptor. The extended production
SRV binding harness checks adapted cubes, retained incompatible views, and null
diagnostic output without changing descriptor writes.

Targeted lighting, SRV, draw-trace, sampler-reuse, and descriptor-table tests pass.
The complete acceptance run reports **1020 tests in 885.033 seconds, OK
(skipped=45)**, plus 651265 differential cases. `git diff --check` passes.

This Mac run used Homebrew Python 3.14 / pkg-config. The existing font-test
modules require `fontTools` at import time; the host's Python did not have it on
its normal module path. An existing cached fontTools 4.67.0 copy supplied that
test dependency without installing anything or changing project dependencies:

```sh
# From this worktree, with Homebrew Python and pkg-config on PATH.
mkdir -p /private/tmp/nxbox-p63-tests
cd tests/port
PYTHONPATH=/Users/miguel/.cache/uv/archive-v0/JNxAtyjfuxFcSQC1 \
  TMPDIR=/private/tmp/nxbox-p63-tests PYTHONDONTWRITEBYTECODE=1 \
  python3 -m unittest -q
```

The test scratch, superseded logs, and formatting cache were removed afterward.
No Windows/UWP build or post-change console run was performed. No commit or push
was made.
