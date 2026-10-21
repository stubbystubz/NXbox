# Graphics pipeline lookup and deferred draw serialization

The starting p54 tree already contained a dirty-generation last-pipeline shortcut,
CityHash64 for in-memory graphics key tables, and a bytewise key comparison. This
change retains and exercises those paths rather than counting them as a new gain.
No console timing or frame-rate improvement is claimed from host results.

## Pipeline path

`CurrentGraphicsPipeline` first synchronizes shader invalidation and refreshes the
stage hashes. It compares `GraphicsStateInputs`: the register tracker generation,
stage hashes, attachment formats and sample count, draw topology, depth attachment
enable, indexed mode, and tessellation enables. With matching inputs and a resolved
last pipeline, it returns that pointer before constructing, copying, comparing or
hashing a graphics key. Profiling still counts prewarmed hits. Verify mode bypasses
the shortcut. Pending/unsupported requests cannot install a reusable pointer.
Channel switches, guest-cache mutations and disk reload invalidate the pointer.

The tracker is conservative: dynamic viewport/scissor or blend-color changes can
advance its generation even if the final PSO key stays unchanged. This patch does
not narrow that shared dirty flag, consume shader flags early, or skip shader
refresh. A console workload dominated by those writes can still miss this path.

The remaining key path now avoids zeroing the 704-byte request before fully
assigning it. `FillGraphicsState` initializes all state bytes; key construction
assigns all six shader hashes. Repeated unresolved requests can read the cached
key in place instead of copying it. Register-derived state remains a separate
working copy, because vertex-read lowering modifies it after request lookup.
Verification always builds a separate reference request.

Hash tables capture their immutable hash-mode switches at construction, avoiding
switch accessor calls for each bucket lookup/rehash. CityHash affects only the
in-memory bucket hash; persistent recipe/request hashes and specialization hash
bytes remain unchanged. Full byte equality resolves collisions. The unique object
representation assertion continues to guard byte hashing/comparison.

Default-on switches (set before launching the emulator):

- `NXBOX_D3D12_PIPELINE_LOOKUP_FASTPATH=0`: bypass the generation-based pointer
  return; retain the existing cached-key/equality fallback.
- `NXBOX_D3D12_PIPELINE_KEY_FASTPATH=0`: restore cleared/copied request storage
  and the bytewise FNV table hash. This hash bisection is deliberately broader
  than the incremental storage change; CityHash was already enabled in p54.
- Existing `NXBOX_D3D12_STATE_FASTPATH=0` disables input-cache reuse and CityHash;
  `NXBOX_D3D12_STATE_VERIFY=1` rebuilds the reference key.

## Deferred serialization

`NXBOX_D3D12_RECORD_PACKET_RESERVE=0` restores the multiple-`Write` encoder.
By default, a command header, fixed arguments, padding and snapshot arrays reserve
one contiguous unpublished packet. Arrays copy directly into their final offsets.
The first packet in a compact draw batch uses the same reservation; later draws
still append to that batch. Replay callbacks, ordering, payload layout, descriptor
snapshots, COM holds/releases and the actual D3D12 calls are unchanged.

Reservation checks routing first, acquires a free chunk before reusing its storage,
and never publishes bytes while they are being filled. Exact-boundary, oversized,
split and backpressured packets use the existing writer. A normal command ends a
compact batch, including the reservation path. Replay still pins published chunks
until execution and release finish. Neither producer path allocates per command.

## Host fixture and reproduction

Use host Python 3.10+ with the existing suite's `fontTools` dependency available,
and host `pkg-config` resolving `fmt` and OpenSSL. On this Mac, entering
`tests/port` selects Xcode Python 3.9 and devkitPro's pkg-config through the shell
environment; validation prepended `/opt/homebrew/bin` to `PATH` and used an
already cached fontTools installation through `PYTHONPATH`. No project dependency
was added.

