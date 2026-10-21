# D3D12 frame generation

Frame generation is an optional, experimental presenter effect. It uses only
NXbox's final composed images, after bilinear/FSR1 scaling, cropping, rotation and
layer blending. It needs no guest motion vectors, game patches or captured game
assets. It does not increase emulation speed or the game's input update rate.

## Switches

- `frame_generation=0|1` in `eden_settings.txt`, default **0**.
- Settings → Graphics → **Frame generation** saves that preference for the next
  renderer launch. Both English and Brazilian Portuguese strings are included.
- `NXBOX_FRAME_GEN=0|1` overrides the preference. On UWP it can also be supplied
  through the existing `nxbox_env.txt` loader. Only exact `0` and `1` override;
  missing or invalid values use the preference.
- The loader exports the preference separately as `NXBOX_FRAME_GEN_SETTING` so
  loading settings cannot overwrite the testing override.

When disabled, no frame-generation object, shaders, resources, copies, dispatches,
fence waits or extra Presents are created. The existing compositor, FPS overlay,
submission and `Present(1, 0)` sequence remains in place. The host regression test
freezes the previous compositor and presenter bodies, removes only the guarded
new branches, and checks their normalized source hashes against the baseline.

## Presenter flow

1. Compose the new real image with the existing presentation path. Screenshot
   capture continues to use its own target and bypasses frame generation.
2. Lazily compile four SM6 compute entry points and create their PSOs, root
   signature, descriptors and private resources. HLSL is embedded by CMake using
   the existing presenter shader-source template; no runtime shader files or new
   libraries are required.
3. Retain two private RGBA8 history images at the smaller of output and source
   extent (considering rotation and every composed layer). When that extent is
   smaller than output, save the full real image separately and use a bilinear
   compute downsample for history; otherwise copy directly into current history.
   The previous history image is immutable while generating; guest texture-cache images are
   never kept as history because games can overwrite them. History resets on
   output or source-size changes. The first real frame only seeds history.
4. On the 1/8-size grid, sample four luma taps per cell from each image and sum
   fixed-point absolute differences into a four-byte statistic. Read back that
   statistic asynchronously through a three-slot readback ring. Before recording
   this frame's luma pass, use the newest earlier sample whose Context fence has
   completed. A mean difference of **0.22 or greater** skips interpolation; the
   current real frame still becomes the next history image. Quantization is 1/4095
   and the accumulator does not overflow at 3840×2160.
5. Match 3×3 luma stencils in both directions. A coarse search at two-cell spacing
   (the 1/16-scale stencil) covers ±32 history pixels, followed by a one-cell
   refinement extending the range to ±40 pixels (scaled to output when needed). This is a two-scale search on
   one luma grid, rather than separately allocated mip pyramids. A small magnitude
   penalty favors zero motion in flat regions.
6. Reconstruct the midpoint with half-displacement samples from both images.
   Reject inconsistent forward/backward vectors and out-of-bounds samples;
   rejected pixels use the unwarped 50/50 blend. The result is a separate RGBA8
   UAV texture. Barriers order luma, motion and warp passes.
7. Copy the generated image to the current swapchain slot, submit/acknowledge it,
   and call `Present(1, 0)`. Acquire the next slot and wait for its previous fence,
   copy the saved real image there, draw the regular diagnostic overlay, then use
   the existing submit and `Present(1, 0)` sequence. Guest frame-end notifications
   and texture-cache aging still happen once per real composite.

Interpolation is skipped when real-image arrival intervals imply **55 fps or
more**, or an interval is invalid or at least 200 ms (pause/stall). Cadence is measured after real-only presentations, excluding the previous
generated pair’s extra vsync. One real-only probe is inserted after every 30
generated pairs so a game recovering to 55+ fps can leave generation mode; this
can produce a small cadence discontinuity once per second at 30 fps. The displayed
guest counter is useful when checking games whose speed changes during play. This is interpolation with a
known future image: the midpoint is shown only after the next real image exists,
so it adds latency compared with presenting that real image immediately.

## Asynchronous resource reuse and difference decisions

Each of three slots owns a shader-visible ten-descriptor heap and a 256-byte
readback buffer. Dispatches stamp the slot with `context.CurrentTick()` after
recording, including history downsampling on seed/probe frames. The statistic copy
also stamps its completion tick. A rotating search rewrites a heap only when
`context.IsFree(slot.tick)` confirms that its last use has completed. It never
submits or waits to reclaim a slot. If every slot is busy, the effect returns no
generated image, invalidates history and counts a history rejection; the next
available frame seeds history again. GPU resources used by recorded commands are
retained through Context, including the selected readback and downsample inputs.
Shared history, luma, motion, statistic and output textures remain ordered by the
existing single command queue and resource/UAV barriers.

