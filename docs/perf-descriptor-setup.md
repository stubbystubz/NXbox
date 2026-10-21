# Descriptor setup: patch unchanged semantic keys

`NXBOX_D3D12_DESCRIPTOR_KEY_PATCH` is **on by default**. Set it to `0` and restart
for the unconditional key reconstruction path. Startup prints
`D3D12 descriptor_key_patch=0/1`. This change targets CPU key construction, not
D3D12 API counts. It does not establish a console speedup.

## What executes inside descriptor setup

The supplied MK8D console result is 2.77 ms/frame for the entire scope. There is
no supplied console subdivision of those 2.77 ms; the following is a code audit,
not an attribution of that time to individual functions.

| Work | Actual path and existing avoidance |
| --- | --- |
| CBVs | Root CBVs collect addresses and resource states; table CBVs use `WriteCbv` and `WriteCached`. Null padding still goes through the writers. |
| SRVs | Resolve views, feedback copies and dimensions, construct the SRV description, then `WriteCached`. Resource-state collection still executes on table hits. |
| UAVs | Storage, texel and image writers construct descriptions and validate backing resources before `WriteCached`. |
| Samplers | An existing exact TSC-entry memo skips sampler description translation on hits, but copies cached semantic keys and rehydrates source handles. Misses use `WriteSampler`. |
| CPU source lookup | `WriteCached` compares the previous draw's full key, then `DescriptorSources::Set` borrows a generation-valid handle. Otherwise `ResolveDescriptor` hashes the semantic key and looks up/creates a CPU descriptor. |
| GPU table lookup | `CanReuseDrawTable` first compares the previous source sequence, then hashes the ordered handles for the bounded table cache. The optional semantic table map is separate. Reservation may submit; `FinishDrawTable` rechecks against the new tick. |
| GPU table writes | Table hits do no copy. With valid sources, a miss already uses **one** `CopyDescriptors` with coalesced source ranges. CPU heap rollover falls back to the existing immediate/batched writer. |
| Other work in the scope | Feedback and uniform copies, alias checks, resource states, predicate preparation, reservation and attachment descriptor updates also contribute. |

Adding another whole-table cache or batching `CopyDescriptorsSimple` would
largely duplicate paths already present. Even an unchanged binding still used
to construct a zeroed 80-byte `DescriptorKey`, copy its description and assign
that full key into persistent scratch, before the table reuse check.

## Change

For a planned write, compare the retained scratch key against the current native
resource identity, descriptor kind, byte length and **every description byte**.
Only reconstruct and overwrite the key when one of those differs. This is not
a hash or a guest view-ID shortcut. Different padding/unused bytes inside the
native description remain significant, just as in the original full key.

`DescriptorPlan::Reset` already preserves key storage. Both plans exchanged by
`FinishDrawTable` therefore warm naturally. A hit returning without swapping is
also safe: the current scratch already contains exactly the requested key.
Shrinking a layout destroys removed keys; growing it initializes new keys.
Changing description size still reconstructs the zero-filled tail.

Resource pins are assigned every draw and scratch pins are cleared by the existing
draw cleanup. Source resolution, CPU generations, GPU recording ticks, reservation,
root binding, validation marks, resource transitions and descriptor copies follow
the same paths. No heap handle lifetime is extended. Oversized descriptions are
rejected before comparison. The optimization adds one flag to each persistent
plan and no allocation or cache of resource objects.

## Host reproduction

From the worktree root:

```sh
python3 tests/port/test_d3d12_descriptor_setup.py
python3 tests/port/test_d3d12_descriptor_setup.py --bench
python3 tests/port/test_d3d12_descriptor_setup.py --profile
```

The runner extracts the **current production** `DescriptorPlan`, `MakeDescriptorKey`,
planned `WriteCached`, table lookup and `FinishDrawTable` implementations into a
clang++ fixture, following the existing draw-scratch fixture. These are executed,
not reimplemented as a separate fast algorithm. The device, resource, CPU source
registry and heap allocation are host mocks. `--bench` uses `-O3`; the correctness
run uses AddressSanitizer and UndefinedBehaviorSanitizer, warnings as errors.
Temporary sources and binaries are removed automatically.

