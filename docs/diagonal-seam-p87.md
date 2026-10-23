# p87: diagonal saw-tooth seam

Local investigation on `p87/diagonal-seam`, based on `rc/next`. No network access,
console access, commit or push. The Vulkan/OpenGL comparison uses the Eden code
already present in this checkout, not a newly fetched upstream revision.

## Result

Fixed a real inconsistency in the D3D12 buffer-cache quad-strip helper: its
`{0,1,2, 2,1,3}` split used the opposite winding and a different diagonal from
Vulkan and the main D3D12 converter. It now uses `{0,3,1, 0,2,3}`.

**This is not a confirmed fix for nick_la's seam.** `PrepareDrawIndices` normally
converts quad strips before binding geometry, bypassing that fallback. Moreover,
both the old and new splits share their own edge endpoints exactly; the defect
can affect culling/interpolation, but does not itself demonstrate a raster crack.

Three independent, default-off diagnostics now separate the remaining candidates:
guest position arithmetic, conservative overcoverage, and fullscreen
`DrawTexture` strip interpolation. They share a cache identity policy so serialized
prewarm cannot silently substitute baseline shaders/state during an experiment.

## Evidence and limits

Tester: nick_la, Series S, 1080p, NXbox 1.0.7, Mario + Rabbids Kingdom Battle.
The supplied description says the line persists with FSR and Bilinear. Inspected
`~/.local/share/nxbox/artifacts/nick_la-stripe.png`: a phone photo embedded in a
Discord screenshot shows alternating teeth crossing the scene from the display's
lower-left corner. Perspective and the partial view prevent measuring its exact
screen-space endpoints or identifying the responsible draw. The accompanying
message calls the stripe vertical; the visible photo and task description justify
investigating the diagonal, without treating the wording as a measured direction.

A shared fullscreen diagonal is a strong hypothesis, not proof of missed or
double-covered samples. Texture-coordinate extrapolation, derivatives or a seam
already present in a source texture can also produce a line. There is no game
draw/vertex/HLSL capture here to establish the topology, MSAA count, conservative
bit or shaders of the responsible pass.

## Path audit

Paths below are relative to `src/`.

| Path | Findings |
| --- | --- |
| `video_core/renderer_d3d12/d3d12_index_converter.cpp`, `EmitSegment` | Quads emit `012,023`; quad strips emit `031,023`. The same integer IDs are reused, with the internal edge traversed in reverse. Provoking-vertex handling rotates triangles, not vertex data. No position copying, rounding or half-pixel arithmetic occurs here. |
| Same converter: fans, polygons, triangle strips | Fan/polygon triangles reuse the hub and preceding perimeter ID. Strip parity preserves winding; restart begins a fresh segment. Generated and indexed paths share `EmitSegment`. Conversion removes restart markers, drops incomplete primitives and emits list topology. Degenerates remain degenerate. Native strips retain hardware assembly where conversion is unnecessary. |
| `video_core/renderer_d3d12/d3d12_rasterizer.cpp`, `PrepareDrawIndices`, `MakeDrawParams`, `DrawIndirect` | Indexed, inline and generated conversion paths converge on the submitted converted count, zero first index and one base vertex. Non-indexed generation uses relative IDs and retains the guest first vertex as the draw base. Indirect draws expand through `Draw`, including the same conversion. No independent per-triangle base/position adjustment found. Conversion cache keys retain topology, format, restart and provoking policy. |
| `video_core/buffer_cache/buffer_cache.h`, `BindHostGeometryBuffers`; D3D12 `BindQuadIndexBuffer` | The shared cache can request generated quad indices. The D3D12 fallback's quad-strip table differed in winding/diagonal: fixed. Main draws with `converted_indices` suppress the fallback binding. Quads already matched. |
| `video_core/host_shaders/hlsl/quad_indexed.hlsl` | Helper compute shader uses the same `012023` / `031023` tables as Vulkan; adds the same base to every fetched ID. It does not calculate vertex positions. No active dispatch of `HelperComputePass::QuadIndexed` found; current guest conversion is CPU-side. |
| D3D12 `MakeViewport`, `MakeFullViewport`, `BuildDynamicState` | Viewport scale/translate/resolution adjustment is computed once per viewport, not independently for each face. Height is made positive and final vertex-stage Y is negated to match Maxwell/Vulkan. Negative-Y swizzle adjusts viewport orientation and the same shader policy. No D3D9-style half-pixel vertex offset found. |
| `shader_recompiler/ir_opt/position_pass.cpp` | When viewport transform is disabled, shared frontend IR converts guest window X/Y using render-area reciprocals, multiply and FMA. All backends inherit this pass. It has no primitive-dependent offset, but is part of the arithmetic chain covered by the precise-position diagnostic. |
| D3D12 `GraphicsRuntime`, `UnsupportedState`; HLSL `FinishOutput` | General X/Z/W swizzles and negative X scale are explicitly rejected. Supported Y handling is a sign change; minus-one-to-one Z becomes `(z+w)*0.5`. Conversion is attached to the final vertex/geometry stage. Geometry emission transforms a copy (`nx_vertex`), preserving guest output across repeated emits. No per-triangle FMA rewrite found. Output position was unqualified even though selected IR temporaries are precise; precision is a candidate only where duplicated guest vertices/control-flow compute an endpoint differently. Identical indexed invocations alone do not imply different results. |
| `d3d12_pipeline_cache.cpp`, `FillGraphicsState`; `d3d12_graphics_pipeline.cpp` | Conservative raster follows the guest bit, and can deliberately cover a shared edge in both triangles. That is overcoverage behavior, not evidence of a malformed index split. It remains a conditional candidate until the game's state is captured. New opt-in override isolates it. |
| `d3d12_graphics_pipeline.cpp`; `d3d12_texture_cache.cpp` | PSO and attachments use matching sample counts and quality zero. MultisampleEnable follows sample count; ForcedSampleCount remains zero, SampleMask is all ones. No `SetSamplePositions` programming exists. Guest `multisample_sample_locations` and `viewport_pixel_center` have register declarations but no consumers in these renderers. No face-dependent sample pattern/half-pixel state found. A global sample-location limitation cannot alone explain non-identical endpoints on a shared indexed edge. Sample shading/coverage differences remain possible without a capture. |
| `renderer_d3d12.cpp`, present/upscale/recorder shaders, `d3d12_blit_image.cpp`, `d3d12_clear.cpp` | Presentation, screen capture/recorder, fullscreen graphics blits and masked clears already issue three-vertex oversized triangles. Upscale work also uses compute passes. None supplies an interior fullscreen quad diagonal. Present-layer blending cannot double-cover such a diagonal inside its single primitive. FSR-to-Bilinear persistence agrees with, but does not prove, a source-side seam. |
| D3D12 `DrawTexture`; `shader_recompiler/backend/hlsl/hlsl_draw_texture.h` | Separate from presentation: a four-vertex strip using `id & 1`, `id >> 1`, with one common lerp/divide expression for every vertex. Shared vertices are hardware strip vertices; blend, depth/stencil, bias and conservative raster are disabled. Sampling/interpolation across its two primitives remains a testable candidate. New fullscreen-only single-triangle option removes that interior edge. |

