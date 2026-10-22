# p89: L3 arbitration and guest motion diagnostics

Implemented locally in the owner's fork, worktree `~/.local/share/nxbox/w-p78`,
branch `p78/motion`. No commit, push, network access, console access, or deployment
was performed.

## Evidence and scope

The supplied console observation from
`~/.local/share/nxbox/artifacts/compat-2026-10-18/r-totk-gyro` (rc/next + p78,
TotK) showed the crouching tutorial while holding L3 and moving the right stick.
That demonstrates a guest L3 action; screenshots do not establish whether motion
reached guest shared memory. This change adds explicit gesture arbitration and
instrumentation at guest HID delivery. It does not claim a new console result or
establish why the earlier build's sensor/modifier gate failed during that run.

## L3 behavior

With an eligible running six-axis sensor, L3 starts pending and is withheld from
the guest from its first down poll:

- Release before 250 ms, with all four stick axes staying within the inclusive
  +/-0.15 deadzone: produce one L3 pulse starting on release, lasting 40 ms
  (cleared on the first poll at or after its deadline). No motion is generated.
- Hold to 250 ms, or move any axis outside the deadzone while held: latch the
  motion modifier until release. The guest receives no L3 press for that hold,
  including on release. Returning the sticks to center does not turn it into a tap.
- The pending phase retains ordinary sticks; the latched modifier consumes both
  sticks and uses the existing p78 motion mapping. Release resets synthesis.
- Without an eligible sensor, L3 passes through immediately with its original down
  and up state. Other players retain this behavior. The existing player 1 / Handheld,
  pad presence, controller support, and motion-setting eligibility rules remain.

Overlay/HOME suppression cancels a pending tap and pulse. A new press cancels any
previous pulse so a rapid motion gesture cannot inherit it. Device replacement
resets gesture state. Starting a sensor during an already held passthrough L3
cancels tap recognition until release; stopping the sensor restores passthrough
on that poll. Arbitration uses steady time, independently of the synthesis dt
clamp, and the same combined physical/remote/script L3 source as before.

`StickMotionSample::guest_l3` is now the sole L3 result used by the frontend's
`SetButton` routing, after merging physical/remote/script sources. The shared
production helper is exercised by the host driver.

## NXBOX_MOTION_DIAG=1

Set this environment variable before starting the process. Its exact value must
be `1`; it is read once. Output uses `LOG_WARNING(Service_HID, ...)`, matching the
existing HID diagnostic convention and surviving the normal warning log filter.
Lines go to the normal Eden log, prefixed `NXBOX_MOTION_DIAG`.

Each observed validated handle has an independent one-second steady-clock window,
keyed by `npad`, numeric `style`, and numeric `device`. Repeated Start calls do not
reset the window. Stopped handles remain observable until the HID resource is
released, including independent JoyDual left/right handles. A line contains:

- `sensor_active`: this exact handle is in the Start/Stop request set.
- `delivered`: the handle matches the current style and motion is enabled in this
  guest shared-memory update.
- `modifier_active`: the frontend has latched the motion gesture.
- `l3_tapped_count`: cumulative recognized taps for that physical slot since its
  last device/gesture reset; shared between player 1 and Handheld mappings.
- `gyro_rad_s=(x,y,z)`: the current gyro actually written by HID, converted from
  turns/s to rad/s by multiplying by 2*pi.
- `max_abs_gyro_rad_s=(x,y,z)`: independent absolute axis peaks over the window.
- `attitude_delta_rad`: the magnitude of the relative orientation change between
  consecutive delivered samples, in radians, computed from the guest orientation
  matrices using `2*asin(min(||R_current-R_previous||_F/sqrt(8),1))`.
- `max_abs_attitude_delta_rad`: the largest such change over the window. Identical
  matrices give exactly zero; the first delivered sample after inactivity has no
  previous attitude and contributes zero delta.

The diagnostics sample the final six-axis states immediately after their guest
LIFO writes, after MotionInput filtering and orientation fusion. This measures
HID delivery rather than only the frontend's intended synthetic rates. Peaks
preserve short motion even if the sticks return to rest before the log line.
Gesture metadata crosses the polling/service threads through atomics; it is
informational and can lag the motion write by a poll. It is not a transactional
snapshot with the HID sample.

Reporting requires the existing active, connected, six-axis-enabled guest
shared-memory update path. Handles never observed through Start/Stop have no
line; updates paused by disconnect or guest resource disable have no line until
updates resume. Release clears handle windows. A logged nonzero delivered peak
proves a nonzero value was written for that handle; it cannot prove that TotK
read or used that value. No console evidence was collected here.

## Validation

Passed locally:

```sh
PYTHONPATH=tests/port python3 -c 'import test_stick_motion; test_stick_motion.test_stick_motion_math_and_gating()'
PYTHONPATH=tests/port python3 -m unittest -q test_recorder test_local_multiplayer
```

The motion driver compiles the production arbitration, synthesis, and diagnostic
accumulator with `clang++ -std=c++17 -Wall -Wextra -Wshadow -Wconversion -Werror`,
then executes it. Coverage includes:

- One guest L3 rising edge for a 249 ms tap, with no L3 during the down interval,
  an expiring release pulse, and a single increment of the tap counter.
- Hold plus stick: zero guest L3 throughout press/hold/release, nonzero gyro, and
  continued modifier ownership after recentering. Each of the four axes is tested.
- The exact 250 ms boundary; inclusive deadzone; no-sensor passthrough; suppression;
  sensor start/stop during a hold; and rapid tap-to-motion pulse cancellation.
- Existing aim, tilt, rest, finite dt, unit gravity, release/reset, and View behavior.
- Diagnostic rad/s conversion, known 90-degree attitude delta, peak retention
  through rest, one-second cadence, window reset, and inactive/reactivated delivery.

The recorder and multiplayer suites ran **4 passing tests**, including their host
C++ drivers. Ruff lint and formatting checks passed for the changed Python test;
changed C++ ranges and the new header were formatted with clang-format.
`git diff --check` passed.

MSVC C4456/C4457/C4459 remain errors through the existing source build flags;
C4244 is already an error for hid_core. The UWP executable now explicitly enables
all four as errors too. No MSVC/UWP build was run on this Mac; the strict Clang host
checks are not a substitute for that build. Console behavior remains unverified.
