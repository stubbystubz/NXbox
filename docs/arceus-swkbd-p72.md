# p72: Arceus firmware software keyboard presentation

## Finding and change

The shared applet framebuffer advertised a **byte stride as a pixel stride**.
`src/core/hle/service/vi/shared_buffer_manager.cpp:176` previously defined
`SharedBufferBlockLinearStride` as `1280 * 4` (5120).
`MakeGraphicBuffer`, at `shared_buffer_manager.cpp:257`, copies that value into
`NvGraphicBuffer::stride`. The correct value is **1280 pixels**.

The fix changes only that constant and adds its unit comment. It applies at the
shared VI buffer producer, before renderer selection. No keyboard-specific crop,
viewport, scaling, height, block-height, or blending workaround is needed.

The buffer path is:

- `src/core/hle/service/nvnflinger/hardware_composer.cpp:126` forwards buffer
  width, height and stride unchanged into `HwcLayer`.
- `src/core/hle/service/nvdrv/devices/nvdisp_disp0.cpp:85` forwards these fields
  into `Tegra::FramebufferConfig`.
- `src/video_core/texture_cache/texture_cache.h:1553` compares cached image width
  with `config.stride`. A genuine 1280-wide applet image cannot match 5120.
- On a miss, `src/video_core/renderer_d3d12/d3d12_texture_cache.cpp:1780` sizes
  the guest read using `config.stride`, then at line 1788 unswizzles using that
  value as the pixel width. The decoder applies bytes per pixel itself
  (`src/video_core/textures/decoders.cpp:185`). The uploaded visible row is the
  first 1280 pixels of this incorrectly decoded 5120-pixel-wide surface.

For RGBA8, log2 block height 4 means 128 scanlines per block row. The correct
block-row stride is `1280 * 4 * 128 = 655360` bytes; the broken one is 2621440.
Thus the broken reader's row 128 addresses the real image's row 512. A 720p
applet scaled to 1080p puts those decoded rows 128–256 at output y=192–384.
This precisely matches the observed displaced keyboard band. The incorrect
layout also makes the tiled read **15728640 bytes** instead of **3932160 bytes**
per pool slot, reading other slots and potentially beyond the pool. The bottom
corruption is consistent with that read; the exact garbage values are not
predictable from the screenshot.

## Console evidence and limits

Sources inspected in
`~/.local/share/nxbox/artifacts/compat-2026-10-18/r-arceus3/`:

- `shots/shot-0180s.png`: prompt at the top, keyboard bottom rows at approximately
  y=192–384, black middle, corrupted bottom y=960–1080. The 192-pixel boundary
  matches one 128-line GOB block row at 1.5x output scale.
- `eden.txt:576–579`: `GetSystemSharedLayerHandle`, then `OpenSharedLayer` and
  `ConnectSharedLayer` with `layer_id=2`, establishing use of the shared applet
  layer path containing the bad stride.
- `eden.txt:581–582,614,1102`: `PopInData` and `PushInteractiveOutData`, confirming
  the firmware applet executes.
- `diag.txt:132–135` (also `crash.txt:98–101`, `eden.txt:233–236`): a single
  1920x1080/stride=1920 RGBA8 texture-cache hit, block=(0,4,0), at 19.15 seconds.
  The applet opens around 75 seconds. These lines describe the initial game
  framebuffer, **not the keyboard framebuffer**.

The unit error and its address mapping are proven by the shared producer code
and host regression. The logs do not prove which cache candidate or fallback
was used for the keyboard at 180 seconds. No Xbox rerun or MSVC/UWP build was
performed here; visual acceptance on console remains pending. Opt-in tracing
below closes that runtime evidence gap.

## Other presentation checks

The logical shared image is 1280x720; its allocation is 1280x768
(`shared_buffer_manager.cpp:173–192`). With block_y=4, 720 rounds to 768, so the
48 padded rows are valid allocation padding, not a destination height. Leave
these dimensions and block_y unchanged.

`FramebufferConfig` contains a source crop and transform flags, blending, and
stack mask, but no per-layer destination position or size.
`src/core/hle/service/vi/system_display_service.cpp:24–30` leaves the position
and size setters unimplemented. Vulkan does not honor hidden position/scale
fields that D3D12 omits: Vulkan `present/layer.cpp:298–309` draws every layer to
`layout.screen`, as D3D12 `renderer_d3d12.cpp:364–397` does. The keyboard's
location inside its full-screen buffer must be decoded correctly.

Both presenters apply source crops: Vulkan uses `Tegra::NormalizeCrop`
(`present/layer.cpp:134`); D3D12 normalizes the crop against the selected texture
and mip, accounting for resolution scaling (`renderer_d3d12.cpp:394–422`).
`src/video_core/present.h` selects filters, not layer geometry. No evidence
supports changing these paths.

Vulkan's fallback is a useful distinction: it calculates tiled size with
`framebuffer.stride`, but unswizzles with `framebuffer.width`
(`renderer_vulkan/present/layer.cpp:347–352`). This can mask the bad pixel
stride in its visible image. D3D12 uses stride for both, so it exposes the
producer error. Neither backend should receive a byte stride.

