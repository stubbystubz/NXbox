# Experimental temporal upscaling and frame generation

## Integration status

The owned presenter files contain the implementation and host tests. **The UI,
startup environment bridge and 4x presentation loop are not connected yet.** Their
call sites are outside the explicitly assigned file ownership. The reviewable,
unapplied `tests/port/p59_integration.patch` contains the minimal changes to:

- `src/video_core/renderer_d3d12/renderer_d3d12.cpp`
- `src/eden_uwp/mesa_window.cpp`
- `src/eden_uwp/ui/library_screen.cpp`

Run `git apply --check tests/port/p59_integration.patch` to check applicability.
Do not mistake the schedule/API tests for an active 120 Hz console integration.
No commits, binaries or SDK redistributables are included.

## Switches

Frame generation and temporal accumulation are opt-in. The existing FSR 1 spatial
default is preserved; temporal history, motion analysis and extra dispatches are
not allocated or recorded with temporal disabled.

| Control | Values | Default / precedence |
| --- | --- | --- |
| Frame generation preference | off, 2x, 4x | Off |
| Persisted frame generation | `frame_generation=0`, `1`/`2`, `4` | Legacy `1` means 2x; reader also accepts `frame_gen=off\|2x\|4x` |
| `NXBOX_FRAME_GEN` | `0`, `1`, `2`, `4` | Overrides preference; `1` means 2x; invalid text preserves preference |
| Upscaler preference | `upscaler=bilinear\|fsr1\|temporal` | Existing `fsr1` default |
| `NXBOX_TEMPORAL` | `0`, `1` | Overrides temporal preference; `0` is the temporal opt-out |
| `NXBOX_D3D12_FSR` | `0`, `1` | Existing master spatial presenter switch; `0` also bypasses temporal |

After the integration patch is approved/applied, the environment loader exports
`NXBOX_FRAME_GEN_SETTING=0|2|4` and `NXBOX_UPSCALER_SETTING=...` before loading
`nxbox_env.txt`. The menu cycles Off/2x/4x and Bilinear/FSR 1/Temporal
(experimental), with PT-BR translations including “Desligada”. Restart the title
when changing these preferences.

For a console A/B, change one line in `nxbox_env.txt`:

```ini
NXBOX_FRAME_GEN=0
```

or independently:

```ini
NXBOX_TEMPORAL=0
```

**Without that patch:** `NXBOX_FRAME_GEN=4` still reaches the old single-midpoint
caller. It must not be advertised as 4x. `NXBOX_TEMPORAL=1` can exercise the
presenter through the existing FSR path, but selecting `upscaler=temporal` alone
is not yet forwarded to it. The old UI still displays boolean choices.

## Frame generation

`GeneratedFrameSchedule` specifies one midpoint for 2x and quarter/half/three-quarter
fractions for 4x. For real images A and B, the intended display order is:

| Mode | Generated timestamps | Display order |
| --- | --- | --- |
| 2x | A + (B − A) × 0.5 | I(0.5), B |
| 4x | A + (B − A) × 0.25, 0.5, 0.75 | I(0.25), I(0.5), I(0.75), B |

This retains one real-frame interval of history latency. Fractions are virtual
image timestamps, not guest clock advancement. Each generated and real image
uses `Present(1, 0)`; there is no sleep, spin or interval-zero substitution.
The cadence probe still periodically presents a real-only frame to avoid
mistaking presentation backpressure for a slow producer.

`Configure` queries the swap chain refresh rational and containing output. When
flip composition reports an unspecified rational, completed DXGI refresh/QPC
statistics provide the measured refresh rate. It logs the requested/effective
mode, refresh Hz, whether the output was found and whether refresh is unknown.
4x requires at least 119 Hz (including nominal 119.88 Hz); 60 Hz and unknown
refresh conservatively select 2x. It does not change the Xbox display mode.
Unavailable/disjoint statistics require console verification.

