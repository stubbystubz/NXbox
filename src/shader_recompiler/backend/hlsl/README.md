# HLSL text backend

`Shader::Backend::HLSL::EmitHLSL(profile, runtime_info, program, bindings)` emits a
`main` entry point for DXC Shader Model 6.0 or 6.6. It consumes the optimized Maxwell IR
and collected `Shader::Info`, including the structured syntax list. It does not
call GLSL, SPIR-V, Mesa, or DXC. The renderer owns DXC compilation and capability
checks. HLSL sources are compiled into `shader_recompiler` independently of
`ENABLE_OPENGL`.

## Integration

- Vertex A/B use `vs_6_x`, fragment uses `ps_6_x`, compute uses `cs_6_x`,
  and geometry uses `gs_6_x`, selecting x from `RequiredShaderModel(program)`.
  Merge dual vertex programs and run the usual IR lowering/info collection first.
- `TEXCOORD0..31` identify guest generic attributes, with vertex signed/unsigned
  input types taken from `RuntimeInfo::generic_input_types`. Integer vertex input
  and integer render-target writes preserve bits with `asfloat`/`asint`/`asuint`.
  Fragment input interpolation comes from `Info::interpolation`; missing previous
  stage components read `(0,0,0,1)`. This version declares full four-component
  generic fields. DXC validation must reject programs exceeding native stage
  signature limits; component packing is a remaining integration task.
- `Bindings::uniform_buffer` assigns CBVs in constant-buffer descriptor order.
  Each guest buffer is a 64 KiB `cbuffer` of `uint4` words, retaining HLSL's
  16-byte array stride. Loads accept dynamic guest buffer indices and byte offsets.
  Out-of-range constant-buffer loads return zero.
- `Bindings::texture` assigns SRVs to texture descriptors first, then texel-buffer
  descriptors. This deliberately follows the P3 contract, which differs from
  GLSL's texel-buffer-first declaration order. Sampler register `sN` pairs with
  texture register `tN`; depth descriptors require comparison samplers.
- UAVs share one register namespace: start at
  `bindings.storage_buffer + bindings.image`, assign storage buffers first, then
  images and image buffers. Those two counters count resources, not two separate
  D3D12 namespaces. Consecutive calls append their UAV spans. The descriptor
  allocator must use that same per-stage append order when sharing counters
  across stages. The storage buffers are one array, `ssbo[N]`, indexed by the
  local IR descriptor index; its first register includes the binding offset. Global
  memory searches its windows in one loop (`nx_global_find`) and indexes that array,
  like dynamic texture/image arrays, with `NonUniformResourceIndex`.
- Sampler registers use `bindings.hlsl_sampler`, independently of the SRV counter:
  texel buffers consume SRVs but no sampler slots. Sampler variable names retain
  the corresponding texture's SRV number. Each sampled descriptor advances the
  sampler span, including MSAA descriptors with an unused sampler slot.
- `hlsl_root_constants.h` defines the exact 64-byte CPU ABI at `b0, space1`:
  render area DWORDs 0..3, resolution down factor 4, Y direction 5, texture/image
  scaling masks 6/7, base vertex/instance 8/9, draw ID 10, reserved 11..15.
  Upload all 16 DWORDs, using `RootConstants` defaults for identity transforms.
  Populate scaling masks with local IR indices. Static vertex Y inversion and
  depth-range conversion use `RuntimeInfo` at every return.
- A failed emission throws `NotImplementedException` and leaves binding counters
  and IR instructions/use counts unchanged. Do not cache an empty shader or
  replace an unsupported program with a trivial shader.

## Coverage relative to GLSL

