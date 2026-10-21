# Bayonetta 2: the apparent 30 fps ceiling

## Finding

There are **two different regimes**, not one proven CPU bottleneck:

1. The guest explicitly requests swap interval **2** for about 38 seconds. That
   section is presentation-limited to approximately 30 fps. The existing
   `NXBOX_FORCE_SWAP_INTERVAL=1` experiment can test this without a new patch.
2. The guest then requests interval **1**, but frame production varies widely
   around/below 30. This is **not a hard 30 fps presentation cap**. No guest CPU
   host thread is saturated, JIT recompilation is small in the median, and the
   existing GPU/query/host-present waits do not explain the whole missing time.
   The supplied logs cannot distinguish guest event/IPC scheduling dependencies
   from time spent executing the guest's frame-critical work.

Do not enable a permanent Bayonetta frame unlock or remove GPU synchronization
based on these captures. The title allowlist in `vi/frame_unlock.h` deliberately
requires console validation of game speed, physics and cutscenes first. This
change adds diagnostics only, with **no default gameplay change**; disable them
with `NXBOX_THREAD_PROFILE=0` (or leave it unset). No new performance fix is
justified by the current evidence.

## Evidence and window selection

Inputs are the four text files in each of:

- `~/.local/share/nxbox/artifacts/compat-2026-10-18/r-bayo2-gdr/`
- `~/.local/share/nxbox/artifacts/compat-2026-10-18/r-bayo2-gdr-prof/`

`summary.txt` reports whole-run live-window medians of **29.7** and **27.2 fps**
respectively. These mix menus, transitions, interval-2 scenes and interval-1
scenes. Profiling also perturbs performance: do not compare the runs as an
optimization A/B or infer a fixed overhead from different scene timings.
`crash.txt` ends with live heartbeats, about 2.8 GiB usage against a 5 GiB limit;
it supplies no per-guest-thread wait breakdown or evidence of a memory-limit
termination.

The following table uses `diag.txt` from **gdr-prof**, where the literal
`THREAD_PROFILE` lines actually exist. Group lines preceding each `GAME_PRESENT`
up to the previous `GAME_PRESENT`; ignore thread samples with `baseline=1` or
`interval_ms=0`. Associate the `VI_TIMING`, `GAME_STALL` and
`D3D12_GUEST_WAITS` in that block. Select:

- Interval 2: `swap_2 > 0 && swap_1 == 0`: **7 windows**, ending at
  `diag.txt:925` through `:1455`.
- Later interval 1: `swap_1 > 0 && swap_2 == 0`, and `tick_ms` more than
  75,000 ms after the first live `GAME_PRESENT`: **24 windows**, `:1630` through
  `:3626`. Exclude the mixed transition at `:1541`.

Numbers are medians across these windows, except explicit ranges. Thread CPU
percent is relative to **one host core**, not the entire Series X. All 41
process-enumeration reports have `coverage=process complete=1`. Different
reporters have slightly different boundaries; dividing their totals by the
block's presented frames is an approximation, not an exclusive frame timeline.

| Measurement | Interval 2 | Later interval 1 |
| --- | ---: | ---: |
| Presented fps median (range) | 29.95 (28.00–30.15) | 25.47 (20.59–37.59) |
| Guest CPUCore_0 CPU % | 29.69 | 48.28 |
| Guest CPUCore_1 CPU % | 15.94 | 30.62 |
| Guest CPUCore_2 CPU % | 15.94 | 22.50 |
| Guest CPUCore_3 CPU % | 0 | 0 |
| GPU emulation thread CPU % | 55.62 | 73.44 |
| GPUFencingThread CPU % | 0 | 0.31 |
| Audio service CPU % | 0 | 0.155 |
| AudioManager / DSP_OpusDecoder_Main CPU % | 0 / 0 | 0 / 0 |
| VI signals per ~5 s | 300 | 300 |
| Largest VI signal gap, median ms | 17.272 | 17.393 |
| VI composition total ms per ~5 s | 8.005 | 5.646 |
| Host `Present` wall ms / presented frame | 0.204 | 0.195 |
| Host fence wall ms / presented frame (all reporting threads) | 1.362 | 1.215 |
| GPU queue idle ms / frame | 12.537 | 4.429 |
| NV event arm-to-signal ms / frame | 0 | 0 |
| Runtime JIT compilation ms / frame | 0.106 | 0.289 |