Two existing viewport limitations deserve separation from this report: shader Y
orientation is specialized from viewport zero, so mixed orientations in a
multi-viewport draw are not generally emulated; disabled viewport-transform
window coordinates inherit the frontend's normalization assumptions. Neither is
established as active in this game's pass. No blanket swizzle, pixel-center or
depth-mode behavior was changed on speculation.

## Comparison with the inherited Eden renderers

* Vulkan `vk_buffer_cache.cpp`, `QuadArrayIndexBuffer`/`QuadStripIndexBuffer`, and
  `host_shaders/vulkan_quad_indexed.comp` use `012023` and `031023` respectively.
  They reuse IDs rather than reconstructing shared positions. The D3D12 CPU
  converter already agreed; the fallback helper now agrees too.
* OpenGL `maxwell_to_gl.h::PrimitiveTopology` submits native `GL_QUADS`,
  `GL_QUAD_STRIP`, fans and strips. It does not use the D3D12 CPU expansion.
* Vulkan `GetViewportState` supports signed viewport height and optionally native
  viewport swizzles. D3D12 moves the height sign into the final position Y. OpenGL
  uses viewport/clip-control/depth-range state and native viewport swizzle where
  available. These API differences are not per-face offsets.
* Vulkan uses native negative-one-to-one depth when available; SPIR-V converts
  depth in the final stage when needed. D3D12 always performs the required shader
  conversion for that guest mode. The clip enable derives from the same guest
  geometry-clip modes as OpenGL's depth-clamp decision.
* Vulkan optionally enables conservative overestimation from the guest bit when
  the extension is supported. OpenGL's rasterizer has no corresponding guest-bit
  consumer in this checkout. Backend capability/state differences therefore matter
  more than simply assuming every backend drew the same quad under the same rules.
* Vulkan uses guest sample count and host sample shading, with no custom sample
  locations in this path. `DrawTexture` goes through `BlitColor`; OpenGL uses
  `glDrawTextureNV` when available, otherwise its blit helper. Their blit paths
  differ from D3D12's own strip shader.

Absence of Vulkan/OpenGL tester reports does not establish an endpoint mismatch
in DXIL. A capture of the affected pass is still needed for causal attribution.

## A/B controls in this binary

Set before launching the process; exactly `1` enables each new option. Unset,
empty, `0` and other strings leave it off. Relaunch between cases, with the same
save/camera, upscaler and resolution. Start with all three off, then each alone;
combine only to check an interaction. One build supports all eight combinations.

