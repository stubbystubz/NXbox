# Local multiplayer on Xbox

NXbox polls Windows.Gaming.Input and maps up to four Xbox gamepads to Switch
players 1–4. Each player starts as a Pro Controller, with the selected face-button
layout, both sticks, shoulder buttons, digital triggers, and View + right-stick
motion emulation. Each player has independent sensor and gesture state.

A shared device-to-slot table preserves every connected controller's slot when
Windows changes enumeration order or another pad disconnects. Missing devices
release their slots; newly seen devices take the lowest free slot. A controller
that reconnects is a new arrival and may occupy a different free slot. There is
no persistent hardware identity across app restarts. Devices beyond the limit
wait until a slot becomes free.

Players 2–4 connect and disconnect with their physical pads. Player 1 retains the
existing virtual connection without a pad so remote input keeps working and games
do not repeatedly open controller-support applets. Guest-supported player IDs
and styles still take precedence over all connections. The existing automatic
style selection prefers Pro, then paired Joy-Con, then handheld; handheld is
available only to player 1, when the guest allows the handheld ID. Other players
never use the handheld style. The frontend observes the guest's supported Npad ID
list through HIDCore rather than repeatedly forcing excluded players to connect.

Only slot 0 controls View + Menu HOME actions, overlays, and shader-preload skip.
Remote keys and input scripts feed player 1 only. An overlay suppresses guest
input only for player 1; other players continue sending their own pad input.
View + Menu on another controller does not activate or consume the HOME combo.

## Player limit

In the graphics/settings screen beside the other input options, select **Local
players** to cycle 1 → 2 → 3 → 4 → 1. The choice applies on the next game launch.
It is stored in the app's local `eden_settings.txt`:

```ini
max_players=4
```

The default is 4. Numeric values are clamped to 1–4; invalid text uses 4.
`NXBOX_MAX_PLAYERS` overrides the file and uses the same parsing rules, including
when displaying the setting at startup. An environment override continues to
win over a saved UI choice. Set `max_players=1` to restore a single guest player.
No dependencies or background services are required.

## Host validation

From `tests/port`, run the existing host suite with a writable scratch directory:

```sh
TMPDIR=/path/to/scratch python3 -m unittest -q
```

`test_local_multiplayer.py` exercises initial order, enumeration reorder,
disconnect holes, reconnect order, excess devices, player-limit changes and
clamping, settings preservation, environment precedence, and HOME isolation.
Source integration checks cover per-player routing and guest-limit guards.
Host tests cannot validate Windows.Gaming.Input or Xbox UWP execution.

## Two-pad console checklist

1. Start with two pads connected. Launch Mario Party Superstars, Smash, or Mario
   Kart 8 Deluxe and join as two players. Verify face buttons in both layout
   modes, sticks, D-pad, shoulders, triggers, and View + right-stick motion on
   each pad. Confirm input from one pad never moves the other player.
2. Start with one pad, then connect the second in the game's join screen. Remove
   and reconnect pad 2; player 1 must keep working. Remove pad 1 while pad 2 stays
   connected: pad 2 must remain player 2. Reconnect pad 1 into the free slot.
3. On pad 2, press View + Menu: no NXbox overlay or HOME action should occur.
   On pad 1, verify the existing single/double combo, overlay navigation,
   dismissal, and guest suppression. Pad 2 should continue driving player 2
   while the overlay is open. Check Device Portal remote keys and scripted input
   still reach only player 1; pad 2 must not skip shader preload.
4. Save limits 1, 2, 3, and 4, relaunch, and verify persistence and admission.
   Test `NXBOX_MAX_PLAYERS=1` with a file value of 4. Check both English and
   Portuguese settings labels, hints, focus, and scrolling.
5. Enter a mode allowing only one player or a restricted style set. Verify NXbox
   does not repeatedly reconnect rejected IDs or reopen support applets. Return
   to multiplayer and check that allowed pads become available again.
6. When four pads are available, verify players 3–4, independent motion, removal
   of a middle slot, and no renumbering of the remaining pads.

Console behavior, game compatibility, UWP compilation, and visual UI rendering
remain unverified until tested on the owner's console/toolchain.
