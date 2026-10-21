# D3D12 CPU cost per draw (p47)

The target is the CPU GPU thread, not physical GPU utilization. The supplied
Series X MK8D race capture has about 1019 draws and 33 ms of GPU-thread CPU per
frame (32 us/draw). A 30% reduction requires about 9.9 ms/frame. None of the
estimates below is a console measurement. The changes target roughly
0.7–2.3 ms/frame together, before overlap; **30% is not established or predicted
by this patch**. Reaching that target needs further console attribution, rather
than treating all cache checks as removable.

## Draw path and remaining dependencies

`Draw` normally calls `DrawSynchronous`; the experimental GPU pipeline instead
captures a draw snapshot and queues it. `DrawSynchronous` pauses predication,
prepares parameters, calls `PrepareDraw`, releases converted indices and ticks
pending work. This patch leaves the experimental split disabled.

`PrepareDraw` drains guest memory invalidations, locks the buffer and texture
caches, synchronizes descriptor-table registers, prepares render targets, resolves
the pipeline, and converts unsupported indices. It then gathers shader buffer
bindings and reads guest texture/sampler handles; resolves and prepares images;
prepares render targets again after sampled-alias synchronization; synchronizes
and binds buffers; validates ranges; handles feedback and geometry; builds
descriptor plans; reserves immutable descriptor slots; records transitions, root
arguments and dynamic state; draws; and publishes query/image-write bookkeeping.
Allocation/submission can occur before recording, so table reuse must be checked
again after reservation. Copies, predication and queries remain ordered.

The base branch already avoids much of the obvious repeated work:

- `ProcessDirtyRegistersImpl` compares the register before loading dirty tables.
  `ConsumeSinkImpl` batches shadow processing and unions changed flags. Track,
  Replay and changed-then-restored writes must retain their semantics. The
  measured `shadow_dirty` 0.74 ms and plain registers 0.26 ms bound their potential
  savings; neither accounts for all 6.3 ms of Maxwell methods. No change here.
- Buffer bindings already cache address/range resolutions, immutable resource
  descriptions and uniform copies by content version. `SynchronizeBuffer` has
  upload-range memoization invalidated by write tracking. Guest remaps, uploads,
  written storage buffers and usage tracking still run. No unsafe whole-draw
  binding skip was added to the 2.8 ms buffer-cache phase.
- Texture table-register synchronization has dirty tracking; view IDs have indexed
  lookup; clean render targets use active masks. Descriptor memory reads and
  `PrepareImageView` still detect guest writes, deletions, rescaling and alias
  changes. The second render-target preparation fixes sampled-alias coherency;
  removing it could produce stale bloom/terrain. The 2.2 ms texture-cache phase
  is not all redundant work.
- Attachment formats/extents, graphics state and dynamic state already have
  dependency caches. Pipeline lookup already has a generation-based fast path.
  Their ~0.74/1.3 ms scopes cannot be claimed again as new savings.
- Descriptor writers already reuse CPU sources by exact semantic keys; the bounded
  GPU-table cache compares ordered source handles, layout, recording tick and CPU
  heap generation. Root-binding batches and compact draw packets already exist.
  General draw-state invalidation at query `EndDraw` is a known title workaround
  and remains enabled. Locks protect concurrent guest writes; scratch vectors
  largely retain capacity. Diagnostic formatting is gated, apart from deliberately
  bounded startup reports and infrequent sampler hit reports.

## Changes and expected savings

All switches below are latched once per owning backend/helper; set exactly `0`
to disable. Restart the emulator for each comparison.

| Reduction | What is no longer repeated | Planning estimate from supplied scopes | Opt-out |
| --- | --- | --- | --- |
| Enable existing independent root CBV tracking by default | Root CBV setter/encoding/replay when the same parameter already contains the same GPU address, even after conservative guest draw-state invalidation | 0.3–1.0 ms/frame from the 3.0 ms draw-recording scope; 3676 root binds provide opportunity, not a measured hit rate | `NXBOX_D3D12_ROOT_CBV_REUSE=0` |
| Enable existing transport-state tracking by default | Byte-identical topology, viewport/scissor, vertex/index views, blend and stencil setters before stream snapshots/encoding, despite guest-state invalidation | 0.2–0.7 ms/frame from the same 3.0 ms draw-recording scope; overlaps the root-binding estimate | `NXBOX_D3D12_RECORD_STATE=0` |
| Rehydrate sampler-plan CPU sources | On a semantic sampler-plan memo hit, avoid throwing away immutable CPU handles and repeatedly resolving/hashing sampler keys when another GPU table is needed; restore the bulk-copy and bounded handle-table paths | 0.1–0.4 ms/frame within 3.4 ms descriptor setup; depends on sampler reuse, table changes and recording boundaries | `NXBOX_D3D12_SAMPLER_SOURCE_REUSE=0` |
| Inline disabled frame-profile gates | Out-of-line scope construction/destruction and counter calls that previously entered another function just to discover profiling was off | 0.1–0.2 ms/frame across instrumented phases **only with profiling off**; zero forecast improvement to enabled profile timings | No rendering shortcut or new switch; enable profiling to exercise the original accounting body |

