# p80b — Native floor and real-priority 60 Hz frame generation

Local, uncommitted follow-up in `w-p80`, 2026-10-22. No commit, push, network,
deployment, or console access was performed.

## Native eligibility

`src/video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h` measures
real, nonempty guest presentation arrivals using steady-clock wall timestamps.
The rolling window retains about one second and its preceding boundary endpoint.
Generated frames never enter it, and presentation waits are not subtracted.

The gate starts closed. After a full measurement window, native FPS must remain
at least 29 for another second to qualify. Below 28 it closes immediately when
the rolling estimate crosses the threshold. An enabled gate survives the 28–29
dead band; an inactive gate cannot recover there. Pauses over one second, invalid
timestamps, and backwards clocks reset qualification. Boundary comparisons allow
small floating-point rounding error.

All enabled preferences (`2x`, legacy `4x`, `auto`, title presets, and environment
overrides) share the same rule. There is no title-specific exemption.

| Native FPS | Behavior after qualification |
| --- | --- |
| Below 28 | No generated frames |
| 28–29 | Generation may remain enabled; cannot recover from off |
| 29 to below 50 | At most one synthetic candidate per real frame; fill empty output slots |
| 50 or above | Native pass-through, no generation |

This fixes the earlier p80 restriction to native FPS <=30. Mario Kart 8 Deluxe's
reported 34.8 FPS median, as well as 34 and 45 FPS, remains eligible. `Multiplier()`
returning 2 is a candidate limit, **not** an instruction to display every real
frame twice. Generation remains optional when disabled in settings and remains
subject to the existing history, scene-cut, resource-pressure and GPU-budget checks.

## Slot scheduling and priority

`FrameGenerationVblankSchedule` in the same header owns a shared deadline for real
and synthetic output. `renderer_d3d12.cpp` presents real frames first and retains
one synthetic candidate. The next real arrival cancels that candidate. Real
submission waits for an already occupied output slot to expire; an idle slot can
consume the candidate once. Deadlines advance from the actual submission attempt,
so a stall never creates catch-up credit or multiple synthetic frames.

`RendererBase::PresentationDeadline()` / `PresentIdle()` expose an optional idle
hook. Other renderers retain the indefinite queue wait and their existing behavior.
The GPU thread waits until either queued work arrives or the synthetic deadline
expires, using `SPSCQueue::PopWaitUntil`. It checks the command queue again after
a timeout before dispatching idle presentation. Shutdown interrupts the wait.

D3D12 idle work runs on the existing rasterizer consumer, preserving context,
resource, descriptor, and command-queue ownership. An atomic real-ready flag is
published before dispatching a composite to that consumer. Idle presentation
checks it before work, after its backbuffer fence wait, and again after prediction,
before submitting. A ready real frame therefore wins arbitration over a synthetic
candidate. Generation no longer inserts a mandatory synthetic present before each
real present or unconditionally waits for two output slots per guest frame.

At 60/59.94 Hz, both types use DXGI interval 1. At 120/119.88 Hz, both use interval
2 and the same 60/59.94 Hz output-slot period. Supported refresh ranges remain
[59,60] and [119,120] Hz; unknown/unsupported displays pass through. The generation
schedule cannot exceed 60 Hz. Native pass-through at >=50 FPS is not newly capped.

With successful generation and available GPU time, native 30/34/34.8/45 FPS fills
60 Hz slots (59.94 on fractional output). Between 28 and 30 native FPS, the
one-generated-frame-per-real limit permits at most twice native FPS: 28 native
cannot supply 60 distinct frames. Existing rejection checks or missed deadlines
can also leave an output slot repeating its previous image.

Because the real endpoint now wins first, the idle frame must move **forward**
from that endpoint. Replaying the existing 0.5 midpoint after the current real
frame would move time backwards. The idle warp uses fraction
`1 + native_fps / output_hz`; `FrameGenerator::Interpolate` accepts fractions up
to 2 while retaining its active-history, failure, finite-value and query-budget
guards. The shader clamps only its color blend weight, keeping the motion sample
positions extrapolated. Thus unreliable motion falls back to the current image,
without extrapolating colors. Ordinary fractions below one behave as before.
Prediction quality and actual DXGI scanout cadence still require console validation.

The upscaling policy, resolution settings, title preference files, Vulkan pacer,
and Apple/platform account state are unchanged. The earlier explicit float cast
in `GeneratedFrameSchedule::Fraction` remains in this worktree.

## Diagnostics

