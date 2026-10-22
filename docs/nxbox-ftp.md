# FTP uploads in Dev Mode

Open **Files → FTP** in NXbox. Unless `ftp-autostart` is present in LocalState,
the server starts disabled on each app launch and stops when disabled, when the library screen closes (including
game launch), or when the app suspends. Enable it again after resuming. Every start
generates a new six-digit password. The Files panel displays the private IPv4 URL,
username `nxbox`, and password. No password is written to diagnostics.

The FTP root exposes `/interno` (LocalState) and `/hd` (the first external drive's
NXbox folder, resolved by the same discovery as the library). Upload internal games
into `/interno/games/`, USB games into `/hd/games/`, and keys into
`/interno/eden/keys/`, and firmware files into `/interno/firmware/`. These folders are created on
server start. Uploads are streamed through a 1 MiB buffer into a sibling `.part`
file and replace the destination only after completion. Failed/cancelled uploads
remove that temporary file and retain an existing destination. APPE copies the old
file with the same bounded buffer before appending, requiring room for both copies.
The `.part` suffix is reserved. REST supports offset zero only; choose **overwrite**
when retrying a failed transfer. The library rescans after changes under either games folder.
Existing key and firmware loading/install flows are unchanged.

There is one authenticated control session at a time. Passive connections use
TCP 50000–50009, and the control socket uses TCP 2121. All sockets bind to the
first private IPv4 reported by NetworkInformation. Data connections must come
from the same address as the control connection. There is no active mode or TLS.
The package already declares `privateNetworkClientServer`.

## Local protocol checks

```sh
clang++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/port/ftp_protocol.cpp -o /tmp/nxbox-ftp-test
/tmp/nxbox-ftp-test
clang++ -std=c++20 -x c++-header -fsyntax-only src/eden_uwp/ftp/ftp_protocol.h
```

The test is also registered as `ftp-protocol` in `tests/port/CMakeLists.txt`.
These checks do not compile the WinRT implementation or exercise networking.

## Console acceptance with FileZilla

1. Install the UWP build on an Xbox Series X|S in Dev Mode. Keep NXbox open in
   its library UI, on the same LAN as the PC. Enable **Settings → Online → FTP
   server** and note the URL and current password.
2. In FileZilla, open **File → Site Manager → New site**. Set Protocol to
   **FTP – File Transfer Protocol**, Host to the displayed IPv4 (without `ftp://`),
   Port to **2121**, Encryption to **Only use plain FTP (insecure)**, Logon Type to
   **Normal**, User to **nxbox**, and Password to the displayed six digits.
3. Under **Transfer Settings**, select **Passive**, enable **Limit number of
   simultaneous connections**, and enter **1**. Set **Transfer → Transfer type →
   Binary**. Connect. Confirm `/interno` and, with a connected drive, `/hd` are listed.
4. Upload `prod.keys` to `/interno/eden/keys/`, firmware `.nca` files to `/interno/firmware/`, and
   a legally owned game named `My Game.nsp` to `/interno/games/`. Wait for **226** and an
   empty queue. Confirm the game appears after the library rescan. Reconnect and
   download it; compare its SHA-256 with the source. Repeat with a file over 10 GiB.
5. Rename and delete a game through FileZilla; confirm the library refreshes.
   Create/remove an empty folder. Try navigating above `/`: it must return **550**.
6. Cancel a large upload, then disconnect. Reconnect and confirm no incomplete
   destination is listed. Retry with **overwrite**. An existing destination must
   remain intact after a cancelled replacement. A second client must receive **421**.
7. Enter a wrong password: expect **530** after about two seconds; after five
   failures on the same control socket it must close. With the correct login, turn
   the toggle off during an upload: the client disconnects. Re-enable and verify
   the password changed. Suspend/resume NXbox and verify the toggle is off.
8. If control connects but listing/transfers fail, verify the Xbox/Dev Mode firewall
   permits inbound TCP **2121 and 50000–50009**, and disable Wi-Fi client isolation
   on the LAN. Do not forward these ports on the internet router.

MSVC/UWP compilation, console firewall behavior, suspension cancellation, actual
large-file transfers and visual rendering require Windows/Xbox verification.

If no drive is connected when FTP starts, `/hd` is unavailable; reconnect the
drive and restart FTP. Virtual roots cannot be renamed or deleted.

The Files browser can manage the same roots locally; see [Files](nxbox-files.md).
Additional external roots are named `/hd2`, `/hd3`, etc. in discovery order.
The FTP panel shows the current control client's address while connected.
An `ftp-autostart` file preserves headless startup; its first line overrides the
random password when it contains at least eight characters.