This scene-based selection intentionally differs from the 38 steady GPU windows
in [BAYO2-sync-wait.md](BAYO2-sync-wait.md). The earlier ~5.7 ms idle median mixes
these regimes. An idle GPU consumer establishes a gap in command availability;
it does not by itself establish a CPU execution bottleneck or exclude a cyclic
guest/GPU dependency.

### Presentation and the scene

`eden.txt` records the same request sequence in both runs:

| Run | Request 1 | Request 2 | Back to request 1 |
| --- | ---: | ---: | ---: |
| gdr | 15.112 s, line 186 | 45.028 s, line 473 | 83.618 s, line 734 |
| gdr-prof | 11.280 s, line 186 | 41.244 s, line 488 | 79.699 s, line 773 |

Each has `requested == effective`; no forced unlock was active. During the
interval-2 plateau, VI still signals/composes at 60 Hz, while only ~150 buffers
are queued/presented per five seconds (`diag.txt:1189–1190`). The source's
`HardwareComposer::ComposeLocked` acquisition check uses
`frames_since_last_acquire < expected_interval`; release is also tied to the
buffer's normalized interval. Thus 60 Hz VI ticks do not imply 60 distinct guest
frames. `TIMING_POLICY use_vsync=2` is the settings enum (Fifo), **not** the guest
swap-interval request. The D3D12 host call is `Present(1, 0)`; the measured call
cost is much smaller than a recurring extra 16.7 ms.

After request 1 returns, rates exceed 30 in multiple windows; for example
`gdr-prof/diag.txt:3362–3363` has `swap_1=162 swap_2=0`, **32.39 fps**, with
CPUCore_0 only 47.81% in that block. The unprofiled late section also ranges
**21.96–38.74 fps**, median **28.15**, over 23 windows more than 75 seconds after
its first live window. The supplied shots are only at 100 and 190 seconds;
neither shows the earlier interval-2 section. The input script also keeps
pressing A and later moves the stick. These are not a controlled stationary
cutscene capture. The logs establish a guest-requested 30 fps section, **not**
whether that exact scene is intrinsically 30 fps on original Switch hardware.

### CPU execution, cache and memory access

The largest later CPUCore_0 sample is **53.44%**; CPUCore_1/2 peak at
41.56/35.94%. Whole-thread averages cannot exclude a serial CPU burst on the
critical path, migration, lock contention, or sleeping on another guest thread.
They do exclude the claim that a named CPUCore thread spends the entire window
executing at host-core capacity.

`GAME_STALL jit=total_ms/max_ms/calls` and `jit_compiled0..3` already measure
runtime compilation; the JIT-specific thread line's `compiles=unavailable`
does not mean the capture has no compilation counters. In the 24 later windows,
compilation has a median 0.289 ms/frame, but a worst window of 5.349 ms/frame:
there can still be compilation hitches. Cache clears, capacity clears,
invalidation calls and disk-stale events are **zero**, with no reported repeat
PC/key compiles. Unknown PC/key buckets are nonzero, so zero reported repeats
is not proof of universal uniqueness. Peak code usage over the whole run is
50,913,101 / 21,807,393 / 18,640,382 bytes for cores 0/1/2, well below their
201,326,592-byte capacities. There is no cache-thrashing diagnosis here.

All four `NXBOX FAULT CONFIG` records show `page_table=true fastmem=false
fastmem_exclusives=false wall_clock=true`. The page-table fallback callbacks
remain frequent (roughly 0.82 / 0.33 / 0.47 million per second for cores 0/1/2
in the late samples). They count calls, not their cost, and do not count every
inline page-table access. Neither their cost nor exclusive-monitor lock/spin
cost is separately timed. No fastmem change or exclusive-monitor bypass is
justified; the independent host-memory/fastmem work must supply its own A/B.

