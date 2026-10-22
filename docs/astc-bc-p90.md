# p90: ASTC storage must not select the wrong native BC path

Local, uncommitted changes on `p90/astc-bc`. Read `totk-memory-p76.md` first.
No network, gh, commit, push, Xbox access, deployment or console run was used.

**Found and fixed two concrete storage eligibility defects and a volume mip
packing defect. The supplied captures do not identify the texture responsible
for the screen rectangles. Visual acceptance remains pending.** The opt-in
trace and exclusion rules below cover that remaining investigation.

## Evidence and hypothesis assessment

Capture root: `~/.local/share/nxbox/artifacts/compat-2026-10-18/`.

- `r-totk-p76/diag.txt:37`: `ASTC_STORAGE recompression=2 env_bc=true native=true`.
  Line 38 selects CPU asynchronous ASTC (`astc=2`, `astc_sync=false`).
  `r-totk-p76-nobc/diag.txt:36` selects recompression 0. Both disable GPU ASTC.
- Inspected `r-totk-p76/shots/shot-0230s.png`, 1920×1080. The right-hand dark
  areas have hard, nested rectangular edges; underlying cave details remain
  visible. This is consistent with sampled lighting data, but does not identify
  an RT, mip, layer or GPU address. The BC-off capture is not scene matched.
- BC-on `eden.txt:531`, `:533`, `:534`, `:628`, `:630` warn about irreversible
  ASTC readback. BC-off `eden.txt:423`, `:469`, `:471` has the same warning class.
  The logger deduplicates by format/type/condition, not by image address
  (`src/video_core/renderer_d3d12/d3d12_texture_cache.cpp:46`). These lines
  cannot establish which image was an attachment/copy destination or caused
  the rectangles. `type=1` means 2D, not 3D.
- Searched both runs' `eden.txt`, `diag.txt`, `crash.txt` for ASTC, BC and
  texture/transfer warnings. No BC pitch/bounds/alignment/allocation diagnostic
  identifies an offending image. Both crash logs also contain comparison-LOD
  shader failures; those are not unique to BC-on.

File:line references below refer to the final worktree unless marked pre-change.

| Hypothesis | Code evidence and conclusion |
| --- | --- |
| Wrong image class enters native BC sampling | **Confirmed eligibility bug.** The constructor selects BC3 for ASTC at `src/video_core/renderer_d3d12/d3d12_texture_cache.cpp:249`. Existing Xbox small-cube and array protections at `:262` and `:276` require `IsPixelFormatBCn(info.format)`. ASTC guest formats therefore bypassed them even after becoming BC3 host storage. The existing comments document small BC ambient cubes sampling about 20× too dark and BC array layers sampling shifted rows. This is a strong candidate for lighting corruption, not proof about this TotK image. |
| Partial base blocks change the texture geometry | **Confirmed eligibility bug.** `MakeTexture` rounds the resource base dimensions to the host block size at `src/video_core/renderer_d3d12/d3d12_texture_cache.cpp:112`. A 5×3 ASTC asset becomes an 8×4 BC resource, changing normalized sampling and the mip dimension chain. Repacking the encoded bytes correctly cannot preserve the original geometry. Such assets now remain RGBA. |
| Wrong CPU subrectangle origin | No such path found in the shared ASTC converter: `src/video_core/texture_cache/util.cpp:937` asserts zero origin, base layer zero and the full mip extent. Sync refresh at `src/video_core/texture_cache/texture_cache.h:1994` unswizzles the image; async refresh at `:2237` consumes equivalent full-mip copies. This does not rule out later GPU copies, aliases or a future partial caller. The new trace records every upload region. |
| Wrong BC encoder pitch/layer offset | `src/video_core/textures/bcn.cpp:56` uses ceil(width/4) blocks per row and ceil(height/4) rows per plane; slice `z` advances by that plane stride. D3D transfer at `src/video_core/renderer_d3d12/d3d12_texture_cache.cpp:587` separately computes block row/slice strides and repacks per subresource. No new BC row/array packing error was established. |
| Volume mip offset | **Confirmed secondary bug.** The RGBA branch advanced `output_offset` without multiplying by mip depth. Fixed at `src/video_core/texture_cache/util.cpp:957`. BC output sizing also used base depth for every mip; `:624` now uses mip depth. Volumes now remain RGBA in D3D12, making correct RGBA offsets essential. Neither capture proves a volume caused the rectangles. |
| sRGB / alpha | The constructor retains BC3_SRGB for sRGB ASTC (`src/video_core/renderer_d3d12/d3d12_texture_cache.cpp:254`). BC3 invokes `CompressBCN<16, false>` at `src/video_core/textures/bcn.cpp:80`; it does not use the BC1 alpha threshold. `externals/stb/stb_dxt.cpp:755` encodes alpha from channel 3 separately. Lossy alpha used as data remains a possible title-specific issue; neither logs nor format metadata identify its semantics. |

