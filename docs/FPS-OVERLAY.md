# In-game FPS overlay

Open the Select + Start menu, then **Graphics → Show FPS** (**Gráficos → Mostrar
FPS** in Portuguese). Press A to toggle it. The change takes effect during the
current game. The default is off.

The preference is stored as `show_fps=true` or `show_fps=false` in
`LocalState/eden_settings.txt`, alongside the other frontend preferences. Saving
preserves unrelated settings and comments. It is global, not a per-title setting.
A failed save leaves the preference unchanged and shows the existing save-error
toast.

Set `NXBOX_SHOW_FPS=1` before launching NXbox to force the overlay on for test runs
and external screenshots. Only the exact value `1` forces it on. The override
does not rewrite the saved preference; turning the menu setting off while forced
keeps the overlay visible until the next launch without the override.

## Measurement

`hardware_composer.cpp` counts compositions that acquire a new visible,
non-overlay guest buffer, once per composition even when several game layers
advance together. Reused images and guest overlay-only updates do not increment
this counter. The D3D12 presenter samples its cumulative count in non-overlapping
windows of at least one second using `steady_clock`. FPS is the count delta divided
by the actual elapsed seconds; frame time is `1000 / FPS`, the average wall time
per guest image, including pacing. It is not GPU execution time or display refresh
rate. A 30 FPS game on a 60 Hz display therefore reports approximately
`30 FPS  33.3 ms`.

The first enabled window starts at zero. Enabling again starts a fresh window;
time spent disabled is excluded. A window with no new game images reads
`0 FPS  0.0 ms` when presentation next runs. If the game stops presenting entirely,
the last overlay image remains on screen along with the last game image.

## Rendering and cost

The native D3D12 presentation path draws the overlay after all game layers and
before the swapchain buffer transitions to `PRESENT`. It works with both the
ordinary compositor and the optional FSR presenter. It uses one triangle, a
premultiplied-alpha pipeline, root constants, and original embedded 4×7 monospace
bitmaps. There are no font files, textures, descriptors, vertex buffers, D2D calls,
interop, or dependencies to upload. White text sits in a 72% black capsule, inset
24 pixels at 1080p with a 21-pixel glyph height, scaling with display resolution.
The worst-case white game background still gives text contrast above 8:1.

When off, only relaxed atomic flag checks remain: no counting increments, clock
reads, string formatting, allocations, shader compilation, pipeline creation, or
GPU commands. Pipeline creation is lazy on the first enabled display frame. The
pipeline stays cached after disabling, and teardown drains outstanding GPU work
before releasing it. The sampler resets across toggle changes.

The setting applies to native DirectX presentation. The OpenGL compatibility
renderer does not draw this overlay.

`Capture()` renders to its own BGRA image and never draws the counter. In-game
screenshots requested through the renderer remain clean. Console/system captures
of the final swapchain include the counter, which is intentional for testing.

## Validation

Run `python3 -m unittest discover -s tests/port`. `test_fps_overlay.py` compiles and
runs a portable C++ driver for persistence, unrelated-setting preservation, save
failure, default-off behavior, environment precedence, disabled draw dispatch,
guest counting, sampling, and toggle resets. Source integration checks pin the
live menu action, translations, guest-image counting point, lazy GPU creation,
and display-only draw placement. Existing settings-category tests check that the
new row remains reachable exactly once.

Host validation on this worktree: 971 tests run, 925 passed, 44 skipped, and the
two pre-existing `fontTools` import errors in `test_button_layout` and
`test_xbox_button_icons`. All three new FPS tests passed; the new Python test file
also passes Ruff, and `git diff --check` is clean.

A Windows/Xbox build and device check are still needed to validate DXIL/PSO
creation and TV appearance. Check a 30 FPS game, toggle during gameplay, restart
to check persistence, force with the environment, compare a renderer screenshot
with a system capture, and repeat with FSR enabled and disabled.