### GPU dependencies and audio

All 24 later windows report zero host/guest syncpoint waits, NV event time,
request-sync-operation, request-flush, wait-idle, TickGPU and puller-acquire
counts. Query-bank/readback forced submissions and query-report fence waits are
also zero. This argues against a recurring **measured** query/readback stall.
Host fence totals include asynchronous work and must not be added to GPU-thread
sync time. `fence_queued_avg_ms` is about 33.28 ms in this selection, whereas
`fence_gpu_execution_avg_ms` is 0.406 ms: queue age is not 33 ms of host blocking
or 33 ms of physical GPU execution. These describe the emulated guest-fence
lifecycle, not a new native GPU duration measurement.

The existing counters do not aggregate the time a guest `WaitSynchronization`
SVC is suspended on a readable event. An nvdrv ioctl can arm an event and
return, then the guest waits separately; guest-written semaphore polling can
also remain inside JIT execution. Audio threads are nearly idle, so there is
no evidence of audio host CPU saturation. Waiting for an audio/event cadence
still needs guest wait attribution.

## Added instrumentation

`NXBOX_THREAD_PROFILE=1` now also produces the following **Eden log** lines
(`eden.txt`, Kernel/Service/Service.VI at Info level):

```text
THREAD_PROFILE guest_thread=<guest id> kind=run key=0x0 calls=... wall_us=... max_us=... interval_ms=...
THREAD_PROFILE guest_thread=<guest id> kind=svc key=0x18 calls=... wall_us=... max_us=... interval_ms=...
THREAD_PROFILE guest_thread=<guest id> kind=sync_object key=0x<readable object> calls=... wall_us=... max_us=... interval_ms=...
THREAD_PROFILE event_object=0x<readable object> name=NVCTRL::NvEvent_...
THREAD_PROFILE event_object=0x<readable object> name=DisplayVsync display_id=...
```

The profile belongs to the **guest KThread**, so an SVC that resumes on another
host core is still charged to its original guest caller. It is allocated only
for a profiled guest thread and released at thread finalization. Each holds a
fixed 129-entry SVC table and 16 selected-object entries; excess objects appear
as `kind=sync_overflow`. There are no new shared counter locks, per-instruction
hooks, or per-call log writes. Disabled profiling performs no new clock reads,
counter allocations or reports. The existing JIT/host profilers remain intact.

- `run`: wall time around ArmInterface RunThread/StepThread, including generated
  guest execution, runtime compilation and callbacks. Excludes SVC dispatch and
  scheduler idle outside that call. It is **not thread CPU time**.
- `svc`: elapsed host wall time until a dispatched SVC returns, including
  suspension and runnable-but-unscheduled time. Relevant hex keys from `svc.h`:
  `0xb` SleepThread, `0x18` WaitSynchronization, `0x1a` ArbitrateLock,
  `0x1c` WaitProcessWideKeyAtomic, `0x21` SendSyncRequest, `0x34` WaitForAddress.
  Key `0x80` also collects out-of-range SVC numbers.
- `sync_object`: a **subset** of SVC time, keyed by the object selected by a
  successful WaitSynchronization. Multi-object waits identify the selected
  winner, not the cause of every delay. Zero means no selected object
  (timeout/cancellation/error). Ready/zero-duration calls are counted too.
  Join object addresses with creation/mapping lines in chronological order;
  slab addresses can be reused. `BufferQueue:WaitEvent`, `NVCTRL::NvEvent_*`
  and `DisplayVsync` separate buffer availability, GPU syncpoint events and VI.

Reports occur after returning execution/SVC calls, at least five seconds apart
per guest thread. Sleeping threads report when they resume, with their actual
interval. Completed long calls can span report boundaries; totals from different
threads overlap. Do not add `sync_object` to `svc`, treat totals as a synchronized
frame breakdown, or expect an indefinitely blocked SVC to report completion.
The existing `NXBOX_WAIT_DIAG=1` flight recorder can show active long-lived SVCs
and service calls if needed; it is deliberately optional because it adds locks
on calls. The standard run below leaves it off.

