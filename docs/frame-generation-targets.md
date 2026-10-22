# Frame generation targets

Frame generation is an explicit user preference for every title. Global and title
files accept `frame_gen=off|2x|4x|auto` (the existing `frame_generation` key and
numeric `0|1|2|3|4` spellings remain supported; `1` means 2x and `3` means auto).
The library's global Frame generation row cycles Off, 2x, 4x, Auto, Default.
Each game's Frame generation action opens the existing settings sheet style with
Default, Off, 2x, 4x and Auto choices. Changes apply on the next game launch.
Default removes the stored key; explicit Off is a real override.

Precedence is **per-game user choice > global user choice > built-in preset**.
The small constexpr table lives in `TitleSettings`: Odyssey
`0100000000010000` uses auto; MK8D `0100152000022000` uses 2x; other titles are off.
It is a default table, not an eligibility list. Measurements supplied for Series X
at 60 Hz: MK8D 28–29 real fps with even 16.68 ms 2x gaps; Odyssey ~24 real fps
with alternating 16.7/33.4 ms 2x gaps; BotW ~13 real fps with 33/50 ms 2x gaps.

## Selection and pacing

Auto evaluates output candidates 120, 60, 40, 30 in descending order, requiring
an integer display divisor and 5% producer headroom. Each output prefers 2x over
4x when both sustain it. At 120 Hz it can select 120 (2x or 4x), 60 (2x or 4x),
40 (2x or 4x), or 30 (2x or 4x). At 60 Hz it selects 60 (2x) or
30 (2x). The explicit 60 Hz tradeoff favors 30/2 over 60/4: both require the
same real cadence, while 2x synthesizes fewer images. A native-rate ceiling may therefore require
4x for the 5% margin (a measured 60 fps has no margin for 120/2).
59.94 and 119.88 Hz use the corresponding 60/120 family. Unknown/unsupported
refresh rates do not acquire a lock. Below the minimum sustainable rate auto
presents real frames without interpolation. Fixed modes retain p59 behavior,
including the conservative 4x-to-2x fallback below 119 Hz.

At ~24 real fps Odyssey selects **30 FG, 2x, 15 real fps** on 60 Hz and
**60 FG, 4x, 15 real fps** on 120 Hz. At 60 Hz a diagnostic explicitly explains
that 40 FG requires 120 Hz output. Choosing 30/2 instead of 60/4 keeps one
synthetic image per real frame instead of three. Both require 15 real fps and a
66.7 ms history interval, so 60/4 does not reduce the guest input latency.
This explicit 60 Hz exception favors fewer artifacts over more output samples.
At 120 Hz the general highest-output rule applies: 40 FG is another sustainable
candidate, but ranks below sustainable 60 FG; at ~11 real fps auto selects 40/4.

The estimator uses two-second windows and excludes this presenter's own waits
from producer work. Window duration and promotion stability use wall time.
Step-down takes one valid window; a promotion needs at least five seconds of
consecutive matching windows (normally six seconds). Stalls above 200 ms reset
pending observations and promotion stability. Present sync intervals enforce the
output divisor on **both** generated and real images: e.g. 60 FG at 120 Hz uses
interval 2, 40 FG uses interval 3. Each group contains multiplier-1 evenly spaced
interpolations followed by the real image, so producer backpressure follows the
output/multiplier cadence. Auto disables the fixed mode's periodic real-only
probe because that would intentionally break the lock. Target changes reset
history. Timing, scene-cut, history, resource-pressure and GPU-budget rejection
remain active; rejected interpolation can cause visible gaps and is not reported
as successful smoothing. GPU budgets use the sampled real-frame interval.

The overlay shows the chosen lock, e.g. `60 FG / 15 FPS`; the second number is
the measured guest rate, not an invented generated FPS count. Lock labels describe
the chosen schedule; verify actual displayed cadence from presentation statistics.

## Switches and rollback

Put overrides in LocalState `nxbox_env.txt` (or the existing process environment):

- `NXBOX_FRAME_GEN=0`: disable all generation, presets and target pacing.
- `NXBOX_FRAME_GEN=2` or `4`: force the existing fixed mode and bypass auto.
- `NXBOX_FRAME_GEN=auto` (or `3`): force auto regardless of saved choices.
- `NXBOX_SHOW_FPS=1`: show the guest/output overlay.
- `NXBOX_D3D12_FRAME_PROFILE=1`: record actual DXGI presentation gaps when
  statistics are available, separately from CPU Present-return gaps.

`NXBOX_FRAME_PACING=0` disables the independent legacy pacing preference;
it does not disable the auto lock. Use `NXBOX_FRAME_GEN=0` or a fixed mode for
that. Removing overrides restores saved preferences and then built-in defaults.
No dependencies or persistent service are added.

## Validation and exact console checks

Host acceptance command, from this worktree only. Python must be able to import
`fontTools`, an existing button-font test dependency. This host's cached copy was
made visible through `PYTHONPATH`; no dependency was installed or added to the
project:

```sh
scratch=$(mktemp -d /tmp/p65-host.XXXXXX)
cd tests/port
TMPDIR="$scratch" python3 -m unittest -q
```

Host tests execute production selection, hysteresis, preset lookup, file
round-trips, restore-to-default and override precedence. They also preserve the
fixed-mode policy and generated-image ordering checks. The frozen Draw hash was
updated to the existing p59 HEAD value; this change does not edit Draw. The older
FSR UI wiring assertion now checks p59's existing ReadUpscaler/SaveUpscaler calls.
Host execution cannot establish Xbox visual quality or actual display timing.

Run these checks on Series X with a D3D12 build containing this change; console
checks have **not** been executed as part of this host implementation:

1. Clear global and Odyssey overrides to Default. Remove `NXBOX_FRAME_GEN` from
   `nxbox_env.txt`; enable FPS and frame profiling. Set the Xbox's actual display
   output to **60 Hz**, then relaunch Odyssey `0100000000010000`.
2. Use the same repeatable gameplay/camera path that measured ~24 real fps.
   Wait at least ten seconds, capture a 30-second trace. Expect a 60 Hz refresh
   diagnostic, the message that 40 FG requires 120 Hz, `lock=30 multiplier=2`,
   and `30 FG` in the overlay. Real cadence should converge near 15 fps;
   displayed gaps should cluster around 33.37 ms, rather than 16.7/33.4 ms.
   Inspect cuts, HUD edges, disocclusions and input response. Record timing and
   GPU-budget rejections instead of treating a target label as proof of smoothness.
3. Set actual Xbox output to **120 Hz**, relaunch, repeat the identical path.
   Expect refresh near 119.88/120, `lock=60 multiplier=4`, real ~15 fps and
   interval-2 output gaps near 16.68 ms. 40 is admissible here but is lower than
   sustainable 60/4; a sufficiently slower ~11 fps segment should select
   `lock=40 multiplier=4`, interval 3 and ~25 ms output gaps.
4. In both output modes, stress a slow segment for >2 seconds, then return to
   the stable path for >6 seconds: verify fast demotion, delayed promotion and
   no repeated flip-flop. With unsupported/disjoint DXGI statistics, mark cadence
   unavailable; do not substitute CPU-return gaps for measured display gaps.
5. Verify precedence: global Off disables Odyssey's preset; per-game Auto
   overrides global Off; per-game Off overrides global Auto; Default restores
   inheritance. An unlisted title must accept all four modes. MK8D with both
   settings at Default remains fixed 2x. `NXBOX_FRAME_GEN=0` overrides all of them.
6. Relaunch with fixed 2x and compare against the supplied p59 uneven Odyssey
   baseline. Remove profiling overrides after collecting the evidence.
