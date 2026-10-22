# p80 — Native floor and real-priority 60 Hz frame generation

Local, uncommitted follow-up in `w-p80`, 2026-10-22. No commit, push, network,
deployment, or console access was performed.

## Native eligibility

`src/video_core/renderer_d3d12/d3d12_present/frame_generation_floor.h` measures
real, nonempty guest presentation arrivals using steady-clock wall timestamps.
The history retains about three seconds and its preceding boundary endpoint.
Generated frames never enter it, and presentation waits are not subtracted.

The gate starts closed. It opens once a complete two-second window has a robust
native rate >=28 FPS. Once open, it closes only when the robust rate over a
complete three-second window falls below 27 FPS. Each robust rate is the reciprocal
of the arithmetic mean of real-to-real intervals whose ending timestamp is in
that window, after removing the shortest and longest 10% of samples (rounded
down). This preserves both modes of alternating short/long vblank intervals,
while rejecting isolated hitches without making a sustained 24 FPS game eligible.
A failing complete three-second window takes precedence over two-second recovery,
even while closed; the two windows cannot issue contradictory consecutive
decisions. The 27–28 band preserves qualification. Boundary comparisons allow
small floating-point rounding error.

The separate `NativeFps()` value remains the one-second wall-time average used by
diagnostics, prediction timing and the >=50 FPS pass-through ceiling. It does not
decide the lower floor. A single hitch up to three seconds does not reset history;
a gap greater than three seconds (loading/suspension), invalid timestamps or
backwards clocks clears history and requires a fresh two-second qualification.

All enabled preferences (`2x`, legacy `4x`, `auto`, title presets, and environment
overrides) share the same rule. There is no title-specific exemption.

| Native FPS | Behavior after qualification |
| --- | --- |
| Three-second trimmed rate below 27 | Close the floor gate |
| Robust rate 27 to below 28 | Retain current qualification |
| Two-second trimmed rate >=28, no failing three-second window, one-second average below 50 | Qualify; at most one synthetic candidate per real frame |
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

- startup, exact 27/28 thresholds, dead-band retention and recovery windows;
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
frames should fill empty slots and observed output must not exceed 60. Generation
must stop when the three-second median native rate falls below 27; recovery needs
a complete two-second window with median rate >=28. Short dips must not close it. At >=50, including 55, expect
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

## p80e — Remove producer double pacing and prepare prediction early (2026-10-22)

Only local source, host tests and the existing `r-mk8d-p80d` artifacts were used.
No console, network, commit or push was performed.

### What the artifacts actually establish

Both `diag.txt` and `crash.txt` contain 88 native-policy messages: the initial
closed gate plus 43 enable/disable cycles. Refresh remains 59.94 Hz during the
race transitions; native estimates crossing below 28 close the gate. Each
recovery resets history again. This is not the p80c refresh-estimator failure.

`diag.txt:1462` reports 273 candidates / 300 Generate calls, with 27 history
rejections. `diag.txt:1785` reports another 285 / 300, with 15 history rejections.
Both have zero timing/difference rejections, `gpu_ms=0.374/0.379` and
`budget_missed=false`. These cumulative 300-call tallies exclude gated real
frames and cannot be treated as five-second per-real acceptance rates.

Between those tally boundaries, five complete real-profile windows (ending at
1549, 1598, 1643, 1692 and 1741) contribute 800 real calls. The partially
intersected windows ending at 1501 and 1793 add at most 156 + 171. Therefore
800–1127 real calls produced exactly 300 Generate calls: at least 62.5% never
became candidates because generation was gated. Across the logged profiles,
579 generated Present calls are recorded. The completed tallies account for
558 accepted candidates, plus an unreported partial tally. There is no evidence
that three quarters of *accepted* candidates are discarded.

The dominant observed suppression is native-floor cycling. The source also
contains a pacing feedback mechanism: the real path sleeps until the prior
submission time plus a full output period, in addition to positive-interval
DXGI pacing. Prediction is then dispatched only at the next period's idle
service, behind next-real GPU traffic, despite an earlier midpoint warp already
being recorded. These waits can delay real arrivals, close the native floor,
and repeatedly invalidate history. The artifacts do not contain old cancel
counters or separate Present durations, so they cannot uniquely quantify each
wait's contribution. Queue depth, double-vblank consumption and late GPU
execution cannot be independently measured from the old aggregate lines.