Luma and bidirectional 8x8-cell motion are evaluated once for a real pair.
Additional frames run only the warp/blend pass with a different interpolation
constant. Each pass restores SRV/UAV states and retains its images through its
own submission. Descriptor heaps are not overwritten until their last tick
completes. The real image is preserved before any swap chain copy and restored
after each generated present.

The existing cadence, missing-history and scene-difference rejection remains.
Additional asynchronous GPU timestamp pairs measure analysis and each warp
separately, excluding idle time between presents. A generation cost above
8.33 ms for 4x, or 16.67 ms for 2x, conservatively latches generation off for that
renderer session. These are 30 fps budgets; slower producers receive the same
conservative limits. No fence wait is introduced to obtain statistics. A GPU
miss logs `mode=off rejected_timing=gpu_budget`; subsequent rejected real frames
continue the 300-frame diagnostics.

The `D3D12_FRAME_GEN` summary reports effective mode, generated outputs scheduled
per 300 real calls, timing/history/difference rejections, difference statistics,
latest GPU ms and budget latch. A 4x window can schedule up to 900 outputs. An
exception during a later interpolation can terminate the sequence early, so the
scheduled tally is not a count of successful physical scanouts. Actual display
rate must be checked through DXGI presentation statistics/the overlay.

## Temporal algorithm

This is a compact jitter-free TAAU, **not AMD FSR 3.1**. No FSR 3 shader source was
vendored. It uses the already vendored MIT FSR 1 EASU/RCAS kernels and the project's
own optical-flow/accumulation code. AMD's [FSR upscaler documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/)
requires depth, application motion and jittered render-resolution inputs. Those
signals cannot be reconstructed faithfully from the final Switch compositor
image, so fake depth/jitter was deliberately avoided.

1. Normalize the native render-resolution compositor image into a float RGBA
   scratch texture. No guest motion vectors, depth or title-specific data enter.
2. Reuse `FrameGenerator::Analyze`: four-tap luma cells, then the existing
   bidirectional block search. Analysis-only mode dispatches no generated warp.
3. EASU provides the current spatial reconstruction at display resolution. Thus
   rejected temporal history retains the existing spatial reconstruction.
4. Bilinearly upsample flow for each output pixel. Dilate from a 3x3 cell
   neighborhood using photometric cost, distance and bidirectional consistency,
   instead of blindly spreading the largest vector across a silhouette.
5. Fetch accumulated display-resolution history using 16-tap Catmull-Rom.
   Reproject current-to-previous motion with the **positive** backward displacement
   (the intermediate warp uses a different coordinate convention).
6. Clamp history to the current native 3x3 neighborhood in YCoCg. Luma changes
   produce a reactive weight; forward/backward disagreement and out-of-bounds
   reprojection reject disocclusions. Maximum history weight is 0.8. Preserve
   current alpha, never accumulate overlay alpha.
7. Store unsharpened float RGBA history, then run the existing final RCAS pass.
   Sharpening never feeds back into history.

Only a single opaque, unrotated, uncropped compositor layer is eligible.
Multi-layer, transformed/cropped, non-upscaled and unsupported layouts fall back
to the original spatial pipeline. This restriction prevents unrelated overlay
history and allows the same motion field to feed final-compositor frame
generation. The integration patch supplies layer count and shares that field
when its native grid matches the generation history extent. A mismatched grid
falls back to generation's own analysis. Temporal runs independently when frame
generation is off. Histories reset on shape/crop/transform changes, skipped effect
frames or resource pressure. Shader/PSO/allocation failures disable the optional
effect for the session and fall back to spatial presentation.

No jitter means no promise to reconstruct subpixel detail absent from both input
frames. Image-derived block flow is especially weak on HUDs, particles, repeated
textures, transparency, lighting changes and sub-cell motion. Luma rejection is
not a substitute for geometry depth.

## Validation and costs

