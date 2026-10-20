# Bayonetta 2: Fermi2D and texture-upload CPU cost

Title: `01007960049A0000`. Capture:
`artifacts/compat-2026-10-18/r-bayo2-prof/eden.txt` (outside the repository).
No Xbox measurement of this patch or Windows/UWP build has been performed.

## Diagnosis and evidence

The avoidable backend defect is a **GPU → CPU → GPU round trip when synchronizing
D24S8/color aliases**, even though D3D12 already has the conversion shaders. A
successful accelerated Fermi blit does not imply that its cache preparation stays
on the GPU.

Evidence in the supplied capture:

- The requested `profile_rank.py` summary selects 25 windows: texture upload
  **7.22 ms/frame**, Fermi2D **7.12 ms/frame**, about **640 draws/frame**.
- Independently parsing all 39 `D3D12_FRAME_PROFILE` windows with more than ten
  frames (log lines 249–1552) gives medians of **15.01 accelerated blits/frame**,
  **zero software blits**, **2.024 forced texture-download submissions/frame**,
  **9.139 MB/frame texture staging requests**, **3.991 MB/frame repacked uploads**,
  but only **0.254 MB/frame logical texture refreshes**. These are a different
  window population from the summary; do not compare their medians as paired sums.
- The same windows have **zero staging-pressure and allocation-retry submissions**.
  Repacking itself costs **0.436 ms/frame**. Texture download recording is only
  **0.062 ms/frame**, which does not include all completion-callback processing.
- Repeated-upload diagnostics mostly identify small initial/recreated BC/ASTC
  images during gameplay. Earlier windows identify 512-byte CPU-write refreshes.
  They do not establish that an unchanged full-screen texture is uploaded by
  `RefreshContents` every frame.

Source evidence (line numbers refer to the patched tree):

| Location | What it establishes |
| --- | --- |
| `src/video_core/renderer_d3d12/d3d12_rasterizer.cpp:4129` | Fermi acceleration already calls the shared texture-cache blit. Adding another ordinary color blit cannot fix the captured zero-software-fallback case. |
| `src/video_core/texture_cache/texture_cache.h:1270` and `:1281` | Blit prepares both images before issuing the backend operation. |
| `src/video_core/texture_cache/texture_cache.h:3356` and `:3431` | Preparation refreshes dirty contents and synchronizes aliases; alias copies dispatch through `ShouldReinterpret`/`ReinterpretImage`. |
| `src/video_core/renderer_d3d12/d3d12_texture_cache.cpp:1381` | The old GPU reinterpretation path rejects depth, multiple planes and host conversion, including D24S8. |
| `src/video_core/renderer_d3d12/d3d12_texture_cache.cpp:1679` | CPU alias fallback allocates readback staging, downloads, calls `Finish`, then uploads into the other image. This upload bypasses the logical-refresh hook. |
| `src/video_core/renderer_d3d12/d3d12_texture_cache.cpp:837` and `:869` | Converted transfers pack/unpack each depth/stencil row; downloads defer this work to completion callbacks. |
| `src/video_core/renderer_d3d12/d3d12_formats.h:678` | `ConvertDepthRow` loops over every pixel and handles depth and stencil separately. This is CPU conversion, not a native image copy. |
| `src/video_core/renderer_d3d12/d3d12_blit_image.cpp:760` | The copy helper already selects D24S8 ↔ RGBA8 raw conversion shaders. The runtime previously diverted these aliases into CPU staging first. |
| `src/video_core/texture_cache/texture_cache.h:1796` | Ordinary refreshes explicitly return when `CpuModified` is clear. Suppressing invalidations or hashing textures is not justified by this capture. |

The download completion conversion runs after the local `TextureDownload` scope;
it can therefore be charged to the enclosing Fermi2D phase. Upload conversion runs
inside `TextureUpload`. This explains how one alias round trip can contribute to
both large CPU buckets while the download bucket looks small.

**Attribution limit:** the capture has no per-alias format/address trace. It proves
GPU blits plus extra non-refresh uploads and repeated synchronous download
submissions; it cannot prove the exact alias pair or assign all 14.3 ms to D24S8.
The source proves the round-trip defect. A D24S8 ↔ RGBA8 round trip at 1280×720
would reupload 3,686,400 color bytes plus 3,686,400 depth-plane bytes and 921,600
useful stencil bytes, before row padding and other refreshes. That is consistent
with the observed volume, but remains an inference to verify on Xbox.