Readbacks are polled before recording new work. Each pending sample carries its
own grid-cell count and a monotonically increasing sample ID, so ring order cannot
replace a newer completed value with an older one. In normal operation the
scene-difference decision is **one real frame late**. A scene cut can therefore
produce one extra interpolated frame, and rejection can shift to the following
pair. If the GPU is further behind, reuse the latest completed value until a newer
one completes; the delay can exceed one frame. Before any sample is available,
assume difference zero. Extent changes, fast-game history resets and ring
backpressure discard cached/pending decisions without clearing ownership ticks.
They cannot make an in-flight heap or readback available early.

Luma continues to run on eligible history pairs even when the cached difference
rejects generation, allowing the detector to recover. Motion and warp run only
when that decision accepts. Known 55+ fps games return before initialization,
history copies, readback polling or compute dispatches. The existing cadence and
periodic real-only probe are unchanged. Neither descriptor reuse nor difference
readback calls `Finish`, `Wait`, or an extra submission; normal presenter
swapchain synchronization and exceptional Context allocation recovery still apply.

The `D3D12_FRAME_GEN` diagnostic format and 300-real-frame interval are unchanged.
`rejected_history` includes unavailable-slot skips and subsequent history seeds.
`generated` and `rejected_difference` count decisions using the cached sample;
`difference_mean` and `difference_max` describe the values used for those decisions
(including startup zero), rather than same-frame measurements or unique samples.
`rejected_timing` retains its existing meaning.

All shader/PSO/resource preparation exceptions latch generation off for the
session and emit one diagnostic line. Private resources are released on failure;
Context retains any pending recorded uses until GPU completion, while root/heaps/PSOs
remain alive through renderer teardown. The generator never writes the incoming
swapchain image. A null result follows the existing real presentation path, with
its render-target state restored before drawing diagnostics. Device removal or
failure of DXGI Present itself remains a renderer/device failure, as on the
original path; an effect cannot recover a removed D3D12 device.

## Estimated cost and limits

At 1920×1080, history plus generated output uses about **23.7 MiB**; the two-channel
luma grid and four-channel motion grid add about **0.74 MiB**, plus allocation
alignment and small descriptors/buffers. At 3840×2160 those totals are about
**94.9 MiB + 3 MiB**. Allocations use Context's existing shared 512 MiB budget;
allocation failure disables the effect instead of retrying every frame.

When a 720p game is displayed at 1080p, the two 720p histories plus the full
real/generated images use about **22.9 MiB**, with about **0.33 MiB** of luma/motion
storage. At 4K output with 720p history the corresponding total is about
**70.6 MiB**, rather than about 98 MiB for full 4K history. The full real copy
preserves the original image exactly; only generated frames use lower-resolution
history and can look softer. All layers and the existing scaling result are
preserved by downsampling the final composition, rather than selecting a single
guest texture.

At 1080p history the matcher evaluates approximately 20 million bidirectional stencil
comparisons (about 9 million at 720p), followed by a full-screen warp and copies. The intended Series S
budget is approximately **2 ms GPU per generated frame**; this is a target, not a
measurement. Descriptor reuse and scene-cut readback no longer block the CPU on
GPU completion. The ring adds two small heaps and two 256-byte readback buffers
(overhead is subject to D3D12 allocation alignment). Measure at 720p and
1080p first; do not assume 4K meets the target. No Xbox GPU timings or Windows/UWP
build verification are claimed by host tests.

HUD text can double or bend because it shares the image with moving scenery.
Fast motion beyond the search radius, thin edges, particles, transparency,
repetitive textures, camera cuts and disocclusions can produce ghosting, blur or
incorrect motion. Large brightness changes may conservatively skip a frame;
cuts with similar luma can evade detection. Generated frames do not improve
controller response and should remain an explicit user choice.

## Console validation

1. Compare default-off images and performance against the original build at the
   same resolution/upscaler. Test both explicit setting 0 and missing setting.
2. Enable the preference and test `NXBOX_FRAME_GEN=0`, then enable via the override
   with the preference off. Restart the renderer between changes.
3. Use steady 25–32 fps scenes. Enable **Show FPS** (`NXBOX_SHOW_FPS=1` also works).
   With generation enabled the overlay reads **presented FPS / game FPS**, e.g.
   `60 FPS / 30 FPS`; the first number counts successful swapchain Presents,
   the second remains the guest acquisition counter. Without generation it keeps
   the original `FPS / ms` text. Counters settle over approximately one second.
   On a 60 Hz TV, two synchronized Presents cap real-frame delivery at 30 fps;
   25 fps may reach about 50 presented fps, rather than a guaranteed 60.
