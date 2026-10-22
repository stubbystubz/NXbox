# p74 — GPU-thread CPU audit and descriptor hot-path reductions

Status: **the 5 ms/image reduction and <16 ms actual CPU target are not demonstrated or achieved by host evidence**.
Two equivalent CPU optimizations are implemented and host-tested. They address a
bounded part of descriptor overhead; this is not a claim to have solved the full
GPU-thread budget. Branch `p74/gpu-thread-cpu`, uncommitted; no commit SHA,
CI/build/package run, push, network, PC or console access. The worktree started clean.
Read PERF-COMMON, COMMON and the complete p73 ledger first. The old jadyx path in
PERF-COMMON is absent on this host, so its ~8 us/draw is a supplied comparison,
not a locally reproduced measurement.

## Before: exact capture normalization

Use `r-mk8d-rc90/diag.txt:4731` (149 images, 138249 draws) and
`r-wonder-all-on/diag.txt:1916` (150 images, 91964 draws), under
`~/.local/share/nxbox/artifacts/compat-2026-10-18/`.
Each phase value is divided by its own window's `frames`; per-draw values are
`phase_ms * 1000 / draws`. Ranks include zero fields and waits. These are
**exclusive QPC wall phases, including off-CPU time**, despite the `_cpu_ms`
name. A per-draw normalization includes dispatch/frontend work and does not
establish the marginal cost of a draw or a given operation.

