# p94: synthetic gyro was filtered out by its mapping threshold

Fixed locally in the owner's fork, worktree `~/.local/share/nxbox/w-p78`, branch
`t/gyro2`, starting at `6e3663ff3`. No commit, push, `gh`, console access, UWP build,
or deployment was performed. Console acceptance remains pending because this
request explicitly prohibited console use.

## Root cause

`src/eden_uwp/gamepad.h:98` built both motion mappings with `threshold:1`.
That is an **absolute angular rate in turns/second**, not a unit multiplier:

1. `src/input_common/input_poller.cpp:1071` reads this property unchanged
   (its default is `0.007f`). `InputFromMotion::GetStatus`, at line 583, puts
   it in the gyro axis properties.
2. `src/hid_core/frontend/input_converter.cpp:138` copies a motion callback,
   then sanitizes its axes without changing units. The motion input properties
   have zero deadzone, unit range, and zero offset.
3. `src/hid_core/frontend/emulated_controller.cpp:1050` passes the property to
   `MotionInput::SetUserGyroThreshold`. `src/hid_core/frontend/motion_input.cpp:88`
   divides it by `ThresholdStandard` (`0.007`). Thus `1` becomes about `142.857`.
4. `src/hid_core/frontend/motion_input.cpp:54` zeros gyro below
   `gyro_threshold * user_gyro_threshold`. In Standard mode the cutoff becomes
   **1 turn/s**, while full right-stick aim produces only **0.5 turns/s**.
   Even two simultaneous full aim axes have magnitude about `0.707`, below 1.
   Loose mode makes the cutoff about `1.429`; Tight makes it about `0.286`.
   This explains why the bug can depend on the guest's drift mode.

The accelerometer still reaches orientation fusion. During pitch synthesis its
rotated gravity can change attitude even while gyro is zero. This explains the
small attitude changes in the supplied run; they are not evidence of delivered
gyro. The filter uses the previous callback's user threshold because SetGyroscope
precedes SetUserGyroThreshold. A neutral callback establishes the bad threshold
before the holds; moving that call would not fix the mapping's wrong value.

## Other suspects checked

- **No-pad mapping:** `XboxGamepad::Configure` (`gamepad.h:80`) runs before
  `Core::System` initialization (`game_session.cpp:907`). It configures every
  slot without consulting pad presence and assigns the same engine/port/pad/motion
  identifier to both motion entries. Remote player 1 and a real pad in slot 0
  both publish `nxbox`, zero GUID, port 0, pad 0, motion 0 (`gamepad.h:172`,
  `gamepad.h:226`). HID reads settings at `emulated_controller.cpp:100`, creates
  devices at line 206, and registers their callbacks at line 409.
- **AutoStyle:** `gamepad.h:354` changes connection/style, not motion parameters.
  `EmulatedController::SetNpadStyleIndex` (`emulated_controller.cpp:1718`) does
  not replace devices or mappings. `six_axis.cpp:157` uses motion entry 0 for
  Fullkey/Handheld and lines 165–174 select entries 0/1 for Joy-Con styles.
- **Callback deduplication:** `input_engine.cpp:96` always invokes
  `TriggerOnMotionChange`; line 372 dispatches matching motion callbacks even
  for identical samples. `InputFromMotion::OnChange` (`input_poller.cpp:596`)
  also dispatches unconditionally. The host regression verifies every hold poll.
- **Units/time:** TransformToMotion preserves turns/s; MotionInput converts to
  radians only for orientation integration (`motion_input.cpp:151`). The host
  regression checks nonzero, bounded microsecond timestamps after conversion.
  Zero time cannot explain zero `GetGyroscope()`: timestamp handling occurs
  after SetGyroscope.
- **Real pads:** synthesis and mappings are shared with remote input. The fix
  also restores full-stick gyro for real pads in Standard/Loose modes. It does
  not alter physical reading, gesture gating, slot selection, or player limits.

## Diff

The only production behavior change is the threshold in `gamepad.h:102`:

```diff
             // Both motion mappings use this player's sensor, including styles selected by
-            // AutoStyle. Keep HID's standard gyro deadband so its automatic bias estimate cannot
-            // drift at rest.
+            // AutoStyle. The threshold is an absolute rate in turns/s, not a multiplier:
+            // 1 would filter out even full-stick aim (0.5 turns/s). Keep the standard deadband.
             Common::ParamPackage motion{{"engine", "nxbox"},
                                         {"port", std::to_string(slot)},
                                         {"pad", "0"},
                                         {"motion", "0"},
-                                        {"threshold", "1"}};
+                                        {"threshold", "0.007"}};
```

This yields user multiplier 1, preserving HID's Standard/Loose/Tight deadbands
and bias handling. It does not disable filtering globally.

New files `tests/port/test_motion_delivery.py:14` and
`tests/port/motion_delivery.cpp:41` add the host regression. The Python harness
extracts the production ParamPackage initializer verbatim into a temporary header,
then compiles the actual engine, poller, converter, filter, and supporting common
sources. The driver serializes/reloads the mapping and registers two real motion
devices. Its callback invokes the real conversion/filter APIs in the same order
as EmulatedController::SetMotion. Logging is stubbed and assertion failures abort;
no motion implementation is mocked. WinRT enumeration, EmulatedController itself,
and guest shared memory are not compiled in this host test.

## Tests

Before changing the threshold, the compiled regression failed with SIGABRT on the
first yaw sample after rest: `assert(std::abs(gyro[2] + .5f) < .0001f)` (now
`tests/port/motion_delivery.cpp:100`). Raw gyro survived TransformToMotion but
post-filter gyro was zero. With the production fix it passes:

```sh
PYTHONPATH=tests/port python3 -c 'import test_motion_delivery; test_motion_delivery.test_motion_delivery()'
PYTHONPATH=tests/port python3 -c 'import test_stick_motion; test_stick_motion.test_stick_motion_math_and_gating()'
PYTHONPATH=tests/port python3 -m unittest -q test_recorder test_local_multiplayer
```

The new driver checks remote-equivalent player 1 input without a physical device,
pad-equivalent samples on ports 0–3, both motion entries, four seconds of yaw and
three seconds of pitch, retained +/-0.5 turns/s, callback count on repeated samples,
release to zero, a smaller 0.02 turns/s rate, rejection of 0.001 turns/s noise, and
isolation from unrelated ports. The existing synthesis/arbitration suite and all
four recorder/multiplayer tests pass.

The new driver and production MotionInput pass Clang `-Wall -Wextra -Wshadow
-Wconversion -Werror`. The full dependency build passes `-Wall -Wextra -Werror`,
with only unused-parameter and deprecated-declaration warnings suppressed for
existing code (including libc++ codecvt). Ruff lint/format, clang-format on the
new driver and changed production range, and `git diff --check` pass.
The production change introduces no variable declarations or numeric conversions;
MSVC C4456/C4457/C4459/C4244 flags remain unchanged. No MSVC/UWP build was run,
so those compiler results are not claimed.

## Console evidence and pending acceptance

The existing local artifact
`~/.local/share/nxbox/artifacts/compat-2026-10-18/r-totk-gyro4/eden.txt` contains
211 `NXBOX_MOTION_DIAG` lines, seven with `modifier_active=true`. All 211 have
`max_abs_gyro_rad_s=(0,0,0)`. This is the **before** run supplied with the task.
No post-fix console run exists from this work; host output is not console proof.

For the next authorized console session, use the unchanged
`~/.local/share/nxbox/artifacts/compat-2026-10-18/async-real/events-totk-gyro2.txt`
with `NXBOX_MOTION_DIAG=1`: +214 s L3 + RS-right for 4 s; +220 s L3 + RS-up for
3 s; +226 s L3 tap. Check delivered player-1 windows for nonzero
`max_abs_gyro_rad_s` during **both** holds, approximately pi rad/s on Z for right
and X for up in Standard mode. Release must return gyro to zero, holds must not
produce guest L3, and the tap must increment `l3_tapped_count` once. Repeat with
a real pad. This hardware acceptance is outstanding, not passed.