## Change and diff

Full diff: see the p90 commit.

The D3D12 policy in `src/video_core/renderer_d3d12/d3d12_astc_bc.h:15` permits
new ASTC→BC storage only for single-sample 2D images with one layer, depth one,
and nonzero base width/height divisible by four. Arrays, cubes, volumes, 1D
images and nonaligned bases use the original ASTC→RGBA decode. This is a
conservative restriction, including larger arrays/cubes whose native BC behavior
was not established. Existing native guest BC workarounds and allocation fallback
remain intact. Small mip tails of an aligned base retain the existing BC path.

The storage choice is latched in `ImageBase::astc_uncompressed`
(`src/video_core/texture_cache/image_base.h:97`). It controls converted staging
size, inline decode, the worker's captured decode policy, and registration/removal
memory estimates. Merely changing DXGI format would upload BC bytes as RGBA;
this patch changes the decoded byte layout too. The shared APIs default to their
old policy for other renderers. No global setting is temporarily changed by a
worker or a filter.

Core changes, abbreviated:

```diff
- switch (Settings::values.astc_recompression.GetValue()) {
+ astc_uncompressed = candidate && (!AstcBcEligible(...) || skip_matches);
+ if (astc_uncompressed)
+     converted_size_bytes = CalculateConvertedSizeBytes(info, true);
+ switch (astc_uncompressed ? Uncompressed : configured_recompression) {

- ConvertImage(input, info, output, copies);
+ ConvertImage(input, info, output, copies, captured_astc_uncompressed);

- output_offset += width * height * layers * 4;
+ output_offset += width * height * layers * mip_depth * 4;
```

Eligible 2D assets retain BC3 savings. Excluded images use more texture/staging
memory than the p76 opt-in; there is no measured savings claim for this patch.

## Opt-in diagnostics and exclusions

Set `NXBOX_ASTC_BC_DIAG=1` in `LocalState/nxbox_env.txt`. With recompression
selected, `ASTC_BC` records cover candidate images, including those kept RGBA:

- `create`: GPU address, dimensions/depth, mip count, layers, image type, guest
  ASTC block dimensions, format/sRGB, host format/block dimensions and RGBA policy.
- `view`: requested format/type, mip/layer range and RT-view intent, before view
  validation, so rejected reinterpretations remain visible.
- `full_upload` / `partial_upload`: **every region**, including mip, base/count
  of layers, origin, extent, source byte offset/size, row length and image height.
  One mip or some layers being updated is distinguishable from all-image upload.
- `rt_view` is attachment-view intent. `rt_bind` is logged at the main draw's
  `OMSetRenderTargets` recording (`src/video_core/renderer_d3d12/d3d12_rasterizer.cpp:2236`).
  `copy_dst_candidate` includes Fermi preflight; `copy_dst` and `view_copy_dst`
  mark entry into copy/reinterpret/MSAA/blit/convert destination paths, including
  attempts that subsequently fail validation. These events are not proof that
  the GPU completed a write. Join events by GPU address and creation lifetime;
  creation resets state, and framebuffer snapshots may repeat binding events.

The trace is off by default; view/upload strings are constructed only when enabled.
Binding/use events are deduplicated per image/view. Upload regions are not capped.

`NXBOX_ASTC_BC_SKIP` is latched at image construction. It matches case-sensitive
substrings, comma-separated OR, against guest format, `size=WxH`, hex `gpu=...`,
and class tags. Matching images decode ASTC directly to RGBA, with matching
staging size and resource format. Examples:

| Value | Effect |
| --- | --- |
| `ASTC_2D_8X8_UNORM` or `8X8` | Exclude that guest-format group. |
| `size=128x128` | Exclude matching base dimensions. |
| `gpu=1234abcd` | Exclude an address/prefix seen in the trace, if it recurs on replay. |
| `srgb` | Exclude sRGB candidates. |
| `array` / `cube` | These classes already remain RGBA under the structural fix. |
| `partial` | Exclude images with any non-4-aligned BC mip extent, including ordinary 2×2/1×1 tails. This is a **partial-block** class, not a prediction of later subrectangle uploads. Use the logged format/size/address to exclude a partial-upload image on replay. |
| `alpha` | Conservatively exclude all ASTC candidates: ASTC is alpha-capable and has no opacity/data-role metadata. It does not pretend to detect alpha masks. |
| `all` | Keep all ASTC candidates RGBA while retaining the same recompression-selected diagnostic path. |