## Next console experiment

Build/package this worktree, retain the exact executable/PDB pair, and run the
**same Series X, title 01007960049A0000 v0, resolution_setup=3, 100% speed limit,
D3D12 depth reinterpret enabled**. Keep the warmed cache/save and existing
`r-bayo2-gdr-prof/events.txt`. Do not switch fastmem, audio backend, VI mode or
other optimization knobs during this comparison. Capture screenshots at 100
and 190 seconds and all four text logs, including the new Eden lines.

Using the installed local capture harness, after deploying the build:

```bash
capture_root="$HOME/.local/share/nxbox/artifacts/compat-2026-10-18"
bash "$capture_root/_tools/lockrun.sh" \
  bash "$capture_root/_tools/async-session.sh" \
  r-bayo2-cpu-waits 'E:\NXbox\games\Bayonetta 2 [01007960049A0000][v0] (12.41 GB).nsp' \
  205 '100:shot,190:shot' "$capture_root/r-bayo2-gdr-prof/events.txt" \
  'NXBOX_D3D12_FRAME_PROFILE=1\nNXBOX_THREAD_PROFILE=1\nNXBOX_FORCE_SWAP_INTERVAL=0\nNXBOX_VI_INDEPENDENT_VSYNC=0\nNXBOX_WAIT_DIAG=0\nNXBOX_SAMPLER=0'
```

Repeat as **r-bayo2-cpu-unlock**, changing **only**
`NXBOX_FORCE_SWAP_INTERVAL=0` to `1` and the output slug. This existing opt-in
only turns requested ordinary intervals 2–4 into 1, preserving the guest request
in `VI_TIMING`. Inspect `NXBOX_SWAP_INTERVAL requested=2 effective=1` to verify
activation. Compare the **same visible interval-2 scene**, not whole-run medians:
if it goes above 30 with correct game speed/animation/audio, presentation was
limiting that section. A change in script progress alone is not validation.
The interval-1 section should not be unlocked by this knob; analyze it separately.

For the later interval-1 section, rank per-guest-thread `run` and SVC totals
using each thread's own interval, then inspect selected objects:

- Dominant waits on DisplayVsync/BufferQueue with spare CPU: guest cadence/buffer
  scheduling. Dominant NVCTRL events: guest dependency on GPU completion;
  correlate guest-fence publication/queue age rather than deleting the waits.
- Dominant SleepThread: game timer/cadence. Dominant mutex/condition/address or
  SendSyncRequest: guest scheduling/IPC dependency. If IPC dominates, repeat
  that scene with `NXBOX_WAIT_DIAG=1` to identify the service call in flight.
- Dominant `run` on the frame-critical guest thread with low compilation:
  generated-code/callback/guest-polling cost. Only then repeat the same scene
  with `NXBOX_SAMPLER=1`, retaining the matching PDB. The existing sampler also
  visits CPUCore_0..3; symbolize its module+RVA samples to distinguish monitor
  lock helpers, memory callbacks and generated JIT bodies. Stack scanning is
  statistical and cannot turn anonymous JIT PCs into per-instruction timings.

Also repeat the exact interval-2 scene on original Switch, or obtain a matching
reference capture, before calling it a cutscene-only native 30 fps limit. The
current logs cannot establish that claim. No console run was performed here.

## Validation

`test_guest_thread_profile.py` executes the production accumulator under
ASan/UBSan: elapsed run/SVC accounting, guest ownership, five-second gating,
actual long intervals, selected-object grouping, overflow, reset, malformed/off
knobs, reversed clocks and report exceptions. Wiring checks cover successful
wait identity, event labels and profile cleanup. The focused tests pass.

Final command: `python3 -m unittest discover -s tests/port` completed **970 tests
in 565.532 seconds: 924 passed, 44 skipped, no assertion failures, only the two
known `fontTools` import errors** (`test_button_layout`,
`test_xbox_button_icons`). The new two-test profile harness and existing three
frame-unlock tests also pass independently. Ruff and `git diff --check` pass.
Host tests do not compile the Xbox SDK target or measure console performance.
No commits were created.
