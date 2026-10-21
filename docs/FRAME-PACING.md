# Frame pacing (p51)

The D3D12 presenter runs on the emulator GPU thread. Real frames currently use
`Present(1, 0)`. The flip-discard swapchain has two buffers, or three with
`NXBOX_D3D12_PRESENT_FAST=1`. It does not set `SetMaximumFrameLatency`, request
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, or wait on a display event.
GPU fence waits protect descriptor/backbuffer reuse independently of DXGI.

## Mechanism

With pacing enabled, a 16-real-arrival moving median selects interval 2 at a
60 Hz output when all samples are 22–40 ms and the window spans at most 6 ms.
Fast games, startup, stalls, empty frames, and unstable producers retain interval 1.
Arrivals are measured at entry to the real presenter, before drawing. Generated
frames never contribute separate arrival samples. Samples following an accepted
paced presentation are excluded from the estimator: the presentation delay must
not convince it that a fast producer is slow. Every 30 paced attempts the window
is cleared and rebuilt without pacing to detect a recovered producer.

The interval-2 attempt uses `DXGI_PRESENT_DO_NOT_WAIT`. If DXGI reports
`DXGI_ERROR_WAS_STILL_DRAWING` or `DXGI_ERROR_INVALID_CALL`, the same real image is
submitted using the original `Present(1, 0)`. Other errors retain normal device
failure handling. There is no sleep, spin, new fence wait, frame drop, extra image
copy, or new queue/thread. Buffer count, swapchain flags and resize flags are
unchanged. A waitable object would require changing creation/resize policy and
waiting on the GPU thread, so it is deliberately not added.

When frame generation is enabled, interval 2 is bypassed for the whole session,
including history warmup, skipped synthesis and failure fallback. Both generated
and real frames retain the existing interval-1 submissions. This avoids holding a
generated image for two refreshes before its real partner.

**Limit:** a nonblocking attempt prevents a new blocking interval-2 call; it
cannot guarantee unchanged GPU-thread throughput. An accepted two-refresh frame
can retain display queue capacity longer and increase a later ordinary present's
backpressure. A finite queue cannot display every frame from a sustained 32 fps
producer on a strict 30 fps cadence without eventually throttling or dropping.
The host queue model demonstrates this with both two and three buffers. Pacing
therefore defaults to **off**; the supplied UI hint describes the intended effect,
not a measured performance guarantee. Do not enable by default without console
throughput measurements. The current implementation is an opt-in DXGI cadence
experiment, not a hard guarantee of equal display gaps or unchanged real fps.

DXGI interval 2 is a minimum residency request, not a deadline scheduler. A late
25 fps producer still misses refreshes, and this feature cannot repair shader,
I/O, or emulation stalls. At refresh rates other than 60 Hz, two refreshes are
not 33.3 ms; use a 60 Hz output for this experiment.

## Switches

- `eden_settings.txt`: `frame_pacing=0|1`, default `0`. Last entry wins; invalid
  values disable pacing. The Graphics screen exposes the toggle alongside Frame
  generation. Changes apply on the next renderer launch.
- `NXBOX_FRAME_PACING=0|1` overrides the persisted preference in either direction.
  Invalid/empty values use the preference. On UWP put the override in
  `nxbox_env.txt`, following other renderer overrides.
- `NXBOX_FRAME_PACING_SETTING` is the launcher's internal preference transport;
  the launcher does not overwrite `NXBOX_FRAME_PACING`.
- `NXBOX_D3D12_PRESENT_FAST=1` retains the existing three-buffer policy.
- `NXBOX_D3D12_FRAME_PROFILE=1` enables the existing profile line.

With pacing off, the only swapchain submission is the original `Present(1, 0)`;
all drawing, transitions, submissions, fence notifications and generated-frame
commands retain their existing ordering. Profiling can query display statistics
in either mode, independently of pacing.

## Console measurement

Use the same save, camera movement and output mode, and compare repeated runs
with pacing off/on, frame generation off/on, and the fast-present flag off/on.
Include a fast game, a stable 30-ish game, a variable-rate scene, and a sustained
32 fps scene. Allow warmup and compare at least a minute per run. Record real fps
separately from generated/presented fps, GPU-thread present/fence wait time,
latency, median and p95 gaps, and the worst per-window gap distribution.

`D3D12_FRAME_PROFILE` now includes:

- `present_gap_samples`, `present_gap_p50_ms`, `present_gap_p95_ms`: display gaps
  inferred from consecutive DXGI `PresentCount` observations and refresh counts,
  using the `SyncQPCTime` clock. These include generated and real displays.
- `present_call_gap_samples`, `present_call_gap_p50_ms`,
  `present_call_gap_p95_ms`: CPU gaps between successful present returns. These
  measure submission behaviour and **must not be labelled display gaps**.

Each field covers the latest 256 valid samples at most and resets when the
profile window is emitted. Repeated statistics are ignored. Disjoint, unsupported,
occluded or skipped-present-count observations break continuity rather than
manufacturing an average sample. Display sample count zero means unavailable,
not perfectly paced. A busy queue, especially with generation, may make polling
too sparse to capture every display. Use external capture/PresentMon-compatible
console tooling for full delivery and worst-gap validation in that case.

Expected stable cadence is about 33.33 ms without generation and 16.67 ms per
present with generation. Reject the experiment if real fps falls or wait time
increases materially; the host tests cannot validate Xbox compositor behaviour.

## Host validation

Run `cd tests/port && TMPDIR=<scratch-directory> python3 -m unittest -q`.
Use Python 3.10+ and the existing host-test dependencies (`fontTools`, `fmt`,
OpenSSL). On macOS, ensure the host Python and `pkg-config` precede the Xcode and
devkitPro tools in `PATH`, with the host libraries visible to `pkg-config`.
The tests execute the production estimator, precedence and submission adapter,
freeze the disabled renderer command path against its pre-feature digest, check
busy/unsupported/error fallback and sample percentiles, preserve generated/real
ordering and model why the default cannot safely be enabled.

DXGI references: [Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present),
[flags](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present),
[frame statistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics).