`D3D12_FRAME_GEN native_fps=... gated=... mode=...x target=60 refresh_hz=...
interval=...` reports the native policy initially and when eligibility changes.
`gated=0 mode=2x` permits at most one candidate, rather than promising literal 2x
output at 34/45 FPS. On/off transitions reset interpolation history.
`D3D12_FRAME_PROFILE` includes the latest `native_fps` and `gated` values.

Correlate eligibility with generated/display counts, GPU budget and measured
presentation gaps. Configuration lines or a 60 FPS target alone do not establish
that every slot received a new frame. No console log is fabricated here.

## Host validation

Host C++ drivers execute the production gate, slot scheduler, interpolation
boundary and timed queue wait. New drivers compile with:

```text
clang++ -std=c++20 -pthread -Wall -Wextra -Wshadow -Wconversion -Werror
```

Coverage includes:

- startup, exact 28/29 thresholds, dead-band retention and sustained recovery;
- native 27/55 FPS producing no generated frames, with 50 FPS also passing through;
- native 30/34/34.8/45 FPS filling all 600 simulated slots, capped at 60, with at
  most one generated frame per real frame and real frames winning coincident slots;
- 59.94/60/119.88/120 Hz, interval 2 at 120 Hz, unsupported displays, bursts,
  single-use candidates, no early output, stalls and invalid clocks;
- generated frames excluded from native measurement;
- forward prediction fractions, resource retention, invalid inputs, query limits,
  missing history, exception handling and failure latching;
- timed queue expiration, an already-ready command at the deadline, publication
  waking the consumer, and shutdown cancelling the wait;
- production integration assertions for real-only observation, idle arbitration,
  all-mode gating, transition logging, and profile fields;
- the existing frozen disabled GPU command sequence, generated-present resource
  ordering and backbuffer restoration.

Final validation (27 tests passed):

```sh
PYTHONPATH=tests/port python3 -m unittest \
  test_frame_generation test_frame_generation_floor test_frame_generation_targets \
  test_frame_pacing test_d3d12_gpu_thread_profile test_d3d12_draw_phases test_gpu_pipeline
git diff --check
```

The aggregate suite completed with `Ran 27 tests ... OK`; the whitespace check
passed. The pipeline replay tests include AddressSanitizer/UndefinedBehaviorSanitizer
coverage. C++/HLSL changed lines were formatted with the repository's clang-format
configuration; touched Python files were formatted with a locally cached Ruff,
without downloading anything.

No MSVC/UWP build is available on this Mac. Strict Clang shadow/conversion checks
passed for the host drivers; they do not certify MSVC C4456/C4457/C4459/C4244,
HLSL compilation, prediction quality, or physical Xbox presentation timing.

## Pending console acceptance (not run)

Use Mario Kart 8 Deluxe (`0100152000022000`) on Series X with a demanding active
race, recording the output resolution, refresh, upscaling setting and scene.
Suggested LocalState `nxbox_env.txt`:

```ini
NXBOX_RENDERER=d3d12
NXBOX_FRAME_GEN=2
NXBOX_D3D12_FRAME_PROFILE=1
NXBOX_SHOW_FPS=1
NXBOX_FRAME_PACING=0
```

Check native 30 and the reported ~34.8 FPS band, plus 45 FPS if available: generated
frames should fill empty slots and observed output must not exceed 60. Below 28,
generation must stop after the rolling estimate crosses the floor; recovery needs
a full window followed by a sustained second >=29. At >=50, including 55, expect
no generation. Check bursts and real-frame priority, motion direction, occlusions,
scene cuts and absence of backwards motion or color overshoot. Repeat at 120 Hz
and with `4x` / `auto`: interval 2 and the single-candidate limit must still apply.

Acceptance evidence still needed: scene/environment, native on/off/recovery lines,
actual generated counts, GPU budget, display gaps, and visual inspection. Host
simulation does not substitute for those measurements.

## p80c — Console refresh-estimate flap (2026-10-22)

Local follow-up to the committed p80b baseline; no commit, push, network or console
access. Evidence is the existing `compat-2026-10-18/r-mk8d-p80` artifact directory.

The 24 native-policy lines in `crash.txt` include 12 transitions at 33–38 FPS.
These are **display eligibility changes**, not lost native qualification:

| crash.txt lines | Native FPS | Estimated refresh Hz | Result |
| --- | --- | --- | --- |
| 1276–1279 | 33.48 / 33.43 / 33.25 / 33.21 | 39.95 / 59.93 / 60.48 / 59.97 | off/on/off/on |
| 1320–1327 | 33.79 / 33.60 / 33.54 / 33.53 | 29.96 / 59.73 / 60.66 / 59.91 | off/on/off/on |
| 1697–1700 | 38.66 / 38.42 / 37.99 / 38.14 | 29.97 / 59.89 / 60.43 / 59.94 | off/on/off/on |