| Environment variable | Effect and interpretation |
| --- | --- |
| `NXBOX_HLSL_PRECISE_POSITION=1` | Qualifies vertex/geometry output `SV_Position` as `precise`, allowing precision to propagate back through its arithmetic. Does not qualify fragment inputs, change compute precision, disable optimization globally or change the coordinate formulas. A disappearing seam implicates position arithmetic/compilation; it does not identify which guest instruction is wrong. |
| `NXBOX_D3D12_NO_CONSERVATIVE_RASTER=1` | Forces guest graphics PSOs to ordinary rasterization. Logs `suppressed guest conservative rasterization` once if a guest request was actually suppressed. If that marker never appears, this experiment did not exercise conservative rasterization. It can change intentional guest conservative effects, so remains diagnostic-only. |
| `NXBOX_D3D12_DRAW_TEXTURE_TRIANGLE=1` | Uses three vertices and an oversized triangle only when the forward destination rectangle exactly fills the viewport. Preserves source crop, shader interpolation formulas, guest-coordinate normalization and scissor. Fractional, partial, mirrored and empty destinations keep the strip. Logs `DrawTexture fullscreen triangle active` once upon submitting an eligible draw. No marker means the switch cannot exclude the DrawTexture candidate in that scene. |

The common mode bitmask is printed as `D3D12_SEAM_DIAGNOSTICS mode=...` during
disk-cache setup: precise=1, conservative override=2, DrawTexture triangle=4.
Mode participates in guest request specialization and emitter identity bits
10–12. Shader source also changes the DXIL/host-helper cache keys. Serialized
baseline prewarm recipes cannot satisfy an experimental identity. Switching modes
may rebuild cache entries; no manual deletion is required for these new controls.

Existing secondary controls `NXBOX_D3D12_NO_DEPTH_CLIP` and
`NXBOX_D3D12_VIEWPORT_Z01` are still available, but alter clipping/depth behavior
broadly and are not fixes. General swizzle emulation is not silently enabled.
If all three experiments leave the same line, the next useful evidence is the
affected pass's guest indices, vertex outputs, HLSL, attachment sample count and
conservative bit, plus whether the line already exists in its input image. No
console run or capture was performed as part of this task.

## Host validation

Final combined run: 37 unittest cases, 36 passed and one optional-fixture skip;
the separate full HLSL IR CTest also passed. Ruff and `git diff --check` passed.

* Observed red/green regression: `test_fallback_matches_indexed_quad_strip` failed
  on the original `{0,1,2,2,1,3}` table and passes after the fix.
* Native ASan/UBSan test runs actual converter/generated paths for quads, quad
  strips, fans, polygons and triangle strips, all three index widths and provoking
  policies. Verifies a reversed shared edge and exactly one owner for pixel-center,
  fractional and exact-diagonal samples; verifies nonzero generated first vertex.
  This is an independent integer coverage reference, not a D3D12 GPU raster test.
* Diagnostic mode parsing tests cover defaults, invalid strings, all combinations;
  native fullscreen guards reject partial/fractional/mirrored/empty rectangles.
  Tests execute the actual triangle shader's vertex-ID expressions, compare the
  remaining strip/triangle math, and check barycentric UV continuity.
* Built the real HLSL IR fixture locally. Vertex and repeated geometry output tests
  cover qualified/unqualified positions, unchanged Y/depth formulas, unqualified
  inputs and preservation of guest output across GS emits. The full IR suite and
  host binding/storage-image tests pass.
* Related suites passed: index/clear, state coverage, viewport/window origin, MSAA,
  fullscreen diagnostic wiring, compute precision, disk cache, resolution and
  depth recording. The optional real schema-6 cache migration fixture was skipped
  because `NXBOX_PSO_CENSUS_DIR` was not supplied.
* Native seam test compiles with `-Wall -Wextra -Werror -Wshadow -Wconversion` and
  sanitizers. New C++ uses explicit size/value conversions and avoids local-name
  shadowing. There is no Windows SDK/MSVC build or DXC/GPU execution on this Mac;
  C4456/C4457/C4459/C4244 validation on MSVC remains unperformed.

Reproduce the focused checks offline (build output stays outside the worktree):

```sh
cmake -S tests/port -B /tmp/nxbox-p87-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/nxbox-p87-tests --target hlsl-ir-tests --parallel 2
NXBOX_HLSL_IR_TEST_BINARY=/tmp/nxbox-p87-tests/hlsl-ir-tests \
  PYTHONPATH=tests/port python3 -m unittest \
  test_d3d12_diagonal_seam test_hlsl_helpers
ctest --test-dir /tmp/nxbox-p87-tests -R '^hlsl-ir-emission$' --output-on-failure
PYTHONPATH=tests/port python3 -m unittest \
  test_d3d12_index_clear test_d3d12_state_coverage test_d3d12_window_origin \
  test_d3d12_msaa test_d3d12_fullscreen_quad_toggles test_hlsl_compute_precise \
  test_d3d12_disk_pipeline_cache test_d3d12_resolution test_d3d12_depth_recording
```