These ranges are hypotheses, not additive guarantees. A host randomized transport
replay emits 185940 calls instead of 326808 in its mixed-mode workload, with
identical effective draw state; that is a command-count result, not an Xbox
speedup. The independent root-cache randomized oracle skips about two thirds of
its synthetic CBV binds. Real games can have much lower reuse.

## Correctness boundaries and host tests

Root CBVs are independent of descriptor heap identity and buffer contents: the
root argument stores an address. Content updates at that same address still run
through upload/coherency tracking. A different signature, parameter overwrite or
new command list invalidates the remembered root value. Before enabling this
cache by default, this patch adds the missing `ExecuteIndirect` invalidation in
both immediate and deferred paths: opaque indirect signatures can alter root
arguments. General guest-state invalidation and query behavior are unchanged.

Transport reuse compares initialized API bytes (including signed zero/NaN bits),
preserves partial vertex updates and standard/independent stencil aliases, and
resets on new recordings and indirect commands. Root CBV and transport caches
remain separate.

Sampler rehydration uses `DescriptorSources::Set`, the same proof as ordinary
writers. Equal full keys can borrow only current-generation source handles.
Changed keys resolve normally. Mid-resolution heap rollover invalidates the
entire source list and falls back to semantic copies. Oversized plans and the
existing descriptor-fast-path opt-out also fall back. GPU-table tick lifetime,
resource retention and reservation rechecks are unchanged. Guest TSC reads and
the original full-entry/comparison/min-max policy checks still run every draw.

The disabled profile gate changes no accounting: enabled counters, nested phase
switches, fence wait timing and owner-thread checks use the original bodies.

Tests execute production helpers/facade code, without game data:

- `test_d3d12_descriptor_binding_reuse`: randomized effective root state versus
  unconditional binds, recording/signature/parameter/indirect invalidation; actual
  immediate/deferred facade indirect reset.
- `test_d3d12_draw_packets`: ASan/UBSan and TSan production facade replay, 72000
  randomized draws, mixed opt-in/out modes, partial IA updates, float bit patterns,
  stencil aliases, indirect resets and zero producer allocations.
- `test_d3d12_draw_scratch`: executes production sampler-source resolution and
  descriptor finishing, both source-reuse switch values; exact expected handles,
  no resolutions on unchanged current-generation slots, independent CPU generation
  and GPU tick changes, mid-loop rollover, oversized plans and fast-path opt-out.
- `test_d3d12_sampler_plan_reuse`: existing differential reconstruction oracle,
  including changed guest entry/compare/layout, aborted draws and min/max policy.
- `test_d3d12_profile_fast_gate`: production inline gates and enabled bodies;
  10000 disabled scopes/counters perform zero owner checks/clocks; enabled nested
  timing, waits, upload counters and non-owner calls retain expected outputs.

## Console measurement

Use the same build configuration, warmed PSOs/shaders, save/race/camera, resolution
and frame interval for MK8D, Odyssey and Wonder. Also check SM3DW terrain and
sampled-alias/bloom scenes because the conservative invalidation remains necessary.
Capture default-on and each individual opt-out, then all three off together.
Keep GPU accuracy, fence batching, present mode and GPU pipeline setting fixed.

1. With `NXBOX_D3D12_FRAME_PROFILE=1`, compare CPU per draw and per frame,
   `draw_recording_cpu_ms`, `descriptor_setup_cpu_ms`, root CBV binds,
   redundant state skips, descriptor copies/copy calls/writes/table reuses,
   barriers, uploads, submissions and query writes. New shortcuts must preserve
   draw counts and guest query writes. Different binding/copy counts are expected;
   different uploaded data or coherency ordering are not. Transport skips occur
   below the rasterizer's redundant-state counter; evaluate their recording CPU
   and packet volume rather than expecting that counter to include them.
2. Repeat the same runs with profiling off. Compare GPU-thread CPU busy time and
   frame rate; the inline disabled gates cannot be evaluated from enabled scopes.
   Record GPU busy time to confirm the bottleneck has not changed.
3. Compare images at matched guest frames for each opt-out and default-on, including
   clears, compute-to-draw transitions, framebuffer changes, indirect draws,
   texture feedback, loading transitions and long runs with heap/ring recycling.
   Inspect device-removal and validation logs. Host effective-state equality does
   not replace console image validation.