The first group is between heartbeat `tick_ms=113900640` and `113901640`
(UTC 100 ns ticks `134371682028703912` and `134371682038704017`). The last group
is between `113943640` and `113944640`. Individual policy lines have no timestamps;
these surrounding crash heartbeats bound each four-transition burst to one second.
The matching `diag.txt` groups are lines 1322–1325, 1362–1369 and 1740–1743.

A >1 s native gap resets `native_fps` to zero and requires a measurement second
plus a recovery second. It cannot explain these immediate recoveries at 33 FPS.
`gated` is not inverted: it is exactly `effective_frame_generation_mode == 0`.
Generated output calls `RecordPresentation`, never `ObserveNativeFrame`; native
measurement uses wall time and deliberately includes presentation waits.

`FrameGenerator::Configure` overwrote refresh on every adjacent pair of
`SyncRefreshCount` / `SyncQPCTime` statistics. The resulting half-rate, two-thirds
rate and overshoot estimates crossed the strict supported-refresh predicate.
The logs expose the quotient, not the raw DXGI counters; count/QPC sampling skew
is the mechanism reproduced by the host test, not a raw-counter capture claim.

`DisplayRefreshEstimate` now measures counter deltas across two-second windows.
Near 60/120 estimates (within 1 Hz, allowing boundary-counter quantization) use
59.94/119.88 for conservative scheduling. A changed estimate needs two agreeing
windows; a single bad window does not switch modes. A specified swapchain mode
is authoritative. Disjoint/unavailable DXGI statistics clear the estimator and
require a fresh window. Sustained unsupported modes still disable generation.
Native gating, including the 28 floor, 29 recovery and pause reset, is unchanged.

The profile now reports `output_fps` and `output_presents`: successful real plus
synthetic swapchain presents divided by the actual profile window's wall seconds.
Counting precedes the optional DXGI statistics query, so unsupported display-gap
statistics do not suppress this metric. This measures submitted output, not
unique scanouts. Counts reset with the existing profile window/thread ownership.

The regression drives production native gating and refresh estimation for 200 s
at 33 FPS, injecting a single 1.2 s hitch plus count/QPC skew. It asserts exactly
one off/recovery episode, continuous eligibility elsewhere, recovery at 33 FPS,
and output scheduling <=60 at 59.94/60/119.88/120 Hz. The old adjacent estimator
rejects more than 100 samples in each stream. Additional checks cover isolated
bad windows, sustained 30/120/144 Hz changes, resets and invalid timestamps.
All host drivers use shadow/conversion warnings as errors. MSVC/UWP execution
is unavailable on this Mac; no console validation is claimed.

Validation completed: all 29 tests passed in the seven-module host suite listed
above, including the new hitch/refresh and output-profile integration tests.
`git diff --check` passed. Changed C++ lines were formatted with clang-format;
the Python test was formatted with the locally cached Ruff (no download).

## p80d — Candidate versus submitted presentation (2026-10-22)

The committed p80 gate and p80c refresh estimator remain intact. This local change
uses only repository code, host tests and the existing `r-mk8d-p80c/diag.txt`;
no console, network, commit or push was used.

### Evidence and path

The existing artifact contains these profile windows (diag.txt line numbers):

| Line | Frames | Native FPS | Gated | Output FPS | Output presents | Present submits |
| --- | --- | --- | --- | --- | --- | --- |
| 1641 | 183 | 37.60 | 0 | 36.58 | 183 | 183 |
| 1688 | 185 | 35.97 | 0 | 37.00 | 185 | 185 |
| 1739 | 189 | 39.37 | 0 | 37.80 | 189 | 189 |
| 1785 | 193 | 38.62 | 0 | 38.59 | 193 | 193 |

`forced_submits_present` increments for both real and generated submissions.
These windows have exactly one per real frame: they do not show the extra
submission required by `PresentGenerated`. The CPU output counter already counts
both paths; changing its multiplier would fabricate output.

The production path is:

1. `FrameGenerator::Impl::Generate` increments `Tally::generated` after policy
   acceptance and prepares the initial warp. This is a candidate count, not a
   presentation count. The renderer stores its resource in `pending_generated`.
2. Real presentation submits the real backbuffer, calls
   `swapchain->Present(target_interval, 0)`, then publishes the candidate deadline.
3. GPU-thread deadline service invokes `RendererD3D12::PresentIdle`, synchronously
   on the same rasterizer recording consumer used for draws. `Impl::PresentIdle`
   checks candidate eligibility and real readiness, selects
   `GetCurrentBackBufferIndex`, and waits for that buffer's previous submission.
