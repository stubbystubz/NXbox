# D3D12 FSR 1 presentation

Settings > Graphics > Upscaler selects Bilinear or FSR 1 for the next launch.
FSR 1 is the default, including at 0.5x and 0.75x internal resolution. EASU and
RCAS execute only when the visible source rectangle is smaller than the output
in at least one axis, accounting for rotation. Equal-size and downscaled layers
use bilinear. The renderer uses the actual SRV mip extent rather than applying
the internal resolution factor twice.

The setting persists as `upscaler=fsr1` or `upscaler=bilinear` in
`eden_settings.txt`. `NXBOX_D3D12_FSR=0` in `nxbox_env.txt` overrides the setting
for comparison; `1` enables it. The renderer reads this through `BisectEnabled`.

The vendored AMD MIT implementation is embedded by the existing presentation
CMake rules and compiled by the existing runtime DXC compiler as HLSL 2021,
shader model 6.0. EASU writes one float RGBA intermediate per swapchain slot;
RCAS sharpens directly into the final blended swapchain layer. This removes the
previous standalone RCAS intermediate and final bilinear copy. Crop is applied
once by EASU, and final transformed UVs preserve rotation, flips and viewport
origins. Each swapchain slot owns separate scratch images and descriptor
ranges, retired by its presentation fence rather than draining every frame.
The default remains two buffers; the opt-in `NXBOX_D3D12_PRESENT_FAST=1` uses
three coordinated slots. See [the presentation cost audit](PRESENT-COST.md).
BGRA screenshots retain the existing capture compositor.

## Baseline evidence

The supplied logs predate this change. `winstat.py` reports:

| Log | Median game fps | GPU-thread CPU share |
| --- | ---: | ---: |
| x-botw-rec | 12.9 | 69.9% |
| x-ody-rec | 24.7 | 66.7% |
| x-mk8d-rec | 31.8 | 62.6% |

Median CPU phase time per frame, computed from `D3D12_FRAME_PROFILE` windows:
BOTW draw recording 11.29 ms and descriptor setup 7.57 ms; Odyssey Maxwell
methods 13.23 ms and draw recording 3.56 ms; MK8 Maxwell methods 6.23 ms and draw
recording 3.31 ms. These scopes overlap and must not be summed. FSR does not
remove these CPU costs: its purpose here is to reconstruct lower-resolution
frames while avoiding an unnecessary third presentation pass. No measured frame
rate improvement or locked 60 fps is claimed.

The supplied `dbsz-final105/diag.txt` path was absent.

## Changed files

- `src/video_core/renderer_d3d12/renderer_d3d12.cpp`
- `src/video_core/renderer_d3d12/d3d12_present/present.cpp`, `present.h`,
  `present.hlsl`, `present_math.h`
- `src/eden_uwp/renderer_settings.h`, `mesa_window.cpp`
- `src/eden_uwp/ui/library_screen.cpp`, `settings_model.h`, `strings.h`,
  `lang/en.inc`, `lang/pt_br.inc`
- `NOTICE.md`
- `tests/port/CMakeLists.txt`, `test_d3d12_fsr.py`, `d3d12_fsr_test.cpp`
- This document.

## Validation

Each pattern below was run with:

```sh
python3 -B -m unittest discover -s tests/port -p "<pattern>"
```

| Pattern | Output |
| --- | --- |
| test_d3d12_fsr.py | Ran 6 tests; OK (skipped=1) |
| test_d3d12_resolution.py | Ran 8 tests; OK |
| test_renderer_settings_async.py | Ran 2 tests; OK |
| test_settings_categories.py | Ran 3 tests; OK |
| test_d3d12_redundant_state.py | Ran 8 tests; OK |
| test_d3d12_fence_manager.py | Ran 5 tests; OK |
| test_d3d12_robust_profile.py | Ran 4 tests; OK |
| test_desktop_d3d12.py | Ran 3 tests; OK |

The new regressions failed before implementation. Host clang++ tests execute the
production `DrawLayer`, `BeginFrame`, environment loader, settings persistence
and scale-selection math, with a recording backend replacing D3D12 calls.
`git diff --check` passed.

Not verified here: MSVC/UWP compilation, actual DXIL compilation (DXC is absent
on this Mac), Xbox rendered pixels, Series S GPU timings and sustained fps.
CTest registers `d3d12-fsr-dxc` when DXC and Python are available; it compiles all
presentation entry points using the embedded AMD headers and runtime flags.
