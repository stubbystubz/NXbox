# NXbox / Jadyx renderer comparison

Study date: 2026-10-21. Reference: **Jadyx by JulianDr14**, commit **22c67b4**
(<https://github.com/JulianDr14/jadyx>), read-only partial checkout supplied for
this task. All Jadyx source paths below are relative to that checkout; NXbox
links resolve in this repository. Attribution: [third-party notes](third-party-notes.md).
The supplied checkout HEAD is merge `a46b3e75beb35472b1d9639cdee9447975618d10`;
`git diff 22c67b4 HEAD -- src/video_core/renderer_d3d12 docs/xbox` is empty.
The reference is not a complete history or a runnable console build. Documentation
includes experiments, rejected changes, and later corrections; the current code
and the final status of each document take precedence over early proposals.

The Discord report about Mario RPG on Series S is a lead, not a controlled
measurement. Neither that report nor these ports proves native 60 FPS. The supplied
NXbox MK8D baseline is about 25 ms/frame of GPU-thread CPU work, including descriptor
setup 2.77 ms, draw recording 2.09 ms, pipeline lookup 1.06 ms, Maxwell methods
5.3 ms and DMA pusher 3.0 ms. These are our numbers, not Jadyx measurements. Reducing
renderer preparation cannot automatically remove the guest-command portion.

## Descriptors

Jadyx `d3d12_descriptor_heap.{h,cpp}` uses a fence-retired shader-visible resource
ring, compares ordered descriptor payloads with recent tables, and rewinds the
unused most recent reservation on a table hit. Allocation precedes draw recording
because exhaustion can submit/wait. A drained ring preserves its head so remembered
tables are not immediately overwritten. Samplers are interned by content; the
last-key span refers to immutable unordered-map node storage, and is cleared before
reset. CPU descriptor slots also respect recorder consumption. Their sampler table
is acquired before uniform/resource reservations so sampler exhaustion cannot
retire allocations before their draw.

NXbox already has more than a simple fresh-table-per-draw implementation:
[d3d12_descriptor.cpp](../src/video_core/renderer_d3d12/d3d12_descriptor.cpp) and
[d3d12_descriptor.h](../src/video_core/renderer_d3d12/d3d12_descriptor.h) implement
previous-table reuse, bounded content caches, source generation tracking, handle
fast paths and batched copies. Tables are scoped to recording ticks. The draw path
reserves after feedback/uniform copies and restores state across submissions in
[d3d12_rasterizer.cpp](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp).

Judgment: content reuse is already present; transplanting their allocator is not
an obvious win. Rewinding an unused reservation is interesting only where we
actually reserve before discovering a hit. Their sampler last-key span is less
valuable here because NXbox swaps retained plans instead of copying a vector on
every hit. Preserve our generation/lifetime checks. The self-contained missing
piece selected here is immutable stage descriptor counts (below).

## Pipelines, PSO cache and warmup

Jadyx separates guest keys, effective build identity, linked shader compilation,
root signatures and native PSOs (`d3d12_pipeline_cache.cpp`,
`d3d12_pipeline_residency.h`, `d3d12_pipeline_key_tracking.cpp`). It shares work
already in flight, retains recently drawn PSOs, and persists a hot set. **16 is
sessions**, not a universal 16-second residency timeout: `HotPipelineSet` retains
history for 16 sessions, selects at most 4096 boot PSOs and uses 2048 on a first
session. Idle residency is pressure dependent: unlimited with >1536 MiB free,
3 minutes normally, 10 seconds under low headroom, 1 second at emergency (frame
counts assume 60 FPS), with a 256-entry sweep. Background prewarm requires ample
headroom. Driver PSOs are estimated in source comments at roughly 80–100 KiB;
17,000 eager PSOs reportedly consumed about 1.3 GiB before gameplay. These are
reference workload observations/estimates, not a measured NXbox object size.
An experimental previous-pipeline reuse mode skips lookup when relevant
registers and other dependencies remain unchanged. Its policy starts in Off mode
and also provides Check mode; it is not an established default-on performance win.

NXbox already has generation-keyed previous-pipeline reuse, async in-flight work,
normalized PSO identity, interned roots, recipes and native disk cache:
[d3d12_pipeline_cache.cpp](../src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp),
[d3d12_async_pso.h](../src/video_core/renderer_d3d12/d3d12_async_pso.h),
[d3d12_disk_pipeline_cache.cpp](../src/video_core/renderer_d3d12/d3d12_disk_pipeline_cache.cpp),
[d3d12_pso_frame_budget.h](../src/video_core/renderer_d3d12/d3d12_pso_frame_budget.h).
`RecordGraphicsDraw` tracks render-target draw recency (for render-target history),
not a persisted session-aged hot PSO set; existing prewarm loads validated recipes. This is not equivalent to Jadyx's pressure-aware native
PSO eviction and session-aged hot-set selection.

Judgment: hot-set/residency policy is promising for memory and startup hitches,
but involves pointer ownership, async builds, draw recording, persistence and
memory pressure; defer a port until those lifetimes are tested together. Another
previous-pipeline shortcut would duplicate an existing NXbox fast path.

Jadyx `docs/xbox/xbox_pipeline_preparation.md` reports Series .32→.33: weighted
57.01→57.72 FPS; aggregate pipeline preparation 6460.5→2272.7 ms; reported hitches
20→7; worst interval 1549.13→1062.36 ms. Window-median P95 **worsened**
17.22→17.41 ms and P99 19.60→20.89 ms. Boot avoided 701/1447 real PSO builds;
gameplay had 108 hits/150 requests. Sampling, history, driver cache and manual
routes differed, so those are observations, not causal speedup estimates.

## Command-list submission

Jadyx `RasterizerD3D12::FlushIfUploadHeavy` submits at 512 draws or 32 MiB pending
uploads; `FlushCommands` also submits draw work or 8 MiB at a guest-list boundary.
Its comment describes roughly 3900 draws taking about 50 ms to record before the
GPU could start. The aim is overlap, not faster GPU execution.

NXbox already calls `Context::FlushDrawsIfDue` between draws, with configurable
threshold policy (128 draws by default) and reason counters in
[d3d12_context.cpp](../src/video_core/renderer_d3d12/d3d12_context.cpp) and
[d3d12_rasterizer.cpp](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp).
Staging pressure submits the producer before waiting. Judgment: tune existing
thresholds on console; no separate 512-draw mechanism is needed. More submissions
can also add CPU work and shorten descriptor reuse opportunities.

## Uniform / CBV binding

Jadyx counts descriptor layouts once in `d3d12_stage_bindings.*`, uses statically
read cbuffer sizes in `d3d12_graphics_pipeline.cpp`, streams the shader's required
reach, and zeroes unbound/out-of-range portions for root CBVs. Its
`d3d12_uniform_stream.h` suballocates 256-aligned uniforms from 4 KiB staging
reservations within a submission; larger uniforms keep the original path.
It copies current guest bytes on every bind, rather than caching mutable contents.

NXbox uses root CBVs when the 64-DWORD root limit permits, falling back to tables:
[d3d12_root_signature.h](../src/video_core/renderer_d3d12/d3d12_root_signature.h),
[d3d12_buffer_cache.cpp](../src/video_core/renderer_d3d12/d3d12_buffer_cache.cpp).
Aligned direct buffers, read limits, versioned snapshot copies and binding reuse
already avoid much work. Dynamic cbuffer offsets still request full guest ranges;
root bindings and zero padding have their own correctness contracts. Before this
change, mapped uniforms each called the 512-aligned staging ring, and the graphics
draw scanned immutable descriptor vectors to count every stage on every draw.

Judgment: slab allocation and cached counts are low-risk CPU candidates, now
adapted here. More aggressive shader-read-size pruning needs differential shader
coverage, especially dynamic indexing and unbound reads; do not simply shrink all
root buffers. The slab does not solve that separate semantic problem.

Jadyx `docs/xbox/xbox_draw_preparation.md` reports isolated final host medians:
256-byte staging reserve 35.72→4.08 ns; metadata 18.3756→1.2539 ns/stage.
A final PC gameplay window measured inclusive draw cost 8.216039→7.663529 us/draw
(6.72% lower), 29.672333→27.612333 ms/frame, but FPS **fell** 15.83949→15.01426.
Draw density differed by 0.23%, and churn/waits differed. This was not deterministic
replay, nor attribution to either piece. Their IA view-shadow experiment was
removed: only ~4.6–4.9% views avoided, with changing-view microcost 71.69→80.765 ns.
Do not revive that rejected candidate on the strength of its call counts.

## Transfer and upload buffers

Jadyx `d3d12_transfer_buffer_pool.*` shares reusable DEFAULT buffers between caches
for reinterpretation, row repack, plane split and buffer self-copy. Acquire only
returns completed submissions in COMMON, with UAV capability in the key. Idle
pool ceiling is 64 MiB; stale retention is 2048 ticks normally or 16 under pressure.
The source records that clearing every pressured frame recreated about 11 buffers /
6 MiB per frame and cost about 20 ms of GPU-thread work in one workload. This is
not a general benchmark. Upload/readback staging has separate retirement policy.

NXbox [d3d12_context.cpp](../src/video_core/renderer_d3d12/d3d12_context.cpp) already
pools placed buffer heaps and staging pages with fence and CPU lease protection.
[d3d12_buffer_cache.cpp](../src/video_core/renderer_d3d12/d3d12_buffer_cache.cpp)
has versioned DEFAULT uniform snapshot pages. Some texture reinterpretation bounce
resources are still created in
[d3d12_texture_cache.cpp](../src/video_core/renderer_d3d12/d3d12_texture_cache.cpp).
Judgment: a reusable transfer-resource pool could beat repeated resource creation
on those paths, but must distinguish our existing heap reuse from resource reuse.
It is less universal than uniforms and needs state reset, concurrent leases and
pressure trim coverage. It is deferred, not counted as a port in this task.

## Texture memory and heaps — handoff to p60

Jadyx `d3d12_resource_allocator.cpp`, `d3d12_heap_growth_policy.h` and
`docs/xbox/xbox_texture_memory.md` combine:

- Best-fit compatible aligned holes, coalescing, fence-delayed frees, normal
  64 MiB blocks; large blocks align to requirements instead of power-of-two growth.
- Pressure tests that include the proposed growth: app margin 768 MiB or GPU
  free margin 20%. App and GPU budgets are checked independently, never added on UMA.
- Pressured growth in 16 MiB steps; exact aligned capacity when that padding would
  leave at most 256 MiB app headroom. This corrects the earlier exact-heap-per-image
  churn. Do not copy only the older exact-sizing proposal from the document.
- Collect before growing; under exhaustion wait for earlier submitted GPU work
  on the recording thread before retrying. Waiting on every pressured miss was
  rejected as a GPU-thread stall. Newly empty heaps survive 64 submission ticks
  in settled mode; emergency reclaim may trim more aggressively.
- Bounded deferred GPU-dirty readback for GC, validating CPU/GPU write generations
  before committing guest bytes; synchronous fallback under emergency or when
  unsuitable. Deletion, staging retention and allocator retirement are distinct.

NXbox has app-aware cache watermarks, placed resource accounting and deferred
resource lifetimes:
[d3d12_memory_budget.h](../src/video_core/renderer_d3d12/d3d12_memory_budget.h),
[d3d12_texture_cache.h](../src/video_core/renderer_d3d12/d3d12_texture_cache.h),
[d3d12_texture_cache.cpp](../src/video_core/renderer_d3d12/d3d12_texture_cache.cpp),
[d3d12_context.cpp](../src/video_core/renderer_d3d12/d3d12_context.cpp), and generic
[texture_cache.h](../src/video_core/texture_cache/texture_cache.h).
Judgment: p60 should compare growth granularity, retired-but-not-free bytes,
readback waits and churn before changing watermark ages. No texture budget or heap
policy was modified by p61.

Jadyx's September PC trace had Emergency pressure, minimum 120.660 MiB app headroom,
GC maxima 35.796/22.946 ms, 395/429 evictions and 258/250 later recreations at the
same addresses. DXGI usage was only ~665–675 MiB against 3447 MiB. Same address
does not prove same contents; heap capacity minus a different-time peak live
allocation does not measure fragmentation. Later code supersedes early proposals;
these traces do not demonstrate the performance of the October 21 heap policy.

## Total memory versus the 5 GiB app cap

Both projects must fit guest DRAM, JIT, native shader/PSO allocations, texture heaps,
buffers, staging, readback and frontend state in the process limit. Free GPU budget
alone is insufficient. Jadyx uses app headroom guards, bounded staging/transfer
pools and pressure-sensitive PSO residency; its PC 5120 MiB Job limit is useful
but does not reproduce Xbox UMA or driver allocations.

NXbox's app memory query and cache thresholds are in
[d3d12_context.cpp](../src/video_core/renderer_d3d12/d3d12_context.cpp), with buffer
policies in [d3d12_ring_allocator.h](../src/video_core/renderer_d3d12/d3d12_ring_allocator.h).
Judgment: Jadyx's coordination of optional PSOs, retired resources and transient
uploads is more actionable than simply raising the texture cap. For TotK/BotW,
measure sampled and peak process commit plus in-flight allocation demand. The
uniform slab reduces alignment waste for tiny uploads but is not an OOM fix.

## Game bugs and shader correctness

| Area and Jadyx evidence | NXbox implementation / judgment |
|---|---|
| **Hyrule crash**: `xbox_hyrule_crash.md` is about **Hyrule Warriors**, not a demonstrated TotK/BotW fix. Old crashes end in `spirv_to_dxil.dll` with allocation failures; one sample is 5066/5120 MiB. Newer sample is 5045/5120 MiB, then a null write while translating compute. A failed 64 MiB staging allocation was reclaimed/retried. Root cause remains unproven. | Our native path uses HLSL/DXC in [d3d12_dxc.cpp](../src/video_core/renderer_d3d12/d3d12_dxc.cpp) and [d3d12_pipeline_cache.cpp](../src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp); the Mesa diagnostic route is distinct. Headroom evidence is relevant to TotK, but there is no crash patch to port or proof that its crash is ours. |
| **Terrain seams**: `xbox_terrain_seams.md` explicitly leaves the cause unresolved. A Cube request on a one-layer BC3 resource does not establish the terrain material or justify manufacturing cube faces. No certified terrain fix. | Our [texture upload/transfer path](../src/video_core/renderer_d3d12/d3d12_texture_cache.cpp) and [footprints](../src/video_core/renderer_d3d12/d3d12_footprint.h) have distinct upload/readiness, small-mip and array fixtures. Compare complete mip/layer publication and per-draw captures; do not infer identical causes from similar screenshots. |
| **Deferred lighting**: `xbox_deferred_lighting.md` and `xbox_magnesis.md` report reconstructing NoPerspective through smooth weighted attributes / fragment W when clipping crosses W=0. The diagnostic reports 24731/32768 out-of-tolerance points before versus 0 after; user confirmed Series lighting. | [hlsl_emit_context.cpp](../src/shader_recompiler/backend/hlsl/hlsl_emit_context.cpp) emits `noperspective` directly. This is a plausible missing platform workaround, with strong reference evidence, but needs replay with our generated HLSL and clip conventions before lowering every varying. High game-bug priority; no FPS claim. |
| **Discard / helper derivatives**: Jadyx's conservative `DemoteReturnPass` removes terminal returns in already-discarded arms only when it can prove side effects and control flow safe. Four MRTs matched PC exactly at 1618 interior pixels each; Series user confirmed tubes. Later water cases still exceed its safe transformation coverage. | [emit_hlsl_special.cpp](../src/shader_recompiler/backend/hlsl/emit_hlsl_special.cpp) implements demote via HLSL discard, with different compiler lowering. Captured neighbor-lane/derivative tests are needed. The Mesa/SPIR-V pass is not a drop-in HLSL fix. Strong correctness candidate, broader than this renderer-only port. |
| **Magnesis**: `xbox_magnesis.md`, Oct 20, says PC works while Series attachment still fails. It proposes bounded captures and correlated CPU readbacks; hashes/zeros in sparse diagnostics do not prove corruption. | Our [d3d12_query_cache.cpp](../src/video_core/renderer_d3d12/d3d12_query_cache.cpp), [buffer cache](../src/video_core/renderer_d3d12/d3d12_buffer_cache.cpp), and [fence manager](../src/video_core/renderer_d3d12/d3d12_fence_manager.cpp) need action-correlated visibility testing. No known Jadyx functional fix to import. |
| **Geometry passthrough**: `xbox_geometry_passthrough.md` fixes output preservation after EmitVertex, masked components, Generic31Y/Z/W classification, depth conversion and Mesa interface linking. PC component/visual confirmation; Series validation pending in that document. | NXbox lowers passthrough in [translate_program.cpp](../src/shader_recompiler/frontend/maxwell/translate_program.cpp); [HLSL context](../src/shader_recompiler/backend/hlsl/hlsl_emit_context.cpp) still rejects an unsupported passthrough linkage. Audit emission component masks/output lifetime separately from Mesa linkage. Relevant shared-IR semantics; no assumption of Series certification. |
| **SRV dimensions**: `xbox_srv_dimensions.md` corrects wrong-dimension fallback and double-free from storing one descriptor under two view types. A Cube requires a 2D resource with enough faces, not a distinct resource dimension. | NXbox [d3d12_descriptor.cpp](../src/video_core/renderer_d3d12/d3d12_descriptor.cpp) constructs typed null SRVs and checks dimensions; [texture cache](../src/video_core/renderer_d3d12/d3d12_texture_cache.cpp) owns views. Existing dimension/null-view fixtures already address much of this class. Keep requested dimensions and unique descriptor ownership; their exact ownership bug is not established here. |
| **RTV bindings**: `xbox_rtv_bindings.md` reports 497 debug errors for an aliased inactive slot. It masks inactive attachments consistently in PSO and OM bindings, preserves internal target indices with typed null RTVs, and keeps target 0 for alpha-to-coverage. No FPS measurement. | NXbox [rasterizer](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp), [PSO tables](../src/video_core/renderer_d3d12/d3d12_pso_tables.h) and [state tracker](../src/video_core/renderer_d3d12/d3d12_state_tracker.h) handle formats/attachments but do not establish equivalent inactive-alias pruning. Good targeted follow-up: differential coverage and MSAA/A2C tests before changing bindings. |
| **BC block copy**: `xbox_bc_block_copy.md` preserves original native BC blocks beside decoded arrays, with per-block coverage/version invalidation. This completes bit-exact BC3↔UINT128 reinterpretation even when the sampled image is decoded; 192 skipped-copy reports motivated it, without proving one image caused all 192. | NXbox [texture cache](../src/video_core/renderer_d3d12/d3d12_texture_cache.cpp) has decoded BC formats and GPU reinterpretation checks. Original endpoint/index bits cannot be reconstructed from decoded colors. Investigate lossless sidecar preservation for unsupported decoded-array copies; account for duplicate memory under the 5 GiB limit. More substantial than a small copy predicate change. |
| **FSR**: `xbox_fsr.md` uses FP32 pixel-shader EASU/RCAS and a single 32-bpp intermediate, avoiding unsupported native FP16 and typed UAV loads. | NXbox already has FSR policy and tests in [present_math.h](../src/video_core/renderer_d3d12/d3d12_present/present_math.h) and the separately owned present subsystem. Presentation upscaling does not make emulation natively 60 FPS. No present files changed. |

## Frontend features

Jadyx's `xbox_frontend.md`, `xbox_frontend_refactor.md`, `xbox_content.md`,
`xbox_updater.md`, `xbox_rom_storage.md`, `xbox_network_portal.md` and
`xbox_pulse.md` describe a controller-first library, persistent controller layout,
per-game managers for content/mods/cheats, USB storage, update delivery, resumable
LAN import and persistent emulator sessions. The LAN portal has pairing,
checkpoints, resume and publication validation. It reports a real PC transfer of
7,268,474,425 bytes, a resumed offset beyond 4 GiB, and a **point sample** of
14.8 MiB/s on Series Wi-Fi, not a completed-transfer average. Connection/pairing
was confirmed on console; real import/UI and sustained Series performance gates
are not all complete. Session restore is far larger than a frontend feature:
revision/codec fixes and restore audits span guest services and GPU state.

NXbox already has library/content/mods/cheats managers in
[src/eden_uwp/ui](../src/eden_uwp/ui),
[library transfers](../src/frontend_common/library_transfer.h),
[USB library](../src/eden_uwp/usb_library.cpp),
[FTP](../src/eden_uwp/ftp/ftp_server.cpp),
[updater](../src/eden_uwp/ui/updater.cpp),
[controller layout](../src/eden_uwp/button_layout.h),
[per-title settings](../src/eden_uwp/title_settings.h) and
[save sync](../src/eden_uwp/save_sync/backend.cpp).
Save synchronization is not a persistent whole-emulator snapshot. The reference
checkout is partial, so absence elsewhere is not proof that either app lacks a
feature. Judgment: resumable paired browser import and full session persistence
are potentially useful differences, independent of the renderer bottleneck.
No UI, text, artwork or product branding was copied, and no frontend change is
part of p61. Their UI resource reuse is sensible but has no measured FPS advantage.

## Ranked candidates

Expected benefit is a prioritization hypothesis, not a sum of reference timings.

| Rank | (a) GPU-thread CPU time | (b) TotK/BotW memory / crashes | (c) Game bugs |
|---|---|---|---|
| 1 | **Uniform stream (ported)**: frequent small uniform allocations; fewer profiled ring calls and less alignment waste. | **p60: coordinated headroom, stepped heap growth and retirement**. Avoid growth while resources await fences; retain warm heaps only with margin. | **NoPerspective clip reconstruction**: confirmed reference Series replay and visual fix; reproduce in HLSL. |
| 2 | **Stage metadata (ported)**: remove repeated immutable descriptor vector scans. Smaller likely gain, very contained lifetime. | **Hot-set native PSO residency / bounded prewarm**: potentially hundreds of MiB, but recompile hitches and ownership must be measured. | **Discard/helper derivative preservation**: confirmed reference MRT comparison; adapt to our compiler safely. |
| 3 | **Transfer resource reuse** on remaining texture bounce paths: profile CreateBuffer frequency first; existing heap pooling is not enough to infer a win. | **Transient transfer/staging reuse plus bounded deferred dirty GC**: reduce peaks/waits without stale guest writes. | **BC raw-block sidecar / inactive RTV pruning**: clear correctness invariants, memory and A2C gates required. |
| 4 | Tune existing submission overlap and descriptor fast paths on console. Previous-pipeline reuse is already present; Maxwell/DMA costs need independent work. | Instrument actual process headroom and compiler failures. Hyrule Warriors is a warning, not a TotK crash diagnosis. | Geometry passthrough audit, typed SRV parity and action-correlated Magnesis/terrain captures. The latter two have no proven reference fix. |

## Ports, switches and host validation

Both switches are **default on**, read once per process; an exact `0` opts out:

- `NXBOX_D3D12_UNIFORM_STREAM=0`: restore one ring request per mapped uniform.
  The adapted helper preserves ResourcePtr leases, 256-byte CBV alignment,
  existing zero padding and the existing >4 KiB path. Tick is sampled **after**
  allocation because ring pressure can submit; a changed tick discards the old
  slab. Frame collection and the ring pressure callback clear its optional lease first. No guest content is
  reused; allocation uses the existing ring pressure/wait policy and capacity. Slack is
  bounded by one partial 4 KiB slab at a boundary.
- `NXBOX_D3D12_STAGE_METADATA=0`: restore `CountStageTables` on every graphics
  draw. Native pipelines compute four counts when they copy each immutable stage
  Info (80 bytes/pipeline across five stages). Other pipeline implementations
  retain the scan fallback. Root signature layout, descriptor order, shader ABI,
  guest masks and buffer synchronization are unchanged.

[Host fixture](../tests/port/test_jadyx_ports.py) executes the production uniform
stream and extracts the actual metadata setter/getter/count implementation, with
4000 mixed-size/tick byte-preservation cases and 2000 randomized descriptor-array
layouts. It covers alignment, slab overflow, allocator-induced submission, large
64 KiB uniforms, source Info mutation, empty stages and lease release. Integration
checks verify both exact-zero opt-outs and legacy branches. Builds run with
ASan/UBSan and separately with `clang++ -O2`.

Four optimized Apple host processes measured **14.2763–15.4237 ns/stage** for
vector scans versus **0.803916–0.827417 ns/stage** for cached counts (1,000,000
iterations/process, fixed seeded synthetic layouts). These local microbenchmarks
are not console or whole-draw predictions.
The structural result is deterministic: **100,000 × 256-byte uniforms** need
**100,000→6,250 ring requests** (93.75% fewer), and the existing ring's 512-byte
alignment reserves **51,200,000→25,600,000 bytes**. Real size distributions, final
partial slabs, frame boundaries and ring pressure determine actual savings.

Platform validation still requires Windows UWP build/link, then console D3D12 validation with inline
and deferred recording, allocator pressure/submission rollover, paired same-scene
switch-on/off captures, GPU-thread phase timings with profiling overhead controlled,
process commit peaks and long TotK/BotW sessions. Host simulation cannot certify
driver behavior, native 60 FPS, Mario RPG parity, visual correctness or crash freedom.
Present and texture-budget changes remain with their respective owners.

### Test environment

The existing extraction fixtures instantiate the actual uniform stream and keep
independent binding/state oracles. The mapped-uniform padding assertion checks
the bound slice, rather than the end of its backing slab. Both new switches are
also exercised in fresh processes, including exact-zero opt-out and legacy paths.

Reproduction uses Python >=3.10 with `fontTools`, a native `pkg-config` with fmt
and OpenSSL, and a disposable TMPDIR. On this Mac, prepend `/opt/homebrew/bin`
to PATH, set PKG_CONFIG_PATH to `/opt/homebrew/opt/fmt/lib/pkgconfig` and
`/opt/homebrew/opt/openssl@3/lib/pkgconfig` (colon separated), and expose an existing
fontTools installation through PYTHONPATH. No global packages were installed.
Run `cd tests/port && TMPDIR=<scratch> python3 -m unittest -q` in that environment.

Final full-suite result on 2026-10-21: **1020 tests in 632.243 seconds,
OK (skipped=45)**, using the environment above. The new fixture also passed
separately with ASan/UBSan and optimized builds; the affected existing binding and
rasterizer extraction fixtures passed (8 tests). Formatting and `git diff --check`
pass. Scratch binaries and test logs were removed after recording these results.
