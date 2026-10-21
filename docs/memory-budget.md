# Tears of the Kingdom under the Xbox UWP memory limit

This change adds attribution and bounds reclaimable renderer work. It is a candidate
mitigation, not a demonstrated fix for the console crash. Guest memory contents,
DRAM mode, mappings, texture formats and shader output are preserved. Nothing was
committed or pushed.

## Evidence and limits of the diagnosis

The supplied October 18 captures were read outside the repository, including
`r-totk`, `r-totk-async`, `r-totk-np` and `r-totk-rc86`. No captures are copied here.
All four are limited to 5,368,709,120 bytes (5120 MiB). Requests for larger app
budgets are refused. In rc86, process usage rises from 3,354,652,672 to
5,362,384,896 bytes between presentation samples 7.372 seconds apart. Its final
heartbeat before termination reaches 5,368,700,928 bytes: only 8192 bytes below
the cap, with pressure level 2 and no recorded device loss.

The rc86 shader preload raises usage from 1182 to 1221 MiB, approximately 39 MiB.
Its last accounted native resource sample is 119,144,448 bytes (113.625 MiB),
**before** the final burst. Async and np peak at 136,904,704 accounted resource
bytes in their emitted samples. The older plain run reaches 1,061,617,664 native
resource bytes and reports allocation failures without device removal. Thus GPU
resources can become significant, but the sparse old snapshots cannot assign
the rc86 burst to textures, guest pages, CPU scratch, or the driver.

The three later runs each log one async decode request near the burst. This is
consistent with an oversized decode as well as subsequent unobserved work; it
does not establish a large queue or prove ASTC caused the crash. The fatal GPU
thread access violation occurs near the process cap. A handler stack alone does
not establish the allocation category or the original failing operation.

## Allocation paths

| Category | Current implementation and expected contribution |
| --- | --- |
| Guest DRAM | `HostMemory::Impl::Init` reserves private backing with `MEM_RESERVE`; the VEH commits touched 64 KiB chunks. Zero clears can decommit pages. With opt-in fastmem, canonical 64 KiB regional sections and aliases are demand mapped. The full 4/6/8 GiB layout is **not** committed at construction. Default 4 GiB mode has a 3285 MiB application pool; the captured guest requests a 2106 MiB heap. Guest pages are expected to be a major baseline consumer. |
| Buffer cache | Placed resources occupy 16 MiB buffer heaps, including free slots; large buffers use committed resources. Heap charge follows the last resource/fence lease, not guest buffer deletion. Existing pressure paths retire fences and trim empty heaps. |
| Texture cache | Native images, original/scaled images, feedback textures and format reinterpretation copies are separate resources. All allocated mip levels, layers, samples and alignment contribute. Views sharing a resource are not charged twice. Existing LRU deletion downloads dirty images, protects decoding/recent images and defers resource destruction until leases retire. |
| Upload/readback | Buffer and texture staging rings retain reusable pages. Their existing pressure collection releases completed, unleased pages; dedicated oversized/deferred staging stays alive through its lease. They are included in the app cap. |
| ASTC/CPU texture work | Async decoding retains unswizzled compressed input and expanded/recompressed output before upload; upload adds a staging copy and the native texture. There was no byte limit on outstanding decode work. Synchronous conversion also retains swizzle/unswizzle scratch capacity. |
| Shader IR | D3D12 translation uses local instruction/block/flow object pools, not a permanent IR cache. They survive translation through HLSL emission, including concurrent compiler jobs, then are destroyed. Pool array capacity is now counted. Nested allocations and compiler internals remain outside that counter. |
| PSO/shader caches | Worker-local DXC caches retain HLSL strings and shared DXIL blobs; native graphics/compute objects and root signatures are cached separately. UWP already disables the pipeline library. PSO driver allocation sizes are opaque: object counts are reported, not invented byte estimates. The small preload delta does not bound runtime compiler/driver growth. |

## Changes and switches

All new behavior defaults on for UWP. Set the indicated environment variable to
`0` before starting the process to disable that part; switches are latched.
Desktop renderer budget behavior is unchanged.