| Area | Implemented |
| --- | --- |
| Arithmetic | GLSL's implemented integer, FP32/FP64, logical, comparison, conversion, select, bitwise and composite emitters; carry/overflow/zero/sign pseudo-results; packed half and double bit conversions |
| FP64 | Native double arithmetic and FMA; bit-based abs, NaN tests, truncation and nearest-even rounding helpers avoid implicit FP32 narrowing |
| Control flow | Structured if/else regions, conditional break, loops/repeat, early returns; parallel phi snapshots preserve loop-carried swaps; 8192-iteration loop safety |
| Stage interface | Vertex/fragment/compute inputs and outputs, integer targets, position, front face, vertex/instance/primitive IDs, fragment depth, coverage, clip distances, optional vertex layer/viewport outputs |
| Buffers | Byte-addressed constant-buffer loads; storage/shared 8/16/32/64/128-bit loads and stores; subword writes use CAS; local memory; 64-bit guest-address lookup through storage descriptor base/size |
| Atomics | All scalar 32-bit shared/storage/image integer atomics, including signed min/max and wrapping increment/decrement via CAS; storage FP32 add and packed half2 add/min/max via CAS; storage word-pair operations use independent 32-bit atomics |
| Textures | 1D/2D/3D/cube and array resource declarations; implicit/explicit LOD, bias, depth compare, gather component/four offsets, gradients, dimensions/LOD queries, MSAA/texel loads; literal offsets; typed integer fetch bitcasts |
| Images | Typed unsigned storage images and buffer images, load/store and scalar image atomics; typeless images as float4 (raw bits) or uint4 by the bound view's kind, reading zero without additional typed UAV loads; R8/R16 signed images as int |
| Waves | Lane ID, 32-lane partitioned ballots/votes/masks, segmented index/up/down/xor shuffle and in-bounds result, quad lane reads, swizzle add, fine/coarse derivatives |
| Synchronization | Group barrier with synchronization, group/device memory barriers, fragment discard |

Every IR opcode has a dispatch target. Dispatch coverage does not imply semantic
coverage: the base commit had 324/534 emitters with non-rejection bodies; this
sprint increases that to 483/534. There are 47 frontend-only operations (`Join`
and bound/bindless textures) that require the same lowering as Vulkan/GLSL,
and four remaining rejected backend opcodes: `GetPatch`, `SetPatch`,
`SR_WScaleFactorXY`, and `SR_WScaleFactorZ`. Those four are gaps, not emulations.

Scalar FP16 arithmetic uses FP32 carriers and explicit half rounding at each
result. Scalar/vector FP16 and FP64 composites, selects, bitcasts, and every
conversion combination are emitted. Signed narrowing sign-extends from the
requested bit width. The numeric fixture enumerates all 189 numeric opcodes
from the real IR table and emits every one. FP16 FMA still inherits HLSL `mad`
fusion behavior and FP32 intermediate rounding; it is not a bit-exact software
half FMA. FP64 remains native double, not software FP64 emulation.

Global subword stores use CAS to preserve adjacent bytes. Global atomics use
range-checked guest-address lookup, select only the first matching storage
range, and reuse storage operation semantics. Invalid ranges return zero or
ignore stores. Native 64-bit storage/global atomics use
`InterlockedCompareExchange64`, require `Profile::support_int64_atomics`, and
require SM 6.6. Without native buffer support, `hlsl_emulate_int64_atomics`
(default true) selects split-word 32-bit atomics: add with carry, or per-word CAS
for the other operations. The pair is not a linearizable 64-bit transaction;
returned values and cross-word min/max updates can tear under contention. See
[the October bug investigation](../../../../docs/known-bugs-oct21.md).
Mixed-width shared memory uses one uint64 backing array with
native CAS for narrow operations, preserving byte aliasing and wide exchange
atomicity; it requires `Profile::support_shared_int64_atomics` and SM 6.6.
Shared wide exchange has no split-word substitute.

`RequiredShaderModel(program)` returns a conservative 60 or 66 from IR alone,
including atomics that can now be emulated. D3D12 selects SM 6.6 for emitted native
buffer atomic intrinsics and SM 6.0 for their split-word fallback. Callers must
expose matching device capabilities in Profile.
`IsHelperInvocation` uses SM 6.6 `IsHelperLane`. A shader requiring 6.6 must
fall back if the device/compiler target cannot provide it.

Geometry emission supports input primitive arrays, per-vertex attribute reads,
geometry invocation IDs, a single output stream, Append/RestartStrip, and
position conversion at append without repeatedly modifying the guest output.
Passthrough and multiple streams still reject with linkage-specific reasons.
Fragment alpha testing captures guest target-zero alpha even if the color
attachment is masked. Dual-source and transform-feedback varyings retain their
normal HLSL output semantics; native PSO stream-output and blend linkage must
be supplied by the pipeline track.