The synthetic MK8D-like workload has 2,800 draws/frame, 4–16 SRVs, 1–4 UAVs,
root CBVs, eight-draw material runs, recurring materials, layout changes and
independent recording/CPU-generation changes every 177 draws. It includes actual
scratch resource cleanup after every draw. It is **not a captured MK8D stream**.
Sampler TSC memo translation, CBV address collection, image feedback, native view
construction, resource transitions and the real D3D12 driver are outside this
fixture. Its absolute ns/draw must not be equated with console descriptor setup.

Correctness compares the switch unset, `1` and `0`: each GPU table snapshot is
checked against the requested full keys and resource identities at every draw.
A deterministic digest compares allocation handles, copy API range arguments,
semantic descriptors, reuse decisions and API counts between modes. Additional
writes cover resource-only changes, null bindings, kind changes, shortened
native descriptions, their zero tails, the final description byte and oversized
inputs. Existing port tests separately exercise CPU rollover, table collisions,
ring retirement, sampler memo rehydration and skipped-draw cleanup.

`--bench` alternates mode order across five samples, each 600 frames / 1,680,000
draws. `--profile` adds clocks around planning and table lookup/copy separately;
its timer overhead means it is diagnostic, not the headline benchmark.

## Local measurements

Apple clang 21.0.0, arm64 macOS, `-O3`, five samples per mode with alternating
order. Per-slot correctness assertions are disabled during timing; GPU copy
snapshots remain part of the mock device cost. Timing includes synthetic input
construction and resource pins as well as the extracted production methods.

| Host replay metric | Switch `0` | Switch `1` |
| --- | ---: | ---: |
| Median ns/draw, complete replay | 240.188 | 216.953 |
| Sample range, ns/draw | 231.974–269.044 | 213.593–234.400 |
| Profiled planning median, ns/draw | 147.873 | 131.300 |
| Profiled table lookup/copy median, ns/draw | 80.413 | 80.424 |
| Allocations / bulk-copy calls, each sample | 218,400 / 218,400 | 218,400 / 218,400 |
| CPU source resolutions, each sample | 351,603 | 351,603 |

The uninstrumented median improves by **23.235 ns/draw (9.7%)**. The diagnostic
split locates the reduction in planning; lookup/copy remains approximately flat.
This is a small host improvement and cannot be scaled directly to the Xbox.
Earlier exploratory timings included per-slot correctness assertions and overlap
with the full suite's compilation load; they are not the reported result.

The existing font/icon tests require `fontTools`, absent from the default Python
installation in this workspace. For the full suite, expose the already installed
cached package via `PYTHONPATH` (no dependency added to the project):

```sh
mkdir -p /private/tmp/nxbox-p53-scratch
cd tests/port
PYTHONPATH=/Users/miguel/.cache/uv/archive-v0/JNxAtyjfuxFcSQC1 \
TMPDIR=/private/tmp/nxbox-p53-scratch python3 -m unittest -q
```

The complete port suite finished successfully: **1,012 tests, 45 skipped**, in
527.943 seconds. The focused planner/contract/font tests also passed (15 tests).
Formatting used clang-format on the changed C++ ranges and ruff on Python;
`git diff --check` is clean.

## Console A/B

Keep all existing settings identical and change only:

```text
NXBOX_D3D12_DESCRIPTOR_KEY_PATCH=0
```

versus `1` (or unset), fully restarting each time. Use the same warmed MK8D scene
and compare at least the existing 179-frame window, preferably 0/1/0 runs. Compare
`descriptor_setup` wall time per frame and per draw, total emulator GPU-thread
CPU time and frame time. Descriptor-copy/write/reuse counts should be unchanged;
reduced counts are not the expected mechanism. Check textures, HUD, feedback,
scene transitions and device/debug diagnostics outside the timed window.

Xbox timing, Windows/UWP compilation, driver validation and rendered correctness
remain unverified locally. The console owner must decide from measured wall time
whether to keep this enabled. This is a bounded planner improvement, not a claim
to bridge the full ~25 ms to ~14 ms GPU-thread budget.
