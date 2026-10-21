# Community graphics reports, 21 October 2026

## Arceus: world rendering rejected during shader emission

Title: Pokemon Legends Arceus (`01001F5010DFA000`). The supplied
`compat-2026-10-18/r-arceus-rc86/diag.txt` identifies an **Xbox Series X** and
records request `90e38e545d874f3f`, fragment guest `4a4b82e53f441b95`, failing on
`StorageAtomicIAdd64`. This is a shader compilation failure, not evidence of a
missing model file. UI shaders can continue while the world pipeline fails.

`backend/hlsl/emit_hlsl_atomic.cpp::Atomic64` already contained a native
64-bit CAS implementation. However, `TranslationState` never set
`Profile::support_int64_atomics`, and `DxcCache` always selected SM 6.0. Thus even
capable hardware could not use the implementation. Previously queried OPTIONS
and OPTIONS1 describe FP64, waves and integer arithmetic, not buffer atomics.

The D3D12 translator now queries SM 6.6, OPTIONS9 and OPTIONS11. A once-per-process
diagnostic includes query HRESULTs, typed/shared flags, descriptor-heap support,
Int64ShaderOps and the selected path. Failed optional queries select emulation.
For our descriptor-table `RWByteAddressBuffer` resources, native selection requires
SM 6.6, Int64ShaderOps and OPTIONS11's descriptor-heap atomic flag. OPTIONS9's
flags concern typed resources and groupshared memory; requiring those for raw
buffers would incorrectly disable supported hardware. See Microsoft's
[atomic specification](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Int64_and_Float_Atomics.html)
and [OPTIONS11 reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options11).

Native storage/global IAdd, signed/unsigned Min/Max, And/Or/Xor and Exchange use
64-bit compare/exchange, with SM 6.6 selected only for emitted native buffer
intrinsics. The DXIL target has its own cache salt. Serialized pipeline recipes
also have an updated emitter identity partitioned by the two atomic switches,
so prewarming cannot silently bypass an opt-out with an older recipe.

The fallback uses 32-bit InterlockedAdd on the low word and propagates carry
plus the operand's high word with another InterlockedAdd. Other operations use
32-bit CAS retries per word, evaluating signed/unsigned comparisons in 64 bits.
It still requires ordinary shader integer-64 arithmetic, available independently
of integer-64 atomics. It allocates no locks, scratch UAVs or additional buffers.
Global accesses retain their existing address-window bounds checks.

**Atomicity caveat:** two 32-bit atomics are not one linearizable 64-bit atomic.
Addition preserves accumulated sums after all writers finish, if accesses use
this same add protocol and there are no competing stores/other operations.
Returned pairs can tear, readers can observe an unpropagated carry, and min/max
or mixed operations racing across the two words need not match any serial order.
The per-word CAS loops do not fix cross-word races or ABA. This is a compatibility
fallback, not a general-purpose 64-bit synchronization primitive. No spinlock is
used because fragment wave scheduling can deadlock a shader lock protocol.

The existing SPIR-V `StorageAtomicU64` fallback is actually non-atomic
load/modify/store, with a warning, and requires descriptor aliasing. It does not
implement the split-word atomic algorithm suggested in the report. HLSL's
fallback provides stronger per-word behavior while retaining an explicit caveat.
Shared wide exchange is a separate existing capability and remains unchanged.

## Series S: 4K output and boot termination

The reported cross-emulator symptom is consistent with peak allocation pressure,
but **the root cause of a Series S boot termination is not established by the
provided logs**. They concern Arceus on Series X and contain no Series S 4K
allocation failure, device removal or crash record. A display-mode change after
boot succeeding suggests a startup peak or driver/compositor behavior; it does
not by itself prove an out-of-memory failure.

The source audit found:

- D3D12 swap-chain creation and resize previously copied framebuffer-layout
  dimensions without a console-specific upper bound. They use RGBA8 flip buffers
  and `DXGI_SCALING_STRETCH`, so a smaller surface can be display-scaled.
- `game_session.cpp` currently constructs `MesaWindow(window, 1920, 1080)` and
  `MesaWindow` sets that framebuffer layout. The main library/launch D2D renderer
  also uses fixed 1920x1080 buffers. Thus the current guest boot path already
  requests 1080p, regardless of HDMI mode; a 4K guest allocation is not proven.
- The session foreground overlay in `eden_uwp/ui/renderer.cpp` is the exception:
  CoreWindow bounds times RawPixelsPerViewPixel sets its physical extent. It has
  two BGRA8 buffers, premultiplied alpha and `SCALING_NONE`. It may allocate 4K
  even while the guest is 1080p. Capping this surface while retaining foreground
  `SCALING_NONE` would violate its sizing contract, so it is not changed here.
- No NXbox call to HdmiDisplayInformation was found in these paths. No automatic
  guest internal-resolution choice is derived from HDMI. D3D12 texture/render
  scaling follows the existing resolution setting and guest dimensions.
