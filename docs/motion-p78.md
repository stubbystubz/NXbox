# p78: automatic synthetic Xbox motion

Implemented on `p78/motion`, based on `rc/next`. No commit, push, network access,
console access, or console deployment was performed.

## Detection and lifetime

`IHidServer::StartSixAxisSensor` and `StopSixAxisSensor` already forward the
validated handle to `SixAxis::SetSixAxisEnabled`. Requests now start inactive and
are tracked as a set of `(npad style, device index)` keys within each controller's
six-axis parameters. The controller supplies the npad ID part of the key. Repeated
Start calls are idempotent; Stop removes only that handle. In particular, stopping
one half of JoyDual leaves the other half active. HID shared-memory output uses
the corresponding sensor's activation state rather than a controller-wide switch.

SixAxis publishes whether any handle for that controller remains active through an
atomic flag on `EmulatedController`. The existing HID resource mutex serializes
Start/Stop with shared-memory updates. Release/destruction clears requests and the
published flags, so a later game does not inherit sensor demand.

Xbox polling reads player 1 and Handheld demand (both use the player 1 physical pad
mapping). Only slot 0 synthesizes motion, and only with a present, allowed pad,
motion enabled in the existing core settings, no guest-input suppression/HOME
chord, and L3 held. There is no title list or game-specific detection. A game that
starts its sensor at boot still receives ordinary sticks until L3 is held.

## Mapping

- L3 is the fixed modifier in this patch. While active, both guest stick axes and
  the guest L3 button are consumed. Other buttons remain available.
- Right Y supplies pitch angular velocity; right X supplies negative world-up yaw.
  Maximum aim speed is **0.5 turns/s (180 degrees/s)** on each axis.
- Left X supplies roll and left Y pitch, each targeting **±60 degrees**. Tilt slews
  at at most **1 turn/s (360 degrees/s)** per axis, including reversal.
- Each axis has a **0.15 deadzone**, linearly rescaled over the remaining range and
  clamped to unit magnitude.
- Acceleration is a rotated **1 g** gravity vector, initially **(0, 0, -1)**. Pitch
  aim is integrated; tilt pitch/roll rotate gravity, and yaw is expressed around
  world up in controller coordinates. Gyro includes tilt angular velocity and
  uses Eden's existing turns/second sensor path, which derives rotation/orientation.
- Poll intervals use steady time, initially 1/60 s, clamped to **1 microsecond–100 ms**
  to stay within MotionInput's accepted interval. Pitch accumulation wraps at 2π.
- Releasing L3, stopping the last sensor, or suppressing guest input resets the
  synthesis immediately to level gravity and zero gyro, with no return-motion tail.
  Stick routing resumes on that poll. Device replacement also resets synthesis.

The previous View + right-stick tilt is replaced. View tap/Minus, View + Menu HOME,
and View + right-click recording retain independent gesture handling. Players 2–4
receive level synthetic samples and keep ordinary sticks.

No new UI text or toggle was added: a persistent Settings toggle would require
settings storage and UI wiring beyond the optional trivial change. The feature is
on by default, gated by guest sensor demand and L3. Physical sensor support remains
out of scope.

## Files changed

- `src/core/hle/service/hid/hid_server.cpp`: document the existing Start dispatch.
- `src/hid_core/resources/six_axis/six_axis.h` and `.cpp`: per-handle activation,
  independent shared-memory gates, request publication, and lifetime cleanup.
- `src/hid_core/frontend/emulated_controller.h`: atomic service-to-input demand flag.
- `src/eden_uwp/stick_motion.h`: portable synthesis and independent View gesture.
- `src/eden_uwp/gamepad.h`: player 1 demand/modifier gating, motion delivery, stick
  consumption, and preservation of recording/HOME handling.
- `tests/port/test_stick_motion.py`: compile the production math in a C++17 host
  driver and exercise gating, rest, aim, tilt, unit gravity, deadzone, release,
  reactivation, repeated timestamps, long stalls, and View gestures.
- `tests/port/test_recorder.py`: test the extracted View gesture with recording.
- `docs/motion-p78.md`: this report.

## Validation performed

Passed locally:

```sh
PYTHONPATH=tests/port python3 -c 'import test_stick_motion; test_stick_motion.test_stick_motion_math_and_gating()'
PYTHONPATH=tests/port python3 -m unittest -q test_recorder test_local_multiplayer
```

The first command compiled and executed the actual portable math with
`clang++ -std=c++17 -Wall -Wextra -Wshadow -Wconversion -Werror`. The second ran
**4 passing regression tests**, including their C++ host drivers. Ruff formatting
checks passed for both changed Python files; changed C++ ranges were formatted with
clang-format using the repository configuration. `git diff --check` passed.

The default Python lacks pytest, so the plain test function and unittest suites
were invoked directly, without downloading dependencies. No UWP/MSVC build was
run on this Mac. Explicit casts and float constants avoid introducing implicit
narrowing; the host math driver also checks shadowing and conversions as errors.
MSVC integration and game compatibility remain unverified.

## Console check plan (not executed)

1. Build with the normal UWP/MSVC warning-as-error configuration, including
   C4456/C4457/C4459 and C4244. Install only during a separately authorized console
   session.
2. In TotK, enable the game's motion aiming, draw a bow or open the scope, then
   hold L3. Right stick should aim continuously; left stick should tilt. Check both
   directions and tune axis signs/sensitivity if needed. Release L3: ordinary
   camera/movement and the normal L3 action should resume without synthetic gyro.
3. In Splatoon gyro aiming or Super Mario Odyssey motion interactions, repeat the
   same checks. A game that keeps the sensor running from boot must retain normal
   camera controls whenever L3 is released. This implementation is generic, but
   these games have not been verified on hardware.
4. Observe HID Start/Stop for player 1, Handheld, and both JoyDual handles. Start
   both halves, stop one, and verify L3 motion continues through the other; stop
   the last and verify ordinary sticks even while L3 remains held. Repeat Start
   on one handle to confirm it does not require multiple Stops.
5. Test a game/scene with no Start request: L3 and both sticks must behave normally.
   Test HOME/overlay suppression, unplug/reconnect, a second pad, and launching a
   different game. Player 2 must not generate motion or consume player 1 sticks.
6. Check View tap/Minus, View + Menu HOME, and View + right-click recording after
   motion use. Verify there is no stale input or sensor activation across games.
