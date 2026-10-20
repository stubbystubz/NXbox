# D3D12 GPU pass profiling

Set `NXBOX_D3D12_GPU_PASS_PROFILE=1` in the emulator process environment before
starting Eden. Only the exact value `1` enables it. Restart to change the setting.
On Xbox UWP, add that line to `nxbox_env.txt` in the app's LocalFolder; the
existing runtime environment loader reads it at startup.
It is independent of the existing CPU/frame profiler. To disable it, remove the
variable or set it to `0` and restart.

The disabled path creates no profiler, query heap, readback buffer, callback,
metadata collection, timer reads, or profiling log output. Command entry points
only test a null profiler pointer. In particular, there is no per-draw timestamp
or per-draw environment lookup when disabled.

## Capture and rank

Play a representative scene for at least 10 seconds, then collect `eden.txt`.
At completed frame boundaries at least five seconds apart, the profiler emits up to 15 signatures
ranked by mean GPU milliseconds **per presented frame**, followed by a total:

```text
NXBOX GPU_PASS rank=1 ms=8.2500 kind=draw draws=42.000 rt=1280x720 fmt=28 depth=45 shader_hash=0123456789abcdef calls/frame=1.000
NXBOX GPU_PASS total_ms=19.5000 frame_gpu_ms=21.0000 idle_ms=2.3000 frames=150 dropped_submissions=0 truncated_passes=0
```

Prefixes added by the normal logger are allowed. Analyze one log at a time:

```sh
python3 tools/nxbox/gpu_pass_rank.py eden.txt
python3 tools/nxbox/gpu_pass_rank.py eden.txt --top 30
python3 tools/nxbox/gpu_pass_rank.py eden.txt --json > gpu-passes.json
```

The analyzer weights windows by their logged frame counts. It ignores an
incomplete final window, counts malformed lines, and reports dropped submissions
and truncated passes. Older lines without `frames=` have equal window weights.
Only the top 15 signatures are available in each source window, so aggregate
per-signature values are **lower bounds**; absence from a window contributes zero.
`--top 30` can show signatures appearing in different windows, but cannot recover
unlogged passes. The total line includes all captured signatures, including those
outside the top 15. The analyzer returns status 1 when no complete window exists.

## Interpretation

- `ms`: accumulated signature duration divided by all presented frames in that
  window, including frames where that signature does not occur.
- `kind=draw`: contiguous guest draws, split at each `OMSetRenderTargets`, a
  different work kind, target shape change, or queue submission. Shader changes
  within a pass do not split it; `shader_hash` names its **first** guest pixel
  shader, as a 16-digit hexadecimal hash. A pass crossing submissions produces
  multiple fragments. Equal signatures aggregate across fragments and RT objects.
- `kind=compute`: a contiguous batch of dispatches, including helper operations
  and query reductions. No render target or pixel shader applies (`0` fields).
- `kind=copy`: resource/buffer/texture copies, MSAA resolves, and guest query
  resolves. Texture metadata describes the destination resource's base extent and
  DXGI format; query resolves have zero target fields. Buffer copies have no RT.
- `kind=clear`: native color/depth clears and shader-based masked/integer clears.
- `kind=blit`: image blits/conversions, DrawTexture, and the presentation compositor. Presentation's native
  black clear is counted as `clear`. `shader_hash=0` denotes internal shaders or
  work without a guest pixel shader, rather than an identified guest shader.
- `draws`: actual draw commands per frame in the signature. Indirect dispatches
  use `compute` and contribute no draws.
- `rt`, `fmt`, `depth`: host target width/height, numeric DXGI color format, and
  numeric DXGI depth format (`0` means absent). Guest draws report the first color
  slot; the complete RT set still defines boundaries. Clear and presentation
  paths supply their own attachment metadata. Presentation postprocessing reports
  its output viewport extent.
- `calls/frame`: measured pass/batch occurrences per frame, not draw count.
- `total_ms`: sum of **all** measured pass durations per frame.
- `frame_gpu_ms`: sum of submission timestamp spans per frame. This also includes
  barriers/state/setup outside pass spans, so it can exceed `total_ms`.
- `idle_ms`: positive gaps from one submission's end timestamp to the next
  submission's start timestamp on the same direct queue, divided by frame count.
  It is not CPU fence wait time or wall-frame duration. Tiny query-resolution
  tails and commands outside the measured markers can appear in these gaps;
  timestamps cannot distinguish those tails from queue starvation. Display
  scanout, vblank waiting inside `Present`, and other GPU queues are not timed.

Timing spans include barriers/state between their markers, and timestamp
instrumentation itself perturbs execution. These numbers locate expensive work;
they do not predict Series S scaling directly. Compare repeatable scenes with the
same settings. The profiler disables the optional parallel command-list recorder
while enabled so all markers and measured commands execute in one ordered list;
the ordinary deferred recorder and compact draw packets remain supported.

## Async lifetime and limits

The profiler reuses `Context::AfterCompletion` and the existing submission fence
retirement machinery in both immediate and deferred recording modes. The existing
frame timestamp heap has just one pair per allocator and can wait before reuse;
pass profiling instead owns a bounded heap/readback pool with 32 slots and 4,096
pass spans per submission (about 2 MiB of readback storage). Each slot has separate
submission start/end timestamps. Timestamp frequency is queried once.

Query resolves are encoded before sealing a submission. Mapped bytes are read
only in its completion callback; only after that read is the slot released with
an atomic release/acquire handoff. There is no added submit, fence wait, busy
poll, or GPU flush. If all slots are occupied, the submission is skipped and
`dropped_submissions` increments. Beyond 4,096 spans, remaining work contributes
to submission time but not individual pass time, and `truncated_passes` reports
unrecorded pass starts. Any dropped submission breaks idle continuity, preventing
its busy time from being misreported as idle. Windows with nonzero loss counters
are incomplete and should be recaptured.

Frames are counted by completion markers on presentation submissions, after
those submissions' timestamp reads. Reporting therefore follows GPU completion,
not the producer's CPU frame counter. Windows can arrive late when the GPU or
completion thread is delayed; the final window shorter than five seconds is not
flushed at shutdown.

## Validation

```sh
python3 -m unittest discover -s tests/port
```

`test_d3d12_gpu_pass_profile.py` executes the production profiler against a delayed
GPU double under ASan/UBSan, covering first-shader grouping, compute batching,
frame normalization, GPU timeline gaps, deferred reads, pool exhaustion, slot
reuse, overflow, environment parsing, and the exact log grammar. The disabled
facade test aborts if any profiling hook runs and checks immediate/deferred
command equivalence and allocation counts. Analyzer tests cover weighted windows,
malformed lines, and a truncated final window.

A native Windows/Xbox build and real console capture remain necessary to validate
D3D12 debug-layer behavior and the performance impact on MK8D, Odyssey, and
Bayonetta 2; host tests cannot measure Xbox GPU timings.