### Change

Real Present no longer sleeps for a software output period. Interval 1 at
60/59.94 Hz and interval 2 at 120/119.88 Hz retain DXGI's vblank serialization.
The scheduler reserves output periods across both real and synthetic calls;
synthetic submission may start one period before its reserved slot. It carries
reservations forward instead of restarting a full CPU timer at every late real
submission. Idle candidates remain single-use and a newly ready real wins.
DXGI's positive intervals cap scanout; one-slot submission lookahead means CPU
submission timestamps are not themselves required to be a period apart.

Generate accepts the forward fraction `1 + native_fps / output_hz` and records
that warp before the real command-list submission. The idle path presents this
prepared output, avoiding both the unused midpoint warp and a second full-output
warp behind the next guest frame. Copy/Present and restoration ordering remain
unchanged. No interval-zero present, extra output count, worker thread or queue
ownership change was introduced. Native eligibility thresholds and wall-time
measurement remain unchanged; an actual native rate below 28 still disables FG.

### Five-second diagnostics

`D3D12_FRAME_PROFILE` now includes:

- `fg_candidates`, `fg_gated`, and gate subreasons `fg_gated_floor_recovery`,
  `fg_gated_fast`, `fg_gated_refresh`;
- `fg_rejected_timing`, `fg_rejected_history`, `fg_rejected_resources`,
  `fg_rejected_difference`, `fg_rejected_gpu_budget`, `fg_rejected_failure`;
- `fg_cancel_real_arrival`, `fg_cancel_real_fence`, `fg_cancel_real_ready`,
  `fg_cancel_hidden`, and `fg_submitted`;
- `fg_deadline_misses` for service beginning at least a full output period late;
  this is an observation, not an additional terminal cancellation;
- `fg_real_present_ms` for time in the real swapchain path, including gated
  frames, `fg_generated_service_ms` for the generated copy/submit/Present/restore
  service, `fg_fence_ms` for its initial backbuffer wait, and `fg_late_ms` for
  accumulated service lateness.

Each pending candidate is consumed or cancelled once. A swapchain/service
exception consumes it as `fg_rejected_failure` and propagates to the existing
renderer boundary. A candidate crossing a profile boundary can account for a
one-frame difference between that window's accepted and terminal totals.
Counters reset on all existing profile rollovers and recording-owner changes.
The original 300-call rejection tallies remain available for comparison.

### Host validation and limits

The new strict-warning C++ driver executes production idle service, cancellation,
event counting and the scheduler. At 59.94/60/119.88/120 Hz it verifies 300 real
plus 300 generated submissions into 600 distinct simulated output slots, with
nonzero real-return and generated-service costs and busy next-guest recording.
The previous full-period idle deadline fails this test. It also covers real
arrival, real readiness, readiness during a fence wait, hidden-window cancellation,
late service, single terminal accounting, disabled profiling and window reset.
Existing production generation drivers now check rejection classification and
forward-fraction propagation, while the native-floor, 34/34.8/45 FPS slot,
refresh-noise, disabled-path, buffer restoration and pipeline tests remain.

Physical Xbox timing and MSVC/UWP compilation are unavailable on this Mac.
Host warnings include shadow/conversion errors for the new drivers; they do not
certify MSVC C4456/C4457/C4459/C4244. Sustained ~60 displayed FPS and removal of
native-floor cycling require the next authorized console run with these counters.

Validation: all 34 tests passed in the eight-module host suite:

```sh
PYTHONPATH=tests/port python3 -m unittest \
  test_frame_generation_presentation test_frame_generation \
  test_frame_generation_floor test_frame_generation_targets test_frame_pacing \
  test_d3d12_gpu_thread_profile test_d3d12_draw_phases test_gpu_pipeline
```

After extending exception accounting coverage, the five presentation tests were
rerun and passed. The frozen disabled-path hash is unchanged. Changed C++ ranges
were formatted with clang-format and Python with the cached Ruff; no packages
were downloaded. `git diff --check` passed.