Hardware composition sorts layers by ascending z before submission
(`hardware_composer.cpp:159–161`). Vulkan selects opaque/premultiplied/coverage
pipelines in `present/window_adapt_pass.cpp:53–69`; D3D12 does the same in
`renderer_d3d12.cpp:443–449` (and passes the equivalent blend mode to FSR).
The compositor retains each visible layer with an available buffer. The screenshot alone cannot
prove their actual z, crop, or blend values; tracing records those values.

## Opt-in console diagnostic

Set **`NXBOX_LAYER_DIAG=1` before starting the emulator process** and reproduce
name entry. Other values and an unset variable leave the extra tracing off.

`nvdisp_disp0.cpp:71–113` emits an `NXBOX_LAYER` frame header and one record for
every submitted layer, in sorted draw order, including handle, base address,
offset, effective buffer address, width/height, pixel stride, format, crop,
effective position `(0,0)` / full-screen destination, z, blending, transform,
and layer-stack mask. The position reflects current compositor behavior;
guest position/size setters are unimplemented.

`d3d12_rasterizer.cpp:4429–4456` also enables the existing `D3D12_DISPLAY_MISS`
lookup/candidate records and `D3D12_DISPLAY` source records on every lookup,
rather than only the first one. This supplies actual image height, block_y,
subresource, rejection reason, and texture-cache versus guest-memory source
for the applet. Shared-layer submissions have a frame counter; D3D12 records
can be matched by effective buffer address and log order. Screenshot capture
can cause additional D3D12 lookups. Use the existing emulator diagnostic sinks;
no separate per-frame file writer was introduced. Disable tracing after capture.

Expected applet configuration after this fix: 1280x720, stride_pixels=1280,
RGBA8888; a GPU image may have padded height 768. If the fallback is used, its
read stays within one 3932160-byte pool slot. Confirm every keyboard row is
visible at the bottom and the garbage band is gone on the next Xbox run.

## Host validation

`tests/port/test_arceus_swkbd.py` adds one unittest. It compiles the production
shared-buffer constants and `MakeGraphicBuffer` with a small buffer-queue facade,
and the production `GetGOBOffset` decoder function. It verifies both presentation
buffers' offsets and all visible rows at both horizontal edges against the
guest's 1280-pixel layout, including slot bounds. It also checks the old
row-128/row-512 address equivalence explaining the screenshot.

The driver compiles with `-Wall -Wextra -Wconversion -Wshadow -Werror`;
`-Wno-unused-parameter` permits the existing decoder's unused `height` argument.
Restoring only the old stride expression in the extracted production source
makes the executable fail at `display_offset == guest_offset`. The fixed source
passes. This before/after check does not mutate the worktree.

Presentation checks on the final source:

```text
python3 -m unittest discover -s tests/port -p 'test_arceus_swkbd.py'
Ran 1 test — OK

PYTHONPATH=tests/port python3 -m unittest test_arceus_swkbd \
  test_present_image_selection test_d3d12_capture test_d3d12_fsr \
  test_d3d12_window_origin test_d3d12_resolution test_d3d12_output_policy \
  test_frame_capture test_frame_pacing test_vi_vblank
Ran 38 tests in 44.602s — OK (skipped=1)
```

The skipped check requires DXC, which is unavailable on this host. Changed C++
ranges were formatted with the project's clang-format configuration; the new Python test
was formatted with Ruff. `git diff --check` passes.

Full discovery was run with the requested command:

```text
python3 -m unittest discover -s tests/port -p 'test_*.py'
Ran 986 tests in 740.126s
FAILED (errors=3, skipped=46)
```

This run started before the new test driver was finalized. Its three errors were:

1. The initial Arceus driver compilation treated unused extracted constants as
   errors. The final driver explicitly checks those constants; both the isolated
   regression and the 38-test presentation rerun above pass.
2. `test_button_layout` failed to import because this Python environment lacks
   `fontTools`.
3. `test_xbox_button_icons` failed for the same missing dependency.

The already-installed FontTools package was located in the local uv cache,
without installing anything or changing project dependencies. All three affected
modules were then loaded successfully on the final source and the Arceus
unittest reran:

```text
PYTHONPATH=tests/port:/Users/miguel/.cache/uv/archive-v0/JNxAtyjfuxFcSQC1 \
  python3 -m unittest test_arceus_swkbd test_button_layout test_xbox_button_icons
Ran 1 test in 0.531s

OK
```

The two font modules contain pytest-style functions; unittest imports them but
does not execute those functions. The recheck resolves their import errors,
not a claim of running their font assertions. The full discovery command was
not repeated; the corrected Arceus test, module imports, and the whole
presentation group were rechecked instead. Full-run log:
`/tmp/p72-port-tests.log`; presentation rerun: `/tmp/p72-presentation-tests.log`;
failed-module rerun: `/tmp/p72-recheck-tests.log`.

All changes remain in this worktree. No commit or push was made.