- FSR EASU allocates one output-sized RGBA16F scratch image per active frame
  slot when upscaling; RCAS writes the final target directly. Optional FXAA has
  a separate input-sized RGBA16F scratch image per slot, not another mandatory
  output-sized image.
- Frame generation sizes its output from the actual swap-chain texture. Its
  two RGBA8 histories are bounded by the source extent; when history is smaller
  it also keeps an output-sized real-frame texture. Luma and motion are 8 and
  16 bytes per 8x8 history block. These allocations are lazy, not boot allocations
  merely because the console is in 4K mode.

The new output policy detects the existing AnalyticsInfo DeviceForm string
`Xbox Series S` and caps both initial creation and resize to fit 1920x1080,
preserving aspect ratio and smaller requests. It logs requested/chosen extents,
device form and switch state. Desktop and Series X requests are unchanged; if
DeviceForm is unavailable, it logs that the cap cannot be applied. The viewport
and scissor are remapped to the capped target, so letterboxing, FSR output and
frame-generation buffers use the chosen size. Capture retains its requested
layout. The display compositor scales the smaller guest surface to HDMI output.
No files under `d3d12_present/` or texture-cache budgeting were changed.

### Memory arithmetic

These are texture payloads in MiB (2^20 bytes), excluding allocation alignment,
driver/compositor allocations, transient replacement overlap and all guest RAM,
texture caches, upload buffers and shader compilation. The reported 5 GiB app
limit is shared by all of those; it is not a dedicated presentation allowance.

| Allocation | 3840x2160 | 1920x1080 | Reduction |
| --- | ---: | ---: | ---: |
| One RGBA8/BGRA8 surface | 31.64 | 7.91 | 23.73 |
| Default two swap-chain buffers | 63.28 | 15.82 | 47.46 |
| Optional three-buffer fast policy | 94.92 | 23.73 | 71.19 |
| One RGBA16F EASU scratch per slot | 63.28 | 15.82 | 47.46 |
| EASU scratch across two active slots | 126.56 | 31.64 | 94.92 |
| Two physical foreground overlay buffers | 63.28 | 15.82 | 47.46 |

The overlay row compares display modes; the guest output cap does not reduce it.
Default buffers plus EASU scratch alone would fall from 189.84 to 47.46 MiB if
the guest requested 4K. With three active slots they fall from 284.77 to 71.19 MiB.
FXAA depends on input extent and does not necessarily shrink with this cap.

For 1280x720 source history, frame generation uses about 7.03 MiB for histories,
0.33 MiB for block maps and two output-sized RGBA8 surfaces: about 70.64 MiB at
4K versus 23.18 MiB at 1080p. If histories themselves are 4K, three RGBA8 surfaces
plus maps cost about 97.89 MiB, versus 24.47 MiB at 1080p. Statistics/readback
buffers add small fixed overhead. These figures explain why output allocation
can matter on Series S, but are not a measured crash diagnosis.

## Switches and validation

All new behavior defaults on:

- `NXBOX_D3D12_INT64_ATOMICS=0`: restore rejection of storage/global int64 atomics.
- `NXBOX_D3D12_NATIVE_INT64_ATOMICS=0`: force split32 even on capable devices.
- `NXBOX_SERIES_S_1080P=0`: allow the original uncapped guest output dimensions.

Host coverage extends the existing IR/HLSL test executable with native and
fallback fragment fixtures containing StorageAtomicIAdd64 and all eight storage
siblings. The capability policy also checks every combination of enable switches, shader
model, integer arithmetic and descriptor-heap atomic support. The optional DXC
test compiles the fragment fixtures as `ps_6_6` and `ps_6_0`, plus
compute equivalents, and rejects native atomics at SM 6.0. Output-policy tests
cover Series S 4K/1080p/720p, Series X, opt-out, empty dimensions, portrait and
ultrawide aspect ratios. Existing cache-resolution checks cover the updated
emitter identity and switch partitions.

Reproduce the host checks with an existing DXC executable:

```sh
mkdir -p /tmp/nxbox-p62-scratch
cmake -S tests/port -B /tmp/nxbox-p62-host -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/nxbox-p62-host --target hlsl-ir-tests -j 2
cd tests/port
TMPDIR=/tmp/nxbox-p62-scratch \
NXBOX_HLSL_IR_TEST_BINARY=/tmp/nxbox-p62-host/hlsl-ir-tests \
DXC=/path/to/dxc python3 -m unittest -q
```