## p80f — Robust native floor (2026-10-22)

The owner's p80e report records 58.6–59.4 displayed FPS while open, but 72 policy
transitions in 200 seconds as native delivery fluctuates around 28–31 FPS. Most
windows contain only 17–31 generated presents. This follow-up changes the lower
gate measurement to the 3 s / <27 close and 2 s / >=28 reopen policy described
above. The earlier p80b–p80e sections document historical behavior; their 28/29
thresholds and >1 s pause resets are superseded by this section.

The production gate retains only real timestamps. Medians use real interval
samples rather than generated output or a capacity estimate with waits removed.
The one-second reported rate, >=50 cutoff, supported refresh modes, single
synthetic candidate, output cap, real priority and presentation path are unchanged.
The pause reset now requires a gap >3 s so a lone 1.2 s hitch survives as well.

The strict-warning host regression runs 200 s at jittery 28–31 FPS, with a 26 FPS
half-second dip every seven seconds and a single 0.5 s hitch. At each supported
refresh (59.94/60/119.88/120 Hz), it asserts exactly one transition (startup), no
closures after qualification and identical state/rate with or without synthetic
observations. A sustained switch to 24 FPS closes within 3.1 s; a fresh 24 FPS
stream stays closed throughout 200 s, even when synthetic samples are injected.
Other tests cover exact 27/28 boundaries, incomplete startup windows, dead-band
retention, >3 s suspension reset, invalid clocks and a 1.2 s hitch amid noisy
display statistics (now zero floor outages).

No network, console access, commit or push was used. The p80e measurements above
are from the supplied report; this change does not claim new console evidence.
MSVC/UWP is unavailable on this Mac. Host C++ drivers compile with shadow and
conversion warnings as errors; this does not certify MSVC C4456/C4457/C4459/C4244.

Validation: all 35 tests passed in the eight-module host suite listed in p80e,
including pipeline sanitizer checks and the frozen disabled path. The new C++
driver and changed production header were formatted with clang-format; Python
was formatted with cached Ruff. `git diff --check` passed.


## p80g — Bimodal native cadence and contradictory windows (2026-10-22)

### Root cause

The supplied local `r-mk8d-p80f/crash.txt` contains 571 native-gate state records
(including startup), versus 72 in p80e. The consecutive 29.7 FPS examples retain
`refresh_hz=59.94 interval=1`, ruling out refresh qualification for those changes.
There is one presenter-wide `NativeFrameGenerationGate`, observed once at the
start of each nonempty real `Present`. `PresentGenerated` only records output;
it neither observes native arrivals nor selects the gate. `FrameGenerator::Reset`
clears image history, not this native gate. There is no gate per swapchain buffer.
The speculative profile write using `Multiplier(60)` did not mutate qualification
or drive these state diagnostics; it has nevertheless been removed so the profile
uses the actual selected output policy.

The root cause is within `ObservePresent`. A median of individual intervals is
unstable for bimodal vblank arrivals: ~16.7/50.7 ms intervals deliver 29.7 native
FPS on average, but a median can select either mode when window sample parity
changes. Additionally, the qualified branch closed using three seconds, while
the unqualified branch reopened using two seconds. A three-second failure and a
two-second recovery could remain true simultaneously, so consecutive real calls
alternated the same object's qualification. A long window does not prevent this.
The existing uniformly spaced/jittered tests never exercised this cadence.
Raw real-to-real timestamps are not present in these logs; the bimodal host input
is a deterministic reproducer, not a claimed reconstruction of every console call.

The gate now uses a symmetric 10% trimmed interval mean and gives the long-window
failure priority regardless of current qualification. The floor thresholds,
window lengths, native wall-time reporting, >=50 cutoff and refresh limits remain.
`ObserveNativeFrame` uses the existing `PresentationTime` steady clock, allowing
the host driver to control time without replacing the observation logic.

### Native throughput and pacing comparison