4. Report medians and variation across several warmed intervals. Compute
   `1 - new_gpu_thread_cpu_per_draw / old_gpu_thread_cpu_per_draw`; only a measured
   result of at least 0.30 establishes the requested target.

Host acceptance command (scratch is removed afterwards):

```sh
mkdir -p /tmp/p47-tests
cd tests/port
TMPDIR=/tmp/p47-tests python3 -m unittest -q
```

## Host run results

The initial full invocation used the shell's Python 3.9 and reported:

```text
Ran 980 tests in 564.039s
FAILED (failures=1, errors=19, skipped=46)
```

The final full invocation selected the installed Python 3.14 on `PATH`, exposed
existing fmt/OpenSSL `.pc` directories through `PKG_CONFIG_PATH`, and exposed an
already cached FontTools package through `PYTHONPATH`. No packages were installed.
With the same scratch directory and `python3 -m unittest -q`:

```text
Ran 997 tests in 665.775s
FAILED (failures=1, errors=9, skipped=45)
```

The remaining failure is
`test_d3d12_query_gpu_reports.GpuQueryReports.test_reports_keep_snapshots_and_batch_guest_writes`:
the assertion expects `return false;` in the host-managed predicate fallback,
while the existing implementation returns `!Settings::IsGPULevelHigh()`.
The nine errors are subcases of
`test_present_image_selection.PresentImageSelectionTests.test_presenter_fallback_downloads_gpu_owned_alias_before_guest_upload_unless_opted_out`,
whose host fixture aborts. These also failed in the initial run. The extracted
`FramebufferTexture` function was checked byte-for-byte against `HEAD`; it is
unchanged. The failing tests, presenter fixture and query implementation are also
unchanged from `HEAD`. The full suite therefore remains non-green; these failures
were not bypassed or changed to make this patch pass.

The focused command was:

```sh
TMPDIR=/tmp/p47-tests python3 -m unittest -q \
  test_d3d12_descriptor_binding_reuse test_d3d12_profile_fast_gate \
  test_d3d12_draw_scratch test_d3d12_sampler_plan_reuse test_d3d12_draw_packets
```

```text
Ran 9 tests in 22.139s
OK
```

The standalone upload-category accounting test also passed. No console rendering
or performance result is claimed.

## p48

This patch targets the supplied 2.8 ms buffer-bind, 2.2 ms texture-update and
0.84 ms index-flush scopes. These scopes include necessary coherency work. The
planning ranges below are **unmeasured console estimates**, not achieved savings;
the 2 ms/frame goal remains to be tested. No p47 reduction is counted again.

| Change | Skipped work | Expected ms/frame | Default-on opt-out |
| --- | --- | --- | --- |
| Exact buffer usage memo | Rewriting the same usage bitset for up to eight alternating ranges of one D3D12 buffer between usage resets. Unlike the old experimental last-range memo, this is independent of uniform snapshot reuse. | 0.3–1.5 within buffer bind; depends strongly on vertex range sizes and repeated ranges per resource | `NXBOX_D3D12_USAGE_REUSE=0` |
| Small TIC/TSC reads | General nested GPU page walks and device block-reader setup when the descriptor fits within one mapped GPU page and one physical device page. Both graphics and compute use the D3D12 runtime hook; other backends retain their original read path. | 0.1–0.5 within texture update; every descriptor's bytes are still read | `NXBOX_D3D12_DESCRIPTOR_READ=0` |
| Inline invalidation coalescing | Repeated texture cache invalidation for consecutive overlapping or adjacent translated intervals in `FlushCaching`'s batch. No sort, allocation, or unwritten gap is introduced. | 0–0.15 within index flush; zero benefit for already disjoint/coalesced batches | `NXBOX_D3D12_INVALIDATION_COALESCE=0` |

The estimated total is 0.4–2.15 ms/frame, with no guaranteed lower bound. Larger
savings require evidence of more redundancy on console; skipping whole buffer
synchronization or image preparation without write tracking would be unsafe.

The usage memo remembers exact `(offset, size)` operations, rather than inferring
coverage from byte containment. Usage bits only accumulate until
`ResetUsageTracking`, which clears both the bitset and memo. Eviction merely
repeats a scan. Every `MarkUsage` still updates the buffer tick and calls
`Context::Retain`, including when retention starts another recording. Resource
content versions, uploads, storage write tracking, binding resolution and resource
leases are unchanged. The opt-out restores the prior behavior, including the old
last-range memo if `NXBOX_D3D12_BINDING_CACHE` was separately enabled.