4. `FrameGenerator::Interpolate` records the forward warp and a UAV barrier,
   retains its inputs/output and returns the generated resource. It does not
   present. Failure or a newly ready real frame can still prevent submission.
5. `PresentGenerated` transitions and copies that resource into the selected
   swapchain buffer, transitions it to PRESENT, submits commands, acknowledges the
   queue fence and calls `IDXGISwapChain3::Present(target_interval, 0)` exactly once.
   At 60/59.94 Hz this is interval 1; at 120/119.88 Hz it is interval 2. No interval
   zero, DO_NOT_WAIT or TEST flag is used for this generated presentation.
6. Only after that call does it fetch the next backbuffer index and restore the
   real endpoint into that next buffer. The restore is not another Present and
   does not overwrite the generated buffer just submitted for presentation.

Thus each *submitted* synthetic frame has a distinct swapchain copy, command
submission and positive-interval Present. The code cannot prove a physical scanout
for every accepted candidate, nor promise one for every interpolation dispatch.
DXGI/compositor timing remains a runtime observation.

### Dispatch correction

Previously the GPU thread only serviced a generated deadline after a timed queue
wait returned empty **and** `TryPop` found no command. Continuous SubmitList/Tick
traffic could postpone idle generation indefinitely, until the next real frame
cancelled the candidate. This code-level starvation mechanism is consistent with
the artifact's equal real/output/submit counts; the old logs cannot uniquely
attribute every missed candidate to it.

The loop now also services an expired presentation deadline between commands,
before dequeuing more guest traffic. An arbitrary guest command no longer vetoes
a synthetic slot. A real composite already published through `real_ready` still
wins, including the fence-wait and post-interpolation checks. Commands are never
interrupted halfway through execution. A long command or busy GPU can still miss
a slot; this change does not guarantee 60 FPS. The original timeout race check,
single-use candidate, real arrival cancellation, native floor, output ceiling
and refresh policy remain.

### Profile fields

Each actual renderer swapchain call increments `real_present_calls` or
`generated_present_calls` **before** calling DXGI. Nonblocking pacing retries
therefore count separately; a failing call still counts as an attempted call.
`output_presents`/`output_fps` retain their existing successful-return accounting.

Each successful `GetFrameStatistics` sample also feeds `DisplayStatistics`:

- `dxgi_present_delta`: accumulated `PresentCount` deltas in the profile window;
- `dxgi_present_refresh_delta`: accumulated `PresentRefreshCount` deltas;
- `displayed_fps`: DXGI present delta divided by that window's wall duration;
- `display_stats_samples`: number of advancing sample pairs;
- `display_stats_errors`: unavailable/disjoint/reset observations;
- `display_stats_valid`: at least one advancing pair and no errors in the window.

`displayed_fps=-1` denotes unavailable/incomplete statistics. Duplicate samples
add nothing. Normal UINT counter wrap is accepted; backward resets, impossible
pairs and failed queries discard the baseline. Resize invalidates the baseline.
Normal profile rollover clears totals but keeps the previous endpoint, avoiding
one lost interval every window; ownership changes clear both. Sampling can lag
the profile boundary by a refresh, so short-window submitted/displayed rates need
not match exactly.

DXGI `PresentCount` identifies the last displayed present, and IDs can skip.
Consequently its delta is evidence of display-side progress, **not a count of
individually verified unique scanouts**. Raw present/refresh deltas and validity
are included so `displayed_fps` is never mistaken for that stronger guarantee.
Physical scanout of every generated image still requires runtime evidence.

### Host verification

All 33 tests passed:

```sh
PYTHONPATH=tests/port python3 -m unittest \
  test_frame_generation_presentation test_frame_generation \
  test_frame_generation_floor test_frame_generation_targets test_frame_pacing \
  test_d3d12_gpu_thread_profile test_d3d12_draw_phases test_gpu_pipeline
```

New drivers compile with `-Wall -Wextra -Wshadow -Wconversion -Werror`. They execute
the production busy-queue deadline prelude, call-kind counter and DXGI accumulator;
cover real readiness, candidate cancellation, stale statistics, 60/120 Hz deltas,
profile rollover, unavailable queries, resets and UINT wrap. The production
`PresentGenerated` mock verifies separate buffer content/Present at intervals 1
and 2, followed by restoration of the real endpoint into the next buffer. The
frozen disabled GPU command sequence still passes; only diagnostic calls are
normalized out. Changed C++ ranges use clang-format; Python uses cached Ruff.
MSVC/UWP and physical display timing were not tested on this Mac.