| Rank MK/W | Phase (`_cpu_ms`) | MK ms/image | MK us/draw | Wonder ms/image | Wonder us/draw |
| --- | --- | ---: | ---: | ---: | ---: |
| 1/2 | `fence_wait` | 4.159 | 4.482 | 3.865 | 6.303 |
| 2/1 | `maxwell3d_methods` | 3.774 | 4.067 | 6.053 | 9.873 |
| 3/3 | `descriptor_setup` | 2.979 | 3.211 | 2.568 | 4.189 |
| 4/5 | `buffer_cache_bind` | 2.469 | 2.661 | 1.860 | 3.033 |
| 5/4 | `dma_pusher` | 2.444 | 2.635 | 2.247 | 3.665 |
| 6/7 | `draw_recording` | 2.145 | 2.312 | 1.664 | 2.714 |
| 7/6 | `texture_cache_update` | 1.994 | 2.149 | 1.758 | 2.867 |
| 8/10 | `pipeline_lookup` | 1.058 | 1.140 | 0.906 | 1.478 |
| 9/9 | `macro_execution` | 0.916 | 0.987 | 1.181 | 1.926 |
| 10/12 | `index_flush_caching` | 0.696 | 0.750 | 0.638 | 1.041 |
| 11/14 | `attachment_setup` | 0.653 | 0.703 | 0.497 | 0.811 |
| 12/15 | `draw_setup` | 0.613 | 0.661 | 0.406 | 0.663 |
| 13/13 | `resource_bindings` | 0.569 | 0.613 | 0.554 | 0.904 |
| 14/16 | `query_state` | 0.551 | 0.594 | 0.397 | 0.648 |
| 15/11 | `submit` | 0.496 | 0.535 | 0.780 | 1.273 |
| 16/17 | `geometry_setup` | 0.478 | 0.515 | 0.375 | 0.612 |
| 17/18 | `draw_cleanup` | 0.470 | 0.507 | 0.363 | 0.592 |
| 18/19 | `descriptor_writes` | 0.466 | 0.503 | 0.284 | 0.463 |
| 19/20 | `barriers` | 0.298 | 0.321 | 0.262 | 0.428 |
| 20/8 | `draw_dispatch` | 0.240 | 0.259 | 1.184 | 1.931 |
| 21/22 | `index_conversion` | 0.224 | 0.241 | 0.192 | 0.314 |
| 22/25 | `graphics_state` | 0.218 | 0.235 | 0.157 | 0.257 |
| 23/21 | `sync_wait` | 0.207 | 0.223 | 0.225 | 0.368 |
| 24/23 | `upload_recording` | 0.180 | 0.194 | 0.163 | 0.265 |
| 25/24 | `feedback_setup` | 0.169 | 0.182 | 0.158 | 0.258 |
| 26/29 | `draw_memory` | 0.166 | 0.179 | 0.105 | 0.172 |
| 27/26 | `buffer_validation` | 0.162 | 0.175 | 0.150 | 0.244 |
| 28/36 | `render_targets` | 0.127 | 0.137 | 0.083 | 0.135 |
| 29/35 | `stage_buffers` | 0.125 | 0.134 | 0.084 | 0.137 |
| 30/31 | `buffer_upload` | 0.117 | 0.127 | 0.094 | 0.153 |
| 31/27 | `upload_allocation` | 0.115 | 0.124 | 0.136 | 0.222 |
| 32/28 | `staging_collect` | 0.088 | 0.095 | 0.114 | 0.186 |
| 33/30 | `descriptor_and_pipeline` | 0.082 | 0.088 | 0.099 | 0.161 |
| 34/39 | `image_views` | 0.074 | 0.080 | 0.055 | 0.090 |
| 35/40 | `geometry_buffers` | 0.064 | 0.069 | 0.043 | 0.071 |
| 36/42 | `graphics_buffers` | 0.062 | 0.067 | 0.043 | 0.070 |
| 37/38 | `upload_transitions` | 0.056 | 0.061 | 0.064 | 0.104 |
| 38/41 | `extent_copies` | 0.053 | 0.057 | 0.043 | 0.071 |
| 39/43 | `graphics_descriptors` | 0.044 | 0.048 | 0.032 | 0.053 |
| 40/44 | `compute_dispatch_setup` | 0.041 | 0.044 | 0.031 | 0.051 |
| 41/33 | `texture_upload` | 0.024 | 0.026 | 0.088 | 0.144 |
| 42/49 | `inline_to_memory` | 0.020 | 0.021 | 0.013 | 0.021 |
| 43/56 | `texture_download` | 0.019 | 0.020 | 0.000 | 0.000 |
| 44/48 | `texture_upload_diagnostic` | 0.015 | 0.016 | 0.017 | 0.027 |
| 45/50 | `uniform_copy_lookup` | 0.013 | 0.014 | 0.010 | 0.016 |
| 46/47 | `buffer_download` | 0.010 | 0.010 | 0.017 | 0.027 |
| 47/32 | `feedback_copies` | 0.008 | 0.009 | 0.090 | 0.147 |
| 48/37 | `between_draws` | 0.007 | 0.008 | 0.075 | 0.123 |
| 49/51 | `buffer_cache` | 0.002 | 0.002 | 0.002 | 0.004 |
| 50/45 | `texture_repack` | 0.001 | 0.001 | 0.029 | 0.047 |
| 51/52 | `pso` | 0.000 | 0.000 | 0.000 | 0.000 |
| 52/53 | `shader_translation` | 0.000 | 0.000 | 0.000 | 0.000 |
| 53/54 | `dxc_compile` | 0.000 | 0.000 | 0.000 | 0.000 |
| 54/55 | `texture_cache` | 0.000 | 0.000 | 0.000 | 0.000 |
| 55/57 | `vertex_copies` | 0.000 | 0.000 | 0.000 | 0.000 |
| 56/58 | `dma_engine` | 0.000 | 0.000 | 0.000 | 0.000 |
| 57/59 | `fermi2d` | 0.000 | 0.000 | 0.000 | 0.000 |
| 58/60 | `index_read` | 0.000 | 0.000 | 0.000 | 0.000 |
| 59/61 | `index_hash` | 0.000 | 0.000 | 0.000 | 0.000 |
| 60/34 | `index_coherence` | 0.000 | 0.000 | 0.085 | 0.138 |
| 61/46 | `index_gpu_modified` | 0.000 | 0.000 | 0.024 | 0.039 |
| 62/62 | `index_restart_scan` | 0.000 | 0.000 | 0.000 | 0.000 |

| Ledger, ms/image | MK8D | Wonder |
| --- | ---: | ---: |
| Sum of all phase fields | 29.962 | 30.363 |
| GPU-thread queue idle, separately | 3.745 | 2.976 |
| Producer wall total | 33.707 | 33.340 |
| Non-fence active phases (wall) | 25.803 | 26.499 |
| Approximate actual producer CPU (`wall * gpu_thread_cpu_pct / 100`) | 21.289 | 21.251 |
| Native GPU busy (overlaps producer) | 9.612 | 9.146 |
| Approximate actual CPU per normalized draw, us | 22.945 | 34.661 |