Descriptor reads retain table content comparison, refresh/invalidate bits and all
image/sampler resolution and preparation. No descriptor bytes, host pointers or
GPU address translations survive the read. The direct path rechecks the mapping
on every call, drains the GPU pipeline first, and falls back for crossing pages,
reserved/unmapped GPU pages or absent device pointers. The fallback retains the
original split-read/zero-fill behavior. This deliberately avoids assuming that
unchanged table registers imply unchanged TIC/TSC memory. Vulkan/OpenGL do not
implement the optional runtime hook and keep `DescriptorTable::Read`'s default
path. Render-target preparation, including the second alias-coherency pass, is
unchanged.

Invalidations retain the original pending-query report publication **before**
either cache is touched. Texture invalidations preserve the exact address union
and the original lock boundary. Buffer invalidation retains every original range:
its conditional pending-download cancellation must not gain a broader predicate.
Query-cache and shader-cache invalidation also retain the original intervals and
ordering. Empty and overflowing intervals pass through unchanged. The `FlushCaching` call and pipeline drain
remain unconditional; an empty accumulator already has a cheap check.

`test_p48_cache_cpu` compiles production helpers and method bodies with ASan and
UBSan and executes:

- 3,000 draws with alternating/random usage ranges, frame resets, recording-tick
  changes during retention and a null resource. Production usage words, buffer
  ticks and retention counts match unconditional tracking. Default-on performs
  245 scans versus 12,131; the opt-out performs all 12,131.
- 24,000 small reads against the production general `MemoryOperation`/
  `ReadBlockImpl` walker, covering big/small pages, continuous/discontinuous
  device mappings, reserved/unmapped pages, crossing boundaries and zero sizes.
  Four production descriptor tables detect byte writes, remaps, table switches
  and invalidations. The actual D3D12 runtime hook runs with its switch enabled
  and disabled. Missing physical device pages retain zero-fill behavior.
- 5,000 randomized invalidation batches: 160,000 callbacks become 36,041 with
  identical byte coverage. The production texture `WriteMemory` body also yields
  identical per-image flags, write notifications and untrack counts across the
  original/coalesced batches. Opt-out preserves each original interval in order;
  explicit cases cover reverse overlap, adjacency, empty ranges and overflow.

The existing graphics-binding fixture explicitly selects the old usage fallback;
the existing descriptor-table fixture adds the new memory entry point. The image
lookup and sampler-slot fixtures provide a stub runtime with no descriptor hook,
exercising the unchanged shared fallback. Their original assertions remain in place.
The invalidation wiring test recognizes the texture coalescer while preserving
its original buffer, query and shader ordering assertions.

On console, compare defaults, each opt-out separately, and all three off in the
same warmed MK8D race, Odyssey and Wonder scenes. Keep profiling configuration,
GPU accuracy, resolution and pipeline mode fixed. Record the three target scopes,
GPU-thread CPU per draw/frame, draw counts, upload bytes, barriers, submissions,
guest query writes and frame-rate variance. Check matched-frame images through
loading, storage/compute writes, small/8-bit index conversion, CPU/inline
TIC/TSC edits, table/channel changes, sampled aliases/bloom and long runs with
resource recycling. Verify query reports and device-removal/validation logs.
Also repeat with profiling off. Host call reductions do not establish console
image equality or the requested 2 ms/frame saving.

Host validation used the installed Python 3.14, Homebrew fmt/OpenSSL pkg-config
metadata and an existing cached FontTools package. No packages were installed.
The final full invocation was:

```sh
mkdir -p /private/tmp/p48-verified
cd tests/port
TMPDIR=/private/tmp/p48-verified \
PATH=/opt/homebrew/bin:$PATH \
PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig \
PYTHONPATH=/Users/miguel/.cache/uv/archive-v0/uIDCMW0xewgmeAP3 \
python3 -m unittest -q
```

```text
Ran 1000 tests in 551.487s
FAILED (failures=1, skipped=45)
```

The sole failure remains
`test_d3d12_query_gpu_reports.GpuQueryReports.test_reports_keep_snapshots_and_batch_guest_writes`:
its source assertion expects `return false;`, while the existing host-managed
predicate fallback returns `!Settings::IsGPULevelHigh()`. Neither that test nor the
query implementation was changed. An eight-test query/presenter check using
HEAD's original extracted source reproduced exactly this one failure; the
presenter errors recorded under p47 did not recur. There are no new failures.

The focused invocation also passed:

```sh
TMPDIR=/private/tmp/p48-tests python3 -m unittest -q \
  test_d3d12_cached_invalidation test_p48_cache_cpu \
  test_texture_cache_fastpath test_texture_alias_sync \
  test_graphics_binding_cache test_host_memory
```

```text
Ran 20 tests in 12.435s
OK
```

The new Python test passes Ruff checks/formatting, changed C++ regions were run
through clang-format with the repository style, and `git diff --check` is clean.
Scratch directories, generated bytecode and temporary logs were removed. No
console build, rendering comparison or performance measurement was performed.