## Vulkan comparison

`src/video_core/renderer_vulkan/vk_rasterizer.cpp:919` uses the same shared cache.
Its runtime reinterpretation uses GPU image-to-buffer and buffer-to-image commands
(`vk_texture_cache.cpp:1251` and `:1258`), without CPU staging or a blocking finish.
Depth/color conversion also has explicit shader dispatches, including
`ConvertD24S8ToABGR8` at `:1439` and `ConvertABGR8ToD24S8` at `:1461`/`:1480`.
The missing D3D12 piece was runtime dispatch to its existing shaders, rather than
a missing ability to scale/copy ordinary color images.

## Fix

`ReinterpretDepthOnGpu` (`d3d12_texture_cache.cpp:1502`) now handles native,
single-sample 2D D24S8 ↔ RGBA8 UNORM/sRGB aliases on the GPU before CPU fallback.
It supports both guest byte orders, `D24_UNORM_S8_UINT` and `S8_UINT_D24_UNORM`.
The latter needs an explicit 8-bit rotation in the packing shaders; treating the
two layouts as the same host format would move stencil into depth.

The complete copy batch is checked before any GPU write: mip/layer bounds,
matching layer counts, positive extents, offsets, one depth slice, distinct
resources and no resolution-scaled storage. Partial rectangles preserve the rest
of the destination. The existing helper uses raw UNORM views for sRGB aliases,
point loads, stencil export (or the existing stencil fallback), resource
transitions and retained descriptors/resources. No CPU dirtiness or ownership
tracking is bypassed. Other format pairs retain the existing fallback.

Default is enabled. Set **`NXBOX_D3D12_GPU_DEPTH_REINTERPRET=0`** before process
startup to restore the CPU path. Existing `NXBOX_D3D12_CPU_REINTERPRET=1` also
forces the old behavior for this path.

New frame-profile counters:

- `gpu_depth_reinterpret_bytes`: logical bytes routed through the new GPU path.
- `cpu_reinterpret_bytes`: logical bytes sent through the synchronous CPU fallback.

Profiling also emits up to 16 `D3D12_CPU_REINTERPRET src=... dst=... bytes=...`
records to identify remaining CPU format pairs. Counter names and the opt-out do
not change the meaning of existing staging/refresh byte counters.

## Validation

`tests/port/test_d3d12_depth_reinterpret.py` executes the production preflight with
host mocks under ASan/UBSan. It covers both directions and byte orders, partial
copies, late-batch rejection without a partial write, bad offsets/mips/layers,
unsupported formats, MSAA, 3D, scaled/shared resources and both opt-outs. It also
checks all 16,777,216 normalized D24 values for exact numeric round-trip and
compares guest depth/stencil packing against the production CPU conversion.
The existing format-alias fixture still explicitly tests the CPU fallback.

Final requested command: `python3 -m unittest discover -s tests/port` — **954 tests
in 500.214 s; 908 passed, 44 skipped, only the two known environmental import
errors** (`test_button_layout` and `test_xbox_button_icons`, missing `fontTools`).
No assertion failures or additional errors. The four new tests and five existing
format-coverage tests also pass independently after the final fixture changes.
Formatting used `clang-format` on changed C++ ranges and `ruff format` on changed
Python ranges. `git diff --check` passes. No commits were created.

For console acceptance, compare the same warmed gameplay scene with frame
profiling enabled, default versus `NXBOX_D3D12_GPU_DEPTH_REINTERPRET=0`. Confirm
rendered depth effects/stencil and sRGB colors, positive GPU reinterpret bytes,
lower CPU reinterpret bytes, download submissions, texture-upload/Fermi CPU time
and staging volume. Retain other phase totals and frame/GPU timing: this patch
does not establish 60 fps or eliminate the remaining draw/GPU cost. If GPU bytes
stay zero, use the bounded CPU format trace to identify the actual remaining pair;
the D24 attribution would then be disproved for that scene.