Empty tokens are ignored. Do not insert spaces around comma-separated tokens.
Filtering cannot dynamically replace storage after a future upload or bind; the
trace exposes those events without changing live resources or queued snapshots.

## Host validation

`tests/port/test_astc_bc_p90.py:46` extracts and executes the production constructor's
storage-selection section with the real format table and host API doubles. Before
the fix it aborted on `Select(info).block_width==1` for a two-layer ASTC image.
It now covers 2/3/6/12 layers, 3D/1D, nonaligned bases, retained ordinary BC3/sRGB,
existing native BC protection, and latched skip rules in separate processes.

`tests/port/test_astc_bc_p90.py:164` executes the production conversion and sizing
functions with decoder/encoder doubles. An 8×8×4 four-mip volume needs 1172 RGBA
bytes or 320 BC3 bytes. Tests check each mip's offset and contents, including the
per-image RGBA override under a global BC3 setting. A mutation restoring the old
missing-depth offset caused the expected content assertion failure; production
files were not modified for that check. This tests layout, not ASTC decoding or
BC3 numerical quality. Additional cases test filters and worker/accounting wiring.

The drivers run ASan/UBSan and `-Wall -Wextra -Werror -Wshadow -Wconversion`.
`-Wno-sign-conversion` accommodates existing signed conversions in the imported
format/decoder header; shadowing and narrowing checks remain enabled. This is
not an MSVC/UWP build or a D3D12/Xbox sampler test.

Final focused command:

```sh
python3 -B - <<'PYTEST'
import sys, unittest
sys.path.insert(0, 'tests/port')
names = [
    'test_astc_bc_p90', 'test_totk_memory_p76', 'test_d3d12_async_astc',
    'test_d3d12_bc_arrays', 'test_d3d12_bc_small_mips',
    'test_d3d12_host_memory_budget', 'test_d3d12_staging_pressure',
    'test_texture_cache_fastpath', 'test_shader_image_writes',
    'test_d3d12_storage_images', 'test_d3d12_descriptor_setup',
]
result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromNames(names))
sys.exit(not result.wasSuccessful())
PYTEST
```

34 tests passed; no skips. Ruff check/format check passes for both changed Python
files. Changed C++ regions and the new header were formatted with clang-format;
`git diff --check` passes.

## Exact console proof run — pending, not performed

There is **no post-fix console run proving identical-looking output**. Console
access was explicitly prohibited. The following is the exact proposed comparison,
not an executed result:

Build this worktree with MSVC/UWP, preserving warnings-as-errors. Keep the same
TotK title/update/save, `resolution_setup=3`, native renderer, shader/async settings,
5 GiB grant and TAS in `r-totk-p76/inputs.txt`. Cold-relaunch for each variant;
keep all unrelated environment/settings entries identical. In `nxbox_env.txt`:

```ini
NXBOX_ASTC_TO_BC=1
NXBOX_ASTC_BC_DIAG=1
NXBOX_ASTC_BC_SKIP=
NXBOX_D3D12_GPU_ASTC=0
```

Capture through +230 seconds and the cave, then at least ten minutes/repeated area
changes. Save `diag.txt`, `eden.txt`, `crash.txt` and screenshots at the exact same
TAS points. Require `ASTC_STORAGE recompression=2`, `UPLOAD_POLICY astc=2`,
`ASTC_BC` creation records with `rgba=true` for excluded shapes, and `host=BC3_*`
for eligible 2D assets. Track warnings, allocation/device-loss errors and memory.

For the paired reference, change **only** `NXBOX_ASTC_TO_BC=0` and cold-relaunch.
The trace intentionally emits no recompression candidates for that reference.
Require the nested rectangles to be absent in the fixed BC-on capture and
scene-matched cave lighting/alpha to agree with RGBA, allowing BC quality loss.
Existing mismatched-moment p76 shots cannot provide this proof.

If rectangles persist, cold-relaunch with BC=1 and, in order, `SKIP=partial`,
then a narrower guest-format/size/address rule from the trace. `SKIP=all` (or
`alpha`, conservatively equivalent) is the all-RGBA control. Compare identical
TAS points and correlate any partial-upload, RT bind, reinterpret-view or copy
candidate with the logged GPU address. These filters permit a useful selective
console capture without another instrumentation build; they do not establish
causality merely because a class was excluded.
