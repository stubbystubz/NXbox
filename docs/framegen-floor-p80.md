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