Both local crash logs contain 35 `D3D12_FRAME_PROFILE` records. The table uses
records 12–34 (zero-based), the same 23 full gameplay windows after the introduction,
about 115 s per run. Per-frame fields are sums divided by `real_present_calls`
(3465 in p80e, 2864 in p80f), rather than comparing raw five-second totals with
different frame counts. Window durations are reconstructed as
`output_presents / output_fps`, so derived rates inherit the printed rate rounding.
Actual thread CPU uses `gpu_thread_cpu_pct` times those durations. The individual
`*_cpu_ms` phase counters are **wall-time** buckets (`FrameProfile::Switch`),
including blocking work; their names do not imply exclusive processor execution.

| Measurement | p80e | p80f |
| --- | ---: | ---: |
| Real presents / elapsed seconds | 30.11 FPS | 24.89 FPS |
| Median window real-present rate | 30.59 FPS | 24.99 FPS |
| Median sampled `native_fps` | 29.21 | 24.63 |
| Actual GPU-thread CPU ms / real | 24.544 | 24.436 |
| `maxwell3d_methods_cpu_ms` / real | 6.895 | 14.542 |
| `dma_pusher_cpu_ms` / real | 3.071 | 3.093 |
| `macro_execution_cpu_ms` / real | 0.937 | 0.915 |
| `texture_cache_update_cpu_ms` / real | 2.191 | 2.108 |
| `draw_setup_cpu_ms` / real | 0.685 | 0.693 |
| `descriptor_writes_cpu_ms` / real | 0.528 | 0.568 |
| `pipeline_lookup_cpu_ms` / real | 1.222 | 1.247 |
| `submit_cpu_ms` / real | 0.543 | 0.542 |
| `fence_wait_cpu_ms` / real | 0.464 | 0.574 |
| `fg_real_present_ms` / real | 0.189 | 0.198 |
| `fg_generated_service_ms` / real | 0.249 | 0.604 |
| Generated submissions / real | 0.185 | 0.466 |
| History rejections / real | 0.0098 | 0.0971 |

Native throughput did regress by about 17.4%, but actual CPU per real frame did
not increase. Real Present time remains about 0.2 ms/frame: these logs do not
support a restored double CPU/vblank pacing wait. The p80e-to-p80f diff changes
only the native gate, its tests and documentation; the p80e scheduler and real
Present path are identical. The extra wall time is mainly in the Maxwell bucket.
Every gate transition calls `D3D12::Diagnostic`, which calls
`EdenXbox::Diagnostic` and synchronously `FlushFileBuffers` on the recording
thread. Thus the log storm introduces blocking I/O outside the measured real
Present call, as well as repeated image-history resets. This is consistent with
the larger wall bucket and history rejection rate, but aggregate profiles cannot
isolate the exact time spent logging. No pacing change was made in p80g, and no
new console FPS claim is made.

### Host regression

`test_real_generated_real_policy_at_steady_and_bimodal_29_7_fps` extracts the
production real-present policy selection/log/reset block, `ObserveNativeFrame`
and complete `PresentGenerated` method into GPU/DXGI doubles. It drives real →
generated → real for 200 s at 29.7 FPS, with both uniform and alternating native
intervals, at 59.94/60/119.88/120 Hz and across alternating swapchain buffers.
It checks exactly one startup opening, zero later transitions, no generated
mutation of native history/profile/qualification, and no repeated history resets.
The p80f policy fails this driver after the clock is injected through the same
steady-clock helper. No gate formula is copied into the test.

A separate regression starts with 15 FPS and recovers to 30 FPS. It verifies that
the passing short window cannot reopen against the still-failing long window,
then confirms qualification once both are compatible. Existing sustained-low,
hitch, pause, refresh, cap, real-priority and disabled-path tests remain in use.

Validation: 37 tests passed in the eight-module host suite listed in p80e,
including pipeline sanitizer checks. The new drivers use `-Wall -Wextra -Wshadow
-Wconversion -Werror`; integer differences and sizes are explicitly converted.
C++ changes were formatted with clang-format and Python with cached Ruff;
`git diff --check` passed. MSVC/UWP is not installed on this Mac, so native MSVC
C4456/C4457/C4459/C4244 validation remains unavailable. No network, console,
commit or push was used.
