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