## Explicit gaps and limits

- Hull/domain stages and geometry passthrough reject at entry. A correct hull
  implementation needs coordinated control-point/patch linkage and guest
  inter-invocation barriers; duplicating the guest program in a patch-constant
  function would duplicate storage side effects. `GetPatch` and `SetPatch`
  therefore remain explicit gaps. No empty tessellation shader is substituted.
- Native 64-bit atomics and helper queries require the shader model selected by
  `RequiredShaderModel`; SM 6.0 is insufficient. D3D12 descriptor-heap buffer
  atomic support and shared atomic support must be checked separately. See
  Microsoft's [SM 6.6 atomic specification](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Int64_and_Float_Atomics.html).
- Explicit comparison-sampling LOD is supported only when the IR LOD is literal
  zero, using `SampleCmpLevelZero`. Arbitrary comparison LOD requires a later
  shader model; see the [SM 6.7 announcement](https://devblogs.microsoft.com/directx/shader-model-6-7/).
  Depth bias, LOD clamps, dynamic sampled offsets and cube texel
  fetch reject. Sparse residency returns always-resident for committed resources. Integer filtered sampling rejects and requires texel fetch.
- Narrow signed image atomics, cube UAVs, constant/storage descriptor arrays,
  viewport masks, legacy varyings and multiple geometry streams
  reject. Vertex layer/viewport outputs require the device capability in Profile.
- Points are always one pixel: the vertex `PointSize` store is dropped, `PointSize` loads
  read 1.0 and `PointSpriteS`/`PointSpriteT` loads read 0.5, the sprite coordinate of the
  single fragment a 1px point produces. A renderer that expands points into quads must
  supply the real coordinate instead.
- W-scale-factor system registers `SR_WScaleFactorXY` and `SR_WScaleFactorZ`
  reject: neither reference backend supplies a defined register mapping.
  Helper queries use SM 6.6. Demote retains HLSL discard, matching GLSL's
  approximation; helper continuation is not a proven complete demote emulation.
- Wave ballots/shuffles assume host waves have at least 32 lanes. Check D3D12
  OPTIONS1 before selecting this backend. Cross-lane reads of inactive/helper
  lanes have native HLSL undefined behavior. Profile's gather subpixel workaround
  and per-instruction floating-point denormal/rounding modes are not applied.
  FP32 FMA maps to `mad`, whose fusion behavior is hardware-dependent; see
  [Microsoft's mad documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/mad).

## Host validation

The std-only root ABI/binding helper test is included in the Python port suite
and standalone CMake build. When fmt and Boost headers are available, CMake also
builds the complete emitter with Eden's real IR implementation and exercises
vertex, fragment, compute, phis/loops, wave pseudo-results, depth compare/gather,
resource ordering, image operations, repeated emission and failure rollback.

```sh
cmake -S tests/port -B /tmp/nxbox-port-tests \
  -DCMAKE_PREFIX_PATH=/path/to/fmt \
  -DHLSL_BOOST_INCLUDE_DIR=/path/to/boost-headers
cmake --build /tmp/nxbox-port-tests --parallel 2
ctest --test-dir /tmp/nxbox-port-tests --output-on-failure
cd tests/port
python3 -m unittest discover -s . -p 'test_*.py'
```

IR tests leave `hlsl_vertex.hlsl`, `hlsl_fragment.hlsl`, `hlsl_compute.hlsl`,
`hlsl_waves.hlsl`, `hlsl_arithmetic.hlsl` and `hlsl_resources.hlsl` in the build/test working directory.
If DXC is available, CTest also validates these files as DXIL. Host C++/text
checks alone are not proof of DXIL validity or rendered guest-shader correctness;
Windows/Xbox DXC and GPU integration validation remain necessary.

## Coverage and integration contract

The coverage audit enumerates `opcodes.inc`, checks that every opcode has exactly
one dispatch emitter, prohibits blanket NotImplemented stubs, and reports
implementation, frontend-lowering, and rejected-gap counts separately. Adding an
undocumented rejection fails the test. Full implementation remains incomplete:

```sh
python3 tests/port/test_hlsl_opcode_coverage.py --report
# Exits nonzero while any backend opcode gap remains:
python3 tests/port/test_hlsl_opcode_coverage.py --strict
# Run the real IR fixture and optionally all sixteen DXC samples with -WX:
NXBOX_HLSL_IR_TEST_BINARY=/path/to/hlsl-ir-tests DXC=/path/to/dxc \
  python3 tests/port/test_hlsl_opcode_coverage.py
```

DXC skips cleanly when unavailable. A prebuilt IR fixture is selected explicitly
so the Python suite does not silently launch a large compiler build. Local
validation used official DXC 1.9.2609.5 for Windows through an isolated CrossOver
bottle; both shader models compiled with warnings treated as errors. This is
compiler validation, not guest-rendering or GPU race validation.

Notes for pipeline/robustness integration:

- Pipeline compilation must call `RequiredShaderModel`, enable actual buffer,
  heap-buffer, and shared int64 atomic feature flags, and route gs_6_0 programs
  into the GS slot with matching primitive topology. The backend cannot alter
  renderer-owned files in this track.
- Stream-output declarations must map output semantics to xfb buffer/stream/
  stride/offset metadata; dual-source blending requires the native RTV/PSO
  configuration. Wide-point rasterization and viewport masks need pipeline-side
  emulation before their shader attributes can be implemented; points stay 1px (above).
- Renderer exception containment and log-once fallback belong to T4. Emission
  remains transactional for descriptor counters; exceptions carry the specific
  opcode, resource, capability, or stage-linkage reason.
- Full software FP64, exact FP16 FMA/rounding modes, hull/domain stages, and
  remaining texture variants are not provided by this change. The default
  renderer remains unchanged.


## T9 native stage integration

The native pipeline now declares fragment/geometry inputs from the producer's entire
varying layout, including unused float4 components. Generic semantics appear in the
same order on both sides of a stage boundary; rasterizer-generated pixel system
values follow that layout. Vertex and instance IDs include the guest base offsets
passed through root constants. Coordinate conversion runs only at the final vertex
stage when a geometry shader is active.

Stream-zero transform feedback is connected to native stream output: the root
signature allows it, both legacy and advanced depth-stencil PSOs receive its
declaration, guest buffers are synchronized and marked written, and filled-size
counters are retained across draws. A separate `NX_XFB_POSITION` export captures
position before host coordinate conversion. Rasterizer discard works for non-empty
stream-output layouts. Multi-stream geometry and legacy stream-output semantics
still fail explicitly.

The native input assembler receives a separate slot per attribute. Misaligned or
expanded inputs are repacked, three-component 8/16-bit values receive the correct
fourth component, explicit BGRA byte ordering is converted, and scaled integers use typed
integer signatures followed by numeric conversion. Unsupported format combinations
(including BGRA R11G11B10) are skipped with their format/type reason.

DrawTexture has a real rectangle/sampler blit for normalized/float, single-sample
2D and 2D-array sources. Source/destination aliasing uses a feedback snapshot.
Integer and multisample blits remain explicit fallbacks. Hull/domain stages,
GetPatch/SetPatch, geometry passthrough, wide-line/point expansion and SM 6.6
renderer target selection remain unfinished. The 4511-shader corpus tests emission
and DXIL validation; it does not exercise runtime stream-output cursors or Xbox
pixel correctness. `tests/port/test_t9_stages.py` adds executable fetch/conversion
regressions and optional actual-emitter VS/GS/PS and DXC validation.

Vertex aliases A8, G8_R8, and X8_B8_G8_R8 follow the Vulkan/GL raw-component
interpretation (R, RG, and RGBA respectively), rather than inferring channel
swizzles from their names. Only the explicit BGRA flag changes channel order.

Stream-output unbinding for a clear/blit preserves filled-size counters; disabling
feedback on a regular draw requests a reset on the next capture while keeping the
last counters readable.
Guest streaming-byte-count reset/resume still needs query-cache integration: the
existing `QueryCacheRuntime::ResetCounter` baseline does not apply to its direct
`ReadFilledSize` path. That file is outside T9 ownership. Runtime Xbox validation
of cursor and overflow behavior remains required.