Do not add `fence_wait_wall_ms` again; it overlaps `fence_wait_cpu_ms`.
The aggregate CPU percentage does not provide CPU attribution per phase. The
~8 us/draw supplied jadyx number is not a controlled same-scene comparison.
MK8D needs >5.289 ms less actual CPU to get below 16.000 ms/image; a 5.000 ms
reduction alone reaches 16.289 ms, above that stricter target. The 16.667 ms
60-Hz budget instead needs 4.622 ms less actual CPU, before other dependencies.

| Count / related phase | MK count/image | MK us/count | Wonder count/image | Wonder us/count |
| --- | ---: | ---: | ---: | ---: |
| `draws` / `draw_recording` | 927.846 | 2.312 | 613.093 | 2.714 |
| `dispatches` / `compute_dispatch_setup` | 24.832 | 1.660 | 5.400 | 5.738 |
| `barriers` / `barriers` | 1228.430 | 0.243 | 766.800 | 0.342 |
| `descriptor_copies` / `descriptor_writes` | 1890.638 | 0.247 | 1364.213 | 0.208 |
| `descriptor_copy_calls` / `descriptor_writes` | 419.886 | 1.111 | 320.200 | 0.886 |
| `descriptor_writes` / `descriptor_writes` | 768.926 | 0.607 | 327.660 | 0.865 |
| `root_cbv_binds` / `draw_recording` | 1682.664 | 1.275 | 1544.607 | 1.077 |
| `ExecuteCommandLists` / `submit` | 16.450 | 30.181 | 25.533 | 30.564 |
| `index_conversions` / `index_conversion` | 33.128 | 6.748 | 8.147 | 23.601 |

The operation column divides the **whole related exclusive phase**, not an
isolated API timer. In particular descriptor writes include RTV/DSV work and
copy work, and draw recording includes much more than root CBVs; those rows
must not be summed or interpreted as individual API latency.

## Selection and implementation

MK8D's largest actionable per-draw renderer scope is descriptor setup, 3.211
us/draw (2.979 ms/image), followed by buffer binding and recording. The base
already has root-CBV reuse, ordered source-handle table reuse, coalesced bulk
copies, last graphics-request caching, buffer binding/range caches and a native
state tracker. Reimplementing them does not supply another gain. Existing
`docs/perf-descriptor-setup.md` and `docs/perf-rasterizer-state.md` describe those
limits. No existing item from PERF-COMMON was redone.

1. **Remove large stack scratch from `FinishDrawTable`.** The old function had
   a 4096-entry `DescriptorCopyRanges` (48 KiB) and an alternate 4096-handle array
   (32 KiB). Optimizers may share their storage, but the hot function still has
   a large prologue even for early table hits. A producer-owned allocator member
   now serves both branches. Reset `count` before each range build; the first
   `Add` always installs the heap domain, so stale `last_heap` is never consulted
   with a zero count. The non-range branch writes the complete prefix used by
   its call. Native `CopyDescriptors` consumes these inputs synchronously; this
   scratch never enters deferred replay and cannot survive into a concurrent
   producer call. Capacity, copy ranges, source order, null handling, API counts,
   GPU slots, reservation and tick revalidation are unchanged. No new allocation
   or lock occurs on a draw. Scratch occupies about 48 KiB per allocator.
2. **Front-cache immutable CPU descriptors before hashing their full keys.** Each
   CPU descriptor domain has a fixed 64-entry direct-mapped cache. A cheap index
   only selects a candidate; a hit requires the complete `DescriptorKey`
   (resource identity, kind, size and every one of 64 description bytes) and the
   owning map's generation. The main map still owns resource pins and creates
   descriptors. The front cache owns numeric keys/handles only. Frame refresh
   clears the transient view map and increments its generation; capacity rollover
   flushes pending copies, clears the map and increments its generation before
   slot reuse. Permanent null and sampler domains remain independent. A full
   permanent null heap retains its original transient fallback. Submission alone
   does not invalidate immutable CPU sources; existing GPU table tick rules still
   apply. A collision is a miss, never an aliased descriptor. Storage adds about
   6 KiB per descriptor-cache instance (five instances, including unused RTV/DSV
   instances). `NXBOX_D3D12_DESCRIPTOR_LOOKUP_CACHE=0` restores map-only lookup;
   unset/`1` enables it, latched in the allocator constructor, printed at startup.