| Switch | Behavior |
| --- | --- |
| `NXBOX_MEMORY_BREAKDOWN` | Emit one `MEMORY_BREAKDOWN` line per existing five-second gameplay diagnostic window, and update allocation gauges. `0` disables the new diagnostic and gauge updates. |
| `NXBOX_ASTC_MEMORY_BUDGET` | Bound async input plus prospective output before allocating either. Use the normal synchronous upload path when the reservation does not fit. `0` restores unbounded async admission and prior compressed-input retention. |
| `NXBOX_TEXTURE_MEMORY_PRESSURE` | Account for the actual OS memory pressure level when deriving the existing texture GC watermarks. `0` retains the previous usage-derived watermarks and existing staging trim policy. |

Async reservation limit is the smaller of 128 MiB and one quarter of the host
headroom remaining after a 512 MiB reserve. Elevated pressure (level 1) lowers the
ceiling to 32 MiB. Level 2 or higher, or less than 512 MiB headroom, admits no new
async jobs. Telemetry failure uses a conservative 32 MiB ceiling. Admission uses
subtraction checks, so oversized jobs and integer overflow cannot bypass it.
Existing jobs retain their reservations until upload or cancelled-job retirement;
returning memory restores admission. This bounds duplicate **queued CPU** storage,
not the live texture working set or the size of one required synchronous upload.

Completed worker input is explicitly released before publishing completion with
the ASTC budget enabled. When pressure selects synchronous refresh, older pending
uploads of the same image are cancelled so they cannot overwrite newer contents.
Other images' jobs and their leases remain intact. No job is silently discarded
without either its normal upload or a newer synchronous refresh/deleted image.

Texture GC still derives its target from the OS-granted limit minus non-resource
usage and the existing 256 MiB reserve, capped at 320/448 MiB expected/critical.
The new pressure policy adds 128 MiB reserve at level 1 and 256 MiB at level 2+.
Arithmetic saturates. For limit=5120 MiB, process=4900 MiB and native resources=
512 MiB, level 2 changes expected/critical watermarks from 320/448 to 156/220 MiB.
The existing GC and staging trim consume those thresholds; dirty readback,
decode protection, LRU ages, iteration limits and fence lifetimes are preserved.
These are soft eviction targets, not resource-allocation rejections. Live guest
and renderer data can still exceed the app cap.

Lossy BC3/BC7 recompression is not enabled by this patch: it would change texture
contents. Existing configured BC1/BC3 paths remain available. RGBA8 remains the
ordinary ASTC fallback when recompression is disabled.

## Reading MEMORY_BREAKDOWN

Every numeric memory field is **bytes**. `level`, `pipeline_objects` and `query_us`
are respectively an OS enum, an object count and a duration. `texture_evictions`
is a lifetime count of images removed by GC.

- `process`, `limit`, `level`: the same OS app accounting used by the heartbeat.
- `guest_reserved`, `guest_committed`: canonical DRAM reservation and a live
  `VirtualQuery` commitment scan. Fastmem aliases are not counted twice. The old
  `HostMemoryCommittedBytes` private-fallback counter is cumulative and does not
  subtract decommits, so the new diagnostic deliberately does not use it.
- `sparse_committed`: live sparse page-table/storage reservations, queried while
  their allocation registry is locked. Unlike cumulative fault counters, freed
  reservations do not remain charged.
- `gpu_buffers`, `gpu_textures`, `gpu_upload`, `gpu_readback`: allocated heap or
  committed-resource sizes. Placed buffers are counted at heap granularity,
  including slack, and never additionally charged at resource granularity.
  Fence-retained and reusable staging resources remain counted until destruction.
- `texture_rgba8`, `texture_bc`, `texture_depth`, `texture_other`: a partition of
  texture allocation bytes by host format/usage, including per-format copies.
  These are subsets of `gpu_textures`, not additional bytes. RGBA8 includes BGRA8;
  depth classification uses the depth/stencil resource flag. ASTC CPU/GPU decode
  outputs use their actual host format in these totals.
