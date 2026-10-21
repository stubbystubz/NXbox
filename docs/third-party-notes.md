# Third-party implementation notes

## Jadyx renderer study

Credit: **Jadyx by JulianDr14**, <https://github.com/JulianDr14/jadyx>,
reference commit **22c67b4**, GPL-3.0 (source SPDX: GPL-3.0-or-later).

The submission-local uniform upload stream in
`src/video_core/renderer_d3d12/d3d12_uniform_stream.h` adapts the algorithm in
Jadyx's `d3d12_uniform_stream.h`. Its original
`Copyright 2026 Jadyx Project` and SPDX license header are retained, alongside
NXbox's adaptation credit. NXbox retains its own resource leases, ring allocator,
padding, error handling, and command recording lifetime rules.

Precomputed graphics-stage descriptor counts are independently implemented from
the immutable metadata idea documented in Jadyx's `xbox_draw_preparation.md` and
`d3d12_stage_bindings.*`. No Jadyx branding, product names, or UI text is used in
NXbox's interface. See [the comparison](jadyx-comparison.md) for scope and evidence.