Both changes preserve rendering semantics and are default-on CPU bookkeeping.
The stack change has no runtime switch; compare a baseline binary to isolate it.
No PSO/disk-cache hash, guest synchronization, memory validation, content version,
barrier, descriptor API operation, index conversion or submit policy changes.

Production anchors: `d3d12_descriptor.cpp:470` and `:480` use the persistent
copy scratch; `d3d12_descriptor.h:46` defines exact-key/generation lookup;
`d3d12_descriptor.cpp:560`, `:574` and `:650` wire the domain lookups and store.
The existing invalidation points remain `d3d12_descriptor.cpp:298-300` (frame)
and `:584-587` (capacity rollover).

## Measured host evidence and expected scope

A clang++ C++20 `-O3 -fstack-usage` compile of the existing production-extraction
fixture reported **49,312 -> 144 stack bytes** for `FinishDrawTable`; its cold
error helper remained 16 bytes. The baseline source was reconstructed only in a
temporary generated fixture by replacing the two scratch references with their
original local declarations. This proves host stack reduction, not MSVC codegen
or Xbox timing. On Windows, avoiding per-call stack probing is the intended
mechanism; its cost must be measured, not inferred from the scratch byte count.

`python3 tests/port/test_d3d12_descriptor_lookup_cache.py --bench` runs the current
production resolver with a mock device/main map, `-O3`, 32 warmed recurring SRV
keys, 40-byte descriptions and 2,000,000 resolutions per sample. Five samples per
mode alternate order. It excludes draw planning, actual driver work and generation
churn; this is a favorable lookup microbenchmark, not a captured game trace.

| Host arm64 resolver metric | Map only (`0`) | Front cache (`1`) |
| --- | ---: | ---: |
| Median ns/resolve | 11.0614 | 4.90315 |
| Range ns/resolve | 8.98048–12.3441 | 4.39992–4.92556 |
| Main-map hashes per sample | 2,000,000 | 0 |
| Handle digest, every sample | 3,040,000,000 | 3,040,000,000 |

A host saving of 6.15825 ns/eligible recurring lookup corresponds to
`eligible_lookups_per_image * 0.00000615825` ms/image **on this fixture/host only**.
The captures have no count of source-map lookups or front-cache hit rate. It is
incorrect to substitute descriptor creations or GPU copies for that unknown count.
No Xbox ms/image saving can be predicted quantitatively from these samples.
Budget **0.000 ms/image as established console saving** until A/B evidence arrives.

| Phase | Before MK / Wonder, ms/image | Expected effect, unmeasured |
| --- | ---: | --- |
| `descriptor_setup_cpu_ms` | 2.979 / 2.568 | Less source-key hashing and early-return stack prologue work; only a subset is eligible. |
| `descriptor_writes_cpu_ms` | 0.466 / 0.284 | Less stack/scratch overhead on table misses; identical native creation/copy calls. |
| Other phases, waits, submit, native GPU | See table | No intended direct reduction. Indirect scheduling effects require measurement. |

Even deleting both entire affected MK8D wall scopes would offer only **3.446
ms/image wall**, below the requested 5 ms actual CPU gain. These changes therefore
cannot honestly be presented as the requested full-budget solution. The best next
lead is the remaining serial frontend (`maxwell3d_methods` + `dma_pusher` = 6.219
MK / 8.300 Wonder ms/image wall) plus buffer/texture-cache preparation (4.463 MK /
3.617 Wonder). The base already batches plain methods and memoizes buffer upload
ranges. The aggregate captures do not justify skipping guest memory preparation,
alias synchronization, conditional queries or native completion for another 5 ms.

## Host validation

- Initial required command: `python3 -m unittest discover -s tests/port -p 'test_d3d12*.py'`:
  **326 tests, OK, 3 skipped, 212.838 s**; no pre-existing failure observed.
- New `test_d3d12_descriptor_lookup_cache.py` executes the production resolver
  with/without its front cache: 30,000 randomized keys per mode, eight immediate
  repeat lookups, frame invalidation, capacity rollover, permanent-null overflow,
  independent sampler/view/null domains, exact handle/create-count digest and
  map-owned resource release. Direct forced-collision tests mutate each of 64
  description bytes, resource, kind, size and generation. These compile with
  `-Wshadow -Wconversion -Werror`, ASan/UBSan.