Host execution in this worktree built and ran `hlsl-ir-tests` successfully.
The full `tests/port` unittest run passed: **1,019 tests in 606.518 seconds,
45 skips**, including the unavailable DXC check, with 651,265 differential cases.
The 24 focused policy/resolution/opcode/presentation tests passed with one DXC skip. No DXC
executable was available in this sandbox, so actual DXIL compilation of either
new fragment fixture is **not verified** here; the optional tests must be run
with DXC before calling that validation complete. The full suite requires a
Python version with itertools.pairwise (3.10+), fontTools, and pkg-config paths
for the existing fmt/OpenSSL libraries. This run reused Homebrew and cached
fontTools rather than installing dependencies. No new dependency was added.

Console validation still required: build the Windows/UWP renderer, confirm the
capability log and native/split32 selection, verify Arceus world rendering and
correctness under contention with both paths, then cold-boot Series S at 4K and
1080p with the cap on/off. Record AppMemoryUsage/limit, overlay creation size,
chosen guest output size and the first allocation HRESULT/device-removed reason.
Exercise FSR, frame generation, letterboxing, captures and mode switching. A
current fixed-1080p guest request should log 1080p in either HDMI mode; if it still
dies at 4K, investigate the physical overlay and driver/compositor path before
attributing it to guest render targets.

## Other HLSL rejection sites audited

The following is the current opcode inventory with direct
`NotImplementedException` paths. Conditional guards do not mean every use of
that opcode fails. Bound/Bindless image operations must be resolved by TexturePass;
Join must be removed by structured control-flow lowering. The unconditional
backend gaps remain GetPatch, SetPatch, SR_WScaleFactorXY and SR_WScaleFactorZ.

- `emit_hlsl_context_get_set.cpp`: `EmitVertex`, `EndPrimitive`, `InvocationId`, `SetAttributeIndexed`.
- `emit_hlsl_image.cpp`: `ImageFetch`.
- `emit_hlsl_not_implemented.cpp`: `GetCarryFromOp`, `GetOverflowFromOp`.
- `emit_hlsl_special.cpp`: `DemoteToHelperInvocation`, `Join`.
- `emit_hlsl_unsupported.cpp`: `BindlessImageAtomicAnd32`, `BindlessImageAtomicDec32`, `BindlessImageAtomicExchange32`, `BindlessImageAtomicIAdd32`, `BindlessImageAtomicInc32`, `BindlessImageAtomicOr32`, `BindlessImageAtomicSMax32`, `BindlessImageAtomicSMin32`, `BindlessImageAtomicUMax32`, `BindlessImageAtomicUMin32`, `BindlessImageAtomicXor32`, `BindlessImageFetch`, `BindlessImageGather`, `BindlessImageGatherDref`, `BindlessImageGradient`, `BindlessImageQueryDimensions`, `BindlessImageQueryLod`, `BindlessImageRead`, `BindlessImageSampleDrefExplicitLod`, `BindlessImageSampleDrefImplicitLod`, `BindlessImageSampleExplicitLod`, `BindlessImageSampleImplicitLod`, `BindlessImageWrite`, `BoundImageAtomicAnd32`, `BoundImageAtomicDec32`, `BoundImageAtomicExchange32`, `BoundImageAtomicIAdd32`, `BoundImageAtomicInc32`, `BoundImageAtomicOr32`, `BoundImageAtomicSMax32`, `BoundImageAtomicSMin32`, `BoundImageAtomicUMax32`, `BoundImageAtomicUMin32`, `BoundImageAtomicXor32`, `BoundImageFetch`, `BoundImageGather`, `BoundImageGatherDref`, `BoundImageGradient`, `BoundImageQueryDimensions`, `BoundImageQueryLod`, `BoundImageRead`, `BoundImageSampleDrefExplicitLod`, `BoundImageSampleDrefImplicitLod`, `BoundImageSampleExplicitLod`, `BoundImageSampleImplicitLod`, `BoundImageWrite`, `GetPatch`, `GlobalAtomicDec32x2`, `GlobalAtomicInc32x2`, `SR_WScaleFactorXY`, `SR_WScaleFactorZ`, `SetPatch`.

`GlobalAtomicInc32x2` and `GlobalAtomicDec32x2` are legacy rejected entry points,
not current IR opcodes; they do not expand the four active backend gaps above.

Additional context-level guards reject hull/domain entry, geometry passthrough,
legacy varyings, shared wide exchange without shared int64 support, groupshared
memory above 32 KiB or outside compute, constant/storage descriptor arrays,
unsupported resource types, invalid workgroup/geometry counts, unsupported input
attribute/stage linkage (including viewport-index input), and unsupported IR
value/immediate/variable types. Texture guards include dynamic offsets, LOD
clamp, comparison bias, unsupported gather dimensions, non-fetch integer/buffer
sampling, multisample sampling without Load, cube fetch without a 2D-array SRV,
and non-scalar or signed 8/16-bit image atomic UAVs. These remain explicit
failures, not successful empty shaders. Native shared exchange and IsHelperLane
also require SM 6.6; their existing device/target integration is outside this
storage-atomic change.