- `decode_input`, `decode_output`: retained compressed-input reservation and
  allocated output capacity; input is a conservative upper bound with the ASTC
  switch off, until the corresponding context retires. `decode_reserved` is the
  full input-plus-output admission reservation, including jobs not started yet;
  **do not add it again** to input/output bytes.
- `buffer_scratch`: retained buffer-cache immediate-upload and cached-write scratch capacity.
- `texture_scratch`: retained main upload swizzle/unswizzle capacity, updated on
  uploads. A download can grow scratch before its next upload update.
- `shader_ir`, `shader_source`, `shader_dxil`: pool arrays, retained DXC source
  capacity and shared DXIL capacity. Shared blobs are charged once until their
  final owner releases them, including worker caches and jobs.
- `pipeline_objects`: live native graphics/compute wrapper count, including
  prewarmed objects. Root signatures and driver/compiler internals have no
  portable per-object byte query; `driver_bytes=unknown` says so explicitly.
- `texture_expected`, `texture_critical`: latest applied texture GC watermarks.
  `decode_budget_last` is the budget at the last async admission check (zero until
  the first check; UINT64_MAX with admission disabled). `texture_evictions` counts
  GC removals, not fence-completed releases. These policy fields are not additional
  allocations.
- `unattributed`: saturating process usage minus the main nonoverlapping byte
  categories. Includes JIT caches, allocator retention/metadata, nested IR
  storage, transient compiler buffers, disk-cache metadata, other emulator
  subsystems, swapchain/descriptors and opaque driver allocations. It is not an
  estimate of guest memory or PSO memory. Heap capacities and OS commit are
  different measurements; this is an attribution aid, not an exact balance sheet.
- `query_us`: cost of the live VM scans and snapshot preparation. Ordinary gauges
  use relaxed atomics only when capacities change. VM scans visit contiguous
  regions once per five seconds, not every page or allocation. Samples across
  threads are approximate rather than a stop-the-world snapshot.

## Validation and console follow-up

Host tests compile production admission policy, the refresh admission/fallback
branch, shader pool/gauge lifetime code and the complete production texture GC
function with small OS/resource doubles under address/undefined-behavior
sanitizers. Cases include pressure/recovery, overflow, oversized jobs, queued
input plus output, stale-upload cancellation, non-D3D12 fallback, environment
opt-outs, moved/destroyed pools, dirty-image writeback, protected decoding images
and recent images. Existing resource/heap tests include the new accounting hooks.

The 24 focused memory/resource/ASTC regressions pass. The acceptance command is:

```sh
cd tests/port
TMPDIR=/tmp/nxbox-p60-tests python3 -m unittest -q
```

For this Mac's test shell, the existing Homebrew tools were placed first in
`PATH` (`/opt/homebrew/bin`), and `PYTHONPATH` exposed the already cached fontTools
package at `/Users/miguel/.cache/uv/archive-v0/uIDCMW0xewgmeAP3`. No package was
installed. Without that environment, this shell selects Xcode Python 3.9 and
devkitPro pkg-config: unrelated existing tests fail on missing fontTools,
`itertools.pairwise`, and fmt/OpenSSL discovery. The focused C++ drivers do not
need those Python/font dependencies.

Final acceptance result: **1024 tests in 651.532 seconds, OK (45 skipped)**;
651,265 differential cases reported. Clang-format was applied to changed C++
lines, Ruff formatting/checking passed for the new host regressions, and
`git diff --check` passed. Test scratch files were removed.

Console validation must repeat the same title/update/save and collect diagnostics
through the previous failure point. Compare the default policy against the two
budget switches disabled, keeping the breakdown enabled. Establish which category
actually grows; verify the queue reservation stays bounded, pressure lowers GC
thresholds, completed staging/texture allocations return, and the guest backing
is below its reservation until touched. Check whether `query_us` remains cheap.
Confirm sustained gameplay below 5 GiB, correct textures and no stale uploads,
and measure decode stalls/reloads. Test MK8D, Odyssey, BotW and Smash for regressions.

A full MSVC/UWP build, real D3D12 fence/heap behavior, actual peak savings and TotK
survival are console/build-host work. The old captures cannot establish those
results, and this host change does not claim them.