- Its production table-finisher test alternates both copy branches and heap
  domains at 4096/1/127 slots, verifying every emitted descriptor. Existing
  descriptor setup tests compare exact copy-API digests. This older planner
  fixture uses `-Wshadow -Werror` and ASan/UBSan; `-Wconversion` exposes unrelated
  pre-existing fixture/vector-iterator conversions and is omitted for that fixture.
- Existing extraction fixtures were extended with the new scratch/cache members;
  default/unset/zero switch parsing is tested by `test_d3d12_descriptor_ranges`.
- Final required command: `python3 -m unittest discover -s tests/port -p 'test_d3d12*.py'`: **328 tests, OK, 3 skipped, 245.752 s**. The log is `/private/tmp/p74-d3d12-final.log`.
- C++ changed ranges formatted with local clang-format; changed Python formatted
  with cached ruff. `git diff --check` passed. No formatter/dependency download.

## Exact tester run (no console action performed here)

There is **no build/package run id**. Integrate the uncommitted patch through the
owner's build path. At 60-Hz output, compare the same build settings/save/scene/
camera in MK8D (`r-mk8d-rc90` reproduction), then Wonder (`r-wonder-all-on`
reproduction), and Mario Party as a control. Restart fully between each run,
warm up 60 s, capture 60 s. Disable recording and retain other settings unchanged.
Set this exact common environment before launching each game:

```text
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_THREAD_PROFILE=1
NXBOX_FRAME_GEN=0
NXBOX_FRAME_PACING=0
NXBOX_P73_CRITICAL_TRACE=0
```

Remove `NXBOX_RECORD`/`NXBOX_RECORD_DELAY`; keep SYNC_FAST/FENCE_BATCHING/
OVERLAP_SYNCPOINT/VI_INDEPENDENT_VSYNC/PRESENT_FAST and all rendering settings
identical. Do not use SUBMIT_TRACE or synchronous validation diagnostics during
these timing runs. Run this sequence for **each** game:

| Run | Binary | Additional env |
| --- | --- | --- |
| A | Parent `rc/next` baseline | `NXBOX_D3D12_DESCRIPTOR_LOOKUP_CACHE=0` (unknown/ignored by parent) |
| B | Patched | `NXBOX_D3D12_DESCRIPTOR_LOOKUP_CACHE=0` (isolates stack move vs A) |
| C | Patched | `NXBOX_D3D12_DESCRIPTOR_LOOKUP_CACHE=1` (isolates lookup vs B) |
| B2 | Patched | `NXBOX_D3D12_DESCRIPTOR_LOOKUP_CACHE=0` |
| A2 | Parent baseline | Same as A |

Use startup `D3D12 descriptor_lookup_cache=0/1` to confirm the latched patched
setting. Preserve exact environment, diag.txt and rendered captures. For every
steady `D3D12_FRAME_PROFILE` window compute each phase divided by `frames` and
`draws`, then `(sum(all *_cpu_ms) + gpu_queue_idle_wall_ms) / frames *
gpu_thread_cpu_pct / 100` for the approximate actual CPU ledger. Keep enough
windows to report medians and ranges; use thread-profile CPU independently when
available. Do not attribute an improvement in guest interval or fence scheduling
to these CPU caches.

The fields expected to fall are **`descriptor_setup_cpu_ms / frames`** and, for
table misses, **`descriptor_writes_cpu_ms / frames`**; compare per draw as well.
There is no required fall in `descriptor_copies`, `descriptor_copy_calls`,
`descriptor_writes`, `root_cbv_binds`, `barriers` or `ExecuteCommandLists` per
matched scene/draw stream: the operations should stay equivalent. Counts may vary
with frame content; do not mistake that for less cost per operation. The final
acceptance test is >=5 ms/image less actual MK8D producer CPU than matched A/A2,
and <16 ms/image actual CPU for the title target; these are currently **unmet**.
Wonder's own interval-2 requests can retain 30-Hz cadence even after CPU improves;
track `VI_TIMING swap_1/swap_2` separately.

Outside timed runs enable existing validation and inspect HUD/fonts/materials,
feedback/bloom, scene and channel transitions, storage images and conditional
rendering. Compare saved images and device/debug diagnostics. Host proof cannot
establish identical Xbox rendering or compile the full MSVC/UWP renderer; those,
MSVC shadow/narrowing checks and measured real-game gains remain outstanding.