Run the focused host regressions, using the same DXC flags/profiles as production:

```sh
PYTHONPATH=tests/port DXC=/path/to/dxc python3 -m unittest \
  test_frame_generation test_frame_pacing test_d3d12_fsr test_p59_upscaling -q
```

The float32 host adapter mechanically extracts the production HLSL motion,
accumulation, and vendored AMD EASU/RCAS functions. It compares two deterministic
noisy, smooth moving images: 160x96 → 320x192, translation of 8 native pixels,
independent Gaussian RGB noise σ=0.035. Both paths use identical RCAS strength.
Ground truth is the noiseless continuous scene at output sample centers. Scores
include newly exposed pixels and omit only the shared four-output-pixel border.

| Path | RGB MSE |
| --- | ---: |
| FSR 1 EASU + RCAS | 0.00139461 |
| EASU + temporal accumulation + RCAS | 0.00131095 |

That is about **6.0% less error in this specific denoising scenario**. It is not
proof of better detail recovery or universal image quality. Only 116/240 cells
find the exact expected backward vector, which also illustrates why rejection
and spatial fallback are needed. The test additionally checks local scene-cut
rejection. Existing disabled presentation-command and spatial settings/pass tests
remain in the regression suite.

The host is an Apple M3 Pro, 14 GPU cores. **No GPU milliseconds were measured:**
`MTLCreateSystemDefaultDevice()` returns nil in this execution environment, and
the optional offline Metal compiler reports a missing Metal Toolchain. CPU time
is not relabeled as GPU time. The optional host benchmark is included for a
normal Mac terminal:

```sh
python3 tests/port/p59_metal_shaders.py /tmp/p59.metal
clang++ -std=c++20 -O2 -fobjc-arc -framework Foundation -framework Metal \
  tests/port/p59_temporal_metal.mm -o /tmp/p59-metal
/tmp/p59-metal /tmp/p59.metal
rm -f /tmp/p59.metal /tmp/p59-metal
```

Its MSL adapter extracts production shader math, but the Metal shader execution
could not be validated here. It measures luma + motion + EASU + accumulation +
RCAS, with four warmups and twenty timed command buffers. Even a successful
Metal result would be a host algorithm proxy, not a D3D12/Xbox measurement.

For planning, the [Series S specifications](https://www.xbox.com/en-US/consoles/xbox-series-s)
state 4 TFLOPS and 224 GB/s for its fast memory segment. The per-output temporal
kernel has roughly 700–1100 scalar arithmetic operations, including candidate
validation, Catmull-Rom and neighborhood clamps. At 4K, that is roughly 5.8–9.1
GFLOP: 1.45–2.28 ms at *perfect* 4-TFLOPS utilization before motion, EASU, RCAS,
resource transitions or memory traffic. Texture throughput, data-dependent flow
and memory bandwidth will reduce utilization. This is an operation-count model,
not a calibrated hardware prediction.

| Scale | Series S planning envelope, full temporal chain | Under ~2 ms? |
| --- | --- | --- |
| 1080p → 4K | roughly 4–9 ms | **Do not expect it to fit.** |
| 720p → 1080p | roughly 1.5–3.5 ms | Unproven; may exceed budget. |

Two 4K RGBA16F history images alone use 126.6 MiB. Motion/native histories and
scratch textures add more; allocations remain under the context's shared memory
accounting and may force spatial fallback. This implementation prioritizes
inspectable correctness and rejection over the requested Series S performance
budget. Optimization and console profiling are still required.

Console-only checks remain: build/link against the UWP SDK, actual 60/120 Hz DXGI
refresh reporting and fallback, generated scanout order/pacing under load,
resource state/debug-layer validation, memory headroom, the async budget latch,
input latency, and artifact/quality A/B in MK8D/Wonder and other titles. A display
showing 120 Hz does not demonstrate 120 distinct successfully generated frames.