```sh
cd tests/port
scratch=$(mktemp -d /tmp/p54-port.XXXXXX)
TMPDIR="$scratch" python3 -m unittest -q test_d3d12_draw_cost
TMPDIR="$scratch" python3 -m unittest -q
rmdir "$scratch"
```

The fixture compiles with host `clang++ -std=c++20 -O3`. It uses 3,580 draws per
frame, 10 warm-up frames, 120 measured frames and median nanoseconds per draw.
Pipeline workloads contain 73 materials, eight active attributes, color/depth
attachments, and runs of either one or eight draws per input generation.
It executes the production generation-hit branch, request-storage declarations,
input comparison, key normalization/hash/equality, and pointer installation.
Unavailable Maxwell register conversion, shader refresh, device queries and
compilation are fixtures; this is not a benchmark of the complete Xbox lookup.

Lookup modes: `0` disables the lookup and key switches; `1` enables only the key
switch; `2` enables both; `3` uses the starting-tree request storage with the same
generation shortcut and current hasher. Mode 3 isolates the storage comparison;
it is not a separately built historical emulator binary. Every mode must select
identical key bytes and pipeline IDs at every draw. Hash timings separately
measure the production FNV/CityHash functor over a full 704-byte key.

The recording fixture uses the production stream/argument encoder and root-binding
helper: eight root bindings, 16 root constants, a pipeline/topology packet and a
compact indexed draw per draw. Producer timing excludes replay; each frame is
replayed before the next is encoded. This isolates serialization without a native
D3D12 device or concurrent consumer contention. It is synthetic MK8D-like traffic,
not a captured console command stream.

The ASan/UBSan facade fixture runs in separate processes with reservation on/off.
It compares the exact native-call sequence and argument/descriptor contents against
a raw fake list, including null arrays, unions, mutable descriptor snapshots,
COM ownership, failures, shutdown, tiny chunks and backpressure. Existing threaded
recorder fixtures additionally exercise routing, concurrent reuse and replay.

## Measurements and remaining validation

Validation: the complete `python3 -m unittest -q` port suite passed with 1,013
tests and 45 skips in the host environment described above. This includes the new
benchmark/parity fixtures, ASan/UBSan checks, and existing TSan transport/routing
tests. `git diff --check` and formatting checks also passed. Temporary binaries
and scratch files were removed.

Full-suite rerun on this Mac (nanoseconds per draw except the
hash row):

| Workload | Before / disabled | After / enabled |
| --- | ---: | ---: |
| Request storage, changing inputs every draw (mode 3 vs 2) | 130.73 | 124.05 |
| Request storage, eight draws per generation (mode 3 vs 2) | 20.91 | 19.86 |
| Lookup + key switches, changing every draw (mode 0 vs 2) | 842.10 | 124.05 |
| Lookup + key switches, eight draws per generation (mode 0 vs 2) | 154.30 | 19.86 |
| Full-key FNV vs CityHash (ns/key, already available in starting tree) | 724.70 | 29.08 |
| Producer recording, reservation off vs on | 84.79 | 16.24 |

Machine load varied. Earlier targeted runs measured storage at 174.38 -> 150.02
and 240.91 -> 172.17 (changing every draw), 26.22 -> 23.53 and 23.34 -> 23.30
(eight draws per generation), and recording at 103.98 -> 17.67 and 113.05 -> 29.73
ns/draw. Recording consistently improved in these fixtures;
the incremental pipeline-storage gain is not established under this noise.
The large combined-switch difference includes existing CityHash and pointer reuse
and must not be credited to the new storage change. No host number can be
subtracted directly from the console's 1.06 ms / 2.09 ms phase measurements.

Console validation remains with Miguel: same MK8D scene and profile window with
each switch toggled separately, measuring `pipeline_lookup`, `draw_recording` and
GPU-thread frame wall time. A Windows/UWP build, real D3D12 driver output, console
cache-hit frequency, contention and the 14 ms CPU target remain unverified here.