4. Check HUD, menus, scrolling text, camera pans, fast turns, particles, occlusions,
   loading screens, fades/cuts, window resize and long runs with memory pressure.
   Verify first-frame/fast-game skips and delayed cut rejection (one extra generated
   cut frame is expected), no black frames, and no device-removal or
   resource-state validation errors. Check games recovering from slow to fast
   scenes and verify generation stops after the next periodic real-only probe.
5. Capture GPU timings for luma/readback, motion, warp and copies; compare whole
   frame time and pacing with the effect off. Check Series S memory pressure and
   the 2 ms target. Verify 60 fps games stay on the one-Present path.
6. Measure input-to-photon delay with the same controller action and camera setup
   with the effect off/on. Inspect a recorded alternating generated/real sequence
   for correct temporal ordering. A TV's own interpolation should be disabled.
7. Inject shader compilation/PSO/allocation failures in a diagnostic build. Expect
   one `D3D12 frame generation disabled for session` line, real images continuing,
   and no retries until a fresh renderer session.

## Host checks

`tests/port/test_frame_generation.py` compiles production settings and policy,
executes the production exception boundary with injected failures, replays the
production generated-present helper against a two-slot mock swapchain, and checks
the default-off source against the pre-change command path. It covers persistence,
strict override precedence, cheaper history extents, warmup, fast-game/cut/stall skips, failure logging and
session latching, image ordering and swapchain slot-fence reuse. It also executes
production asynchronous slot selection, sample polling and generation control flow
against a fake Context: busy-slot skips, exact ownership ticks, one-frame-late cut
decisions, stale samples, newest-completed selection, normalization, reset safety,
downsample ownership and fast-game dispatch bypass. Source checks prohibit
synchronous completion calls in the generation path. Existing FPS, settings and
presentation tests remain applicable. These checks do not execute a D3D12 GPU.

Run the full suite with a scratch `TMPDIR`:

```sh
mkdir -p /tmp/p52-tests
cd tests/port
TMPDIR=/tmp/p52-tests python3 -m unittest -q
```

On this Mac, expose the already installed fmt/OpenSSL pkg-config metadata and
cached FontTools package as described in `docs/DRAW-CPU.md` when running the suite.

## Files changed

| Files (relative to the repository root) | Change |
| --- | --- |
| `src/video_core/renderer_d3d12/renderer_d3d12.cpp` | Guarded integration, source extent, cadence and generated/real swapchain sequencing |
| `src/video_core/renderer_d3d12/d3d12_present/frame_generation.cpp`, `frame_generation.h`, `frame_generation.hlsl`, `frame_generation_policy.h` | Resource/PSO ownership, compute passes, history selection, skip/failure policy |
| `src/video_core/renderer_d3d12/d3d12_present/CMakeLists.txt`, `shader_source.h.in` | Embedded frame-generation HLSL and build registration |
| `src/video_core/renderer_d3d12/d3d12_present/present.cpp`, `fps_bitmap.h`, `src/common/fps_overlay.h` | Presented/game counters and slash glyph |
| `src/eden_uwp/renderer_settings.h`, `mesa_window.cpp` | Preference persistence and runtime environment bridge |
| `src/eden_uwp/ui/library_screen.cpp`, `settings_model.h`, `strings.h` | Settings toggle and localization keys |
| `src/eden_uwp/ui/lang/en.inc`, `pt_br.inc` | English and Brazilian Portuguese labels/explanations |
| `tests/port/test_frame_generation.py` | Host regression tests |
| `docs/FRAME-GENERATION.md` | Design, cost estimates, controls, limitations and validation |

## Validation result

The p52 host validation uses the full `tests/port` suite with a scratch `TMPDIR`,
the already installed fmt/OpenSSL pkg-config metadata and cached FontTools package
described above. No dependencies are added. The full run completed successfully:

```text
Ran 1012 tests in 949.074s
OK (skipped=45)
Differential cases: 651265
```

The focused frame-generation run passed all seven tests. Ruff formatting/checks,
clang-format checks on the changed C++ regions and `git diff --check` passed.
Scratch files were removed; no commit or push was made.

Host replays do not execute a D3D12 GPU. Windows/UWP compilation, console fence
validation, scene-cut image quality, latency, throughput improvement and the GPU
budget remain unverified here; use the console checklist above.
