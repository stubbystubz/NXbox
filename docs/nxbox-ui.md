<!--
SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

# NXbox setup UI

NXbox currently boots straight into a game with no on-screen UI at all: `eden_uwp/uwp_boot.cpp` is
a headless `IFrameworkView` that picks the game from `LocalState\game.txt` and renders the emulator
directly into the `CoreWindow`. This document plans the setup screens a real player needs: where the
games and keys live, controller confirmation, and a library to pick from.

## Visual language

A reference UI already exists on this machine and console: **Nativra** (the sibling Xbox project,
its app is named `Kiosk` — `XboxDev/uwp/Kiosk/`). It uses a dark navy background (`#0D1012`), a cyan
accent (`#3EFFC0`), Archivo type, and flat 12px-radius bordered cards. NXbox should not look like a
reskin of it. Differences, deliberate:

| | Nativra/Kiosk | NXbox |
| --- | --- | --- |
| Background | navy `#0D1012` | true black `#000000` |
| Accent | cyan `#3EFFC0` | Nintendo Switch red `#E60012` (the one color; thematically the product's own brand, not picked for taste) |
| Depth | flat surface + 1px border | liquid glass on floating panels (game detail, settings sheet); plain 6% white hairline elsewhere |
| Hero background | fixed color | gradient extracted from the selected game's box art, three stops (0% / 48% / 72%) with a scrim, so every game's page feels different |
| Signature gesture | — | "the cartridge lift": selecting a tile lifts and tilts it slightly toward the viewer before the detail panel slides in, echoing a Switch cartridge being pulled — one gesture, used only there |

Everything else follows the house rules: 44px minimum touch target, pill shape on anything tappable,
one accent color used only for focus/action, lists as text rather than stacked cards where the data
is a list (game folder contents, key files found), empty states that say the next step instead of
the error, PT-BR and EN from the first screen, dark only (a TV has no "light mode" concept here, but
the token file still separates color from usage so this isn't hardcoded per-component).

## Game and key discovery: USB, not Device Portal

Development so far has pushed games and keys through the Device Portal (`console.py push`), which is
fine for us but not for a real player — Dev Mode's portal needs a PC and credentials. Nativra proved
a working, fast path already on this same hardware: a USB drive shows up as a drive letter
(`E:\Nativra\games` in Nativra's own layout), reached from a UWP app via
`KnownFolders.RemovableDevices` + `IStorageFolderHandleAccess::Create` (interface
`DF19938F-5462-48A0-BE65-D2A3271A08D6`) for a real Win32 `HANDLE`, not the ~220ms-per-file broker
path a plain file open would take. NXbox's setup screen scans an inserted USB drive for:

- `switch/prod.keys` and `switch/title.keys` (or `prod.keys`/`title.keys` at the drive root) — the
  common SD-backup layout Lockpick and similar tools already produce, so a player who dumped their
  own Switch already has the right file in the right place without doing anything NXbox-specific.
- `switch/games/*.nsp` (or a `nxbox/games/` folder) — copies (not moves) matching files into
  `LocalState\games\` so the emulator's existing `ResolveGamePath()` keeps working unchanged.

Device Portal push remains available underneath for development; the setup screen is what a player
without a PC needs.

## Screens (first layer — open it, no decisions)

1. **Welcome / scan.** Full black, centered: "Insert a USB drive with your games and keys." Live
   status once a drive appears: keys found (or not — link to "where do I get these"), N games found.
   No jargon; no keys/games found is not an error state, it explains the next physical action.
2. **Controller confirm.** "Press A." Once pressed, one extra choice with two options, not a form:
   **Nintendo layout** (confirm is the bottom face button, matching a real Switch controller) or
   **Xbox layout** (confirm is A, where Xbox players expect it) — the one place Switch and Xbox
   controller conventions genuinely conflict, so it is worth surfacing once, in plain terms, instead
   of silently picking one.
3. **Library.** A horizontal shelf of the games found (box art if the NSP's icon can be read, a
   generic cartridge tile otherwise). Selecting one does the cartridge-lift and opens the detail
   panel (glass, box-art gradient) with Play / Delete from console. Empty state (no games yet) sends
   the player back to step 1's instructions rather than showing a blank shelf.

## Screens (second layer — behind "More", on the library page)

- Per-game log level (for reporting bugs, not a normal setting).
- Motion controls: right-stick-as-motion toggle (see `docs/nxbox-port.md`'s motion-controls section).
- Network / Device Portal status (for us, not really for a player, but it's honest to show it).

## Architecture note

Kiosk is a **C# / .NET UWP app** (`Kiosk.csproj`, `App.xaml.cs`) built with MSBuild's XAML
toolchain. NXbox is **C++/WinRT**, built with CMake + Ninja (`CMakePresets.json`'s `uwp-x64`
preset) — a different stack. CMake has no first-class XAML compiler step, and bolting MSBuild's
XAML precompile (`Microsoft.UI.Xaml.Markup.Compiler`, the `.g.h`/`.g.cpp` codegen) onto a
Ninja-generated build is a real project of its own, not a side task here. Kiosk's XAML pages are a
visual/UX reference, not a code template.

Plan: draw the setup UI with **Direct2D + DirectWrite** directly onto the `CoreWindow`, the same way
`MesaWindow` already owns that window for the emulator — no XAML dependency at all, and the existing
`CoreWindow`/`CoreDispatcher` event loop in `uwp_boot.cpp` and `game_session.cpp` already does the
input and lifecycle plumbing this needs. The window's content swaps between the setup UI's Direct2D
render target and the emulator's Mesa/OpenGL swap chain when a game launches, rather than running
two windows. `game.txt`/`game.url` stay as the interface between the UI and `ResolveGamePath()` —
the setup screen picks a game, writes `LocalState\game.txt`, and either re-launches the boot path or
(later) calls into the existing game-session code directly.

First implementable, testable-without-any-rendering slice: the USB scanner itself
(`KnownFolders.RemovableDevices` + `IStorageFolderHandleAccess`), which can prove it finds
`switch/prod.keys`/`switch/games/*.nsp` on a real USB drive and copies them into `LocalState`,
independent of whatever draws the screen on top of it.


## Status (measured on Xbox)

Both foundations proven working, independent of each other, on real hardware:

- **USB discovery**: `usb_library.cpp`'s `ScanUsbForGamesAndKeys()` gets the `removableStorage`
  capability, enumerates the console's USB drive via `KnownFolders::RemovableDevices()`, and
  correctly reports no `switch/`, `nxbox/games/`, `prod.keys` or `title.keys` on Nativra's drive
  (there genuinely is none there) — no crash, no capability denial.
- **Rendering**: `setup_ui.cpp`'s `ShowSetupScreen()` stands up D3D11 + a D2D device/context + a
  DXGI swap chain on the same `CoreWindow` Mesa later takes over, and draws with DirectWrite —
  6 seconds of `BeginDraw`/`DrawText`/`EndDraw`/`Present1` with zero failures logged
  (`SETUP_UI_BEGIN` through `SETUP_UI_END`, no `SETUP_UI_FAILED`).

Not yet built: the actual screens from the plan above (only a placeholder title + one status line
renders right now), input/focus navigation on the setup screen, `ImportFromUsb()`'s copy path has
not been exercised against a drive that actually has files on it, and neither piece is wired into
the real boot flow yet (both are behind the `usb_scan_test.txt` marker file for testing only).

## Implementation plan (2026-10-01)

Approved look: a Library screen and a per-game Mods screen, previewed at 1920×1080 (true black, eShop art,
nav pills top-left (Library, Settings; mods are reached from a game, never from the nav), status top-right, glass hint bar at the bottom). The NXbox name is not shown on
screen. The one accent is the brand ribbon, a green→red gradient (`#2FD07A` → `#E8343E`). It is
used only as the 3 px focus ring around whatever has focus. The focused tile also lifts: scale 1.08,
−16 px, −1.2°, 220 ms with `cubic-bezier(0.2,0.8,0.2,1)`. Primary actions are white pills with
black text, secondary ones are glass pills. Numbers use a monospaced face. Text is Segoe UI.

Rendering stays Direct2D/DirectWrite/WIC on the CoreWindow, before Mesa takes the window (proven
on hardware by `setup_ui.cpp`: `SETUP_UI_BEGIN`/`END`). The UI owns a D3D11 flip swap chain and
releases it fully before the game boots.

### Increment 1: library and launch

- `src/eden_uwp/ui/` holds a small framework:
  - `renderer`: device, swap chain, D2D context, DWrite and WIC factories; text, images, rounded
    rects, gradients.
  - `input`: Windows.Gaming.Input polling with edge detection and repeat for D-pad and stick; A, B,
    X, Y, LB, RB and View.
  - `anim`: time-based easing.
  - `strings`: pt-BR and en. The system language decides the default; `NXBOX_LANG=pt|en` in
    `nxbox_env.txt` forces one.
- Library data:
  - Scan `LocalState\games` for `.nsp` and `.xci`.
  - Read each game's title ID, name and the Switch's own square icon from its control NCA:
    `control.nacp` plus `icon_AmericanEnglish.dat`, via `FileSys::NSP` / `FileSys::XCI`,
    `ExtractRomFS` and `NACP`.
  - Cache the result in `LocalState\library\<titleid>.json` and `.jpg`, so later launches never
    reparse the files.
- Hero:
  - Offline: a three-stop gradient extracted from the icon (0% / 48% / 72%) with a scrim, and the
    icon itself.
  - The hero is generated from the local control icon.
- Selecting a game and pressing A boots it: the path is handed to the existing boot path in
  `RunGameView`. With no games, the screen explains how to add one (USB `switch/games`, or a
  source in Settings).
- Every step writes a `UI ...` line through `Diagnostic()`.

### Increment 2: mod store (GameBanana)

- Find the game: `apiv11/Util/Search/Results?_sModelName=Game&_sSearchString=<name>`, preferring
  the "(Switch)" entry.
- List its mods: `apiv11/Mod/Index?_aFilters[Generic_Game]=<id>&_sSort=Generic_MostDownloaded`.
- Download a mod's files: `apiv11/Mod/<id>?_csvProperties=_aFiles`.
- Install into `LocalState\eden\load\<TITLEID>\<mod name>\`, normalizing the archive to its
  `romfs` / `exefs` / `cheats` folders. zip first, then 7z.
- Enable and disable through Eden's disabled add-ons list.

### Increment 3: sources and art

The Shop and its source management were removed in p66. The replacement is
[Files](nxbox-files.md), including the FTP panel formerly in Settings. The source
plan below is historical. Community library backgrounds remain available.

- Original plan: Settings → Sources (superseded by the Shop tab below), one list:
  - built-in GameBanana for mods;
  - shop-provided artwork on the index origin, or a generated title placeholder;
  - user-added sources in the Tinfoil shop format (`{"files":[{"url","size"}],"directories":[...]}`),
    which download games, updates and DLC into `LocalState\games`.
- No source of games ships configured; the user adds their own.

Focus model (owner, 2026-10-01): three vertical layers, as on the Xbox dashboard.
1. The game rail is the default focus, and A on a tile launches that game.
2. Up from the rail moves focus to the hero actions (Jogar, Mods, Detalhes) of the focused game.
3. Up again moves focus to the top nav (Biblioteca, Configurações).

Down goes back one layer. X (Mods) and Y (Detalhes) also work directly from the rail.

### Increment 4: SwitchSaveSync (requested 2026-10-01)

The first-run setup asks "Sincronizar saves com o SwitchSaveSync?". The same toggle lives in
Settings. The goal is the same cloud layout the Switch homebrew uses, so a real Switch and NXbox
share one save:

- Google Drive through the OAuth device flow: the TV shows the code, the phone signs in. The
  scope is `drive.file`, which only shows files created by the same OAuth client, so NXbox must
  use SwitchSaveSync's client ID. WebDAV comes later.
- Layout: `Nintendo Switch Saves/<game folder>/<account folder>/`, loose files, same names as
  SwitchSaveSync (`core/drive.c`, `core/syncjob.h` in the SwitchSaveSync repo).
- Eden saves live in `nand/user/save/0000000000000000/<user>/<TITLEID>/`.
- Download before the game boots (with a progress bar) and upload after it closes.
- Conflicts use SwitchSaveSync's fingerprint rule: if both sides changed, ask; never overwrite
  silently.

### Increment 1 status (2026-10-01)

Written but not yet compiled on Windows or run: CI and the console are the first to see this code.
It was syntax-checked against stand-in headers and its logic (focus layers, drawing, the `.nsz`
flow) was run on a host with fakes.

- Code: `src/eden_uwp/ui/` (`renderer`, `input`, `anim`, `theme`, `strings`, `library`,
  `library_screen`). The entry point is `Ui::RunLibrary(window)`; `RunGameView` calls it before
  Mesa takes the window. It returns the chosen game's path, which `RunGameView` writes to
  `LocalState\game.txt`, or an empty string, which keeps the old boot (`game.txt`, then the
  bundled homebrew). Every step logs a `UI ...` line.
- Look: the approved 1920x1080 preview, with the icon-derived gradient standing in for the eShop
  banner. The nav is one dark glass capsule with the selected tab as a white pill; the hero
  (gradient and scrim) fills the top 760 px; the rail sits on black below it.
- Navigation, as the owner changed it after the plan above:
  - Originally two tabs; now Library, Shop and Settings (LB/RB switch). Mods always belong to a game,
    so they are reached from the game's hero.
  - Focus moves through three layers: the game rail (default; A launches), the hero pills of the
    focused game (Play, Mods, Details), then the nav. Up goes one layer up, Down one down. The
    ribbon ring is drawn only on the focused element of the active layer. X (Mods) and Y
    (Details) work from the rail too.
  - The focused tile always stands at the left margin and the rail scrolls under it, so the name
    under the tile never meets the hint bar.
  - Mods only shows a notice until Increment 2. Details is a sheet with the package's title ID,
    format, size and file. A on the "add games" tile scans the folder again.
- `.nsz` packages: the scan converts each `.nsz` in `LocalState\games` to a `.nsp` next to it
  (`FileSys::ConvertNszToNsp`, `core/file_sys/nsz.h`) before reading any metadata, and the screen
  shows the game's name, the percentage and a progress bar meanwhile. The converter writes
  `<name>.nsp.partial`, renamed when complete, and the `.nsz` is deleted only after that. A failed
  or cancelled (B) conversion removes the partial file and keeps the `.nsz`; an `.nsz` that already
  has a `.nsp` is left alone. Log lines: `UI nsz convert <file> ok|fail <error>|skip ...`.
- Beyond the plan:
  - `LocalState\skip_library.txt` skips the screen, so unattended runs that boot from `game.txt`
    do not wait for A.
  - A game chosen in the library ignores `game.url`, which belongs to whatever `game.txt` named
    before: downloading it over the chosen file would replace the player's game.
  - The empty state says what works today (copy files with Device Portal, press A to scan); the USB
    import and the sources of Increment 3 are not wired to the screen yet. When the folder has
    games but none could be opened for want of keys, it says that instead of "no games yet".
- To check on the console first: that Mesa gets the window after the library's swap chain is
  released (`UI renderer released`, then the normal `GAME_*` lines); that the icons decode and the
  hero gradients look right; that Segoe UI, the monospaced face and the CJK fallback exist.

### Local covers and community backgrounds (2026-10-15)

- Covers in the library grid and detail sheet reuse `ui/library.cpp`'s Control NCA / RomFS
  extraction from the user's NSP/XCI. The icon matching the Xbox system locale wins; otherwise
  the first available `icon_*.dat` is used. Covers always remain local.
- Original icons are cached as `LocalState/library/<TITLEID>.control.jpg`; decoded BGRA icons
  are cached as `.control.bmp`. Metadata includes the icon language, so changing the system
  locale or replacing the package rebuilds the icon and derived hero.
- The fallback hero is generated from that control icon. A 32x32 area downscale feeds deterministic
  three-cluster k-means, ordered by luminance; near-monochrome art gets darker shades of its
  dominant color. Gradient stops are exactly 0%, 48%, 72%. A 64x64 downscale receives three
  separable box blur passes, a slight saturation lift, a cover crop, and the palette overlay.
  The resulting 192x76 BMP is cached as `.control.hero-v1.bmp` and upscaled on the GPU.
- The scrim remains a separate layer: transparent in the upper half, closing from 82% to the
  bottom, plus an 82% black veil across the left text zone. Even white artwork under this veil
  supports AA contrast for the title and metadata. Selection crossfades take 400 ms with
  `cubic-bezier(0.22, 1, 0.36, 1)` from `ui/anim.h`. Titles and cover icons remain visible.
- Before reading old art, a retryable one-time migration removes legacy banner JPEGs,
  their partial writes, old store icons and the old art index. It writes `library/own-art-v1`
  only after successful cleanup. New cache names never read legacy artwork.
- Shop `titledb` metadata may supply `iconUrl` and `bannerUrl`. Only HTTP(S) URLs on the index's
  own origin are accepted, with that shop's headers and no redirects/cookies; vendor hosts are
  rejected. Bounded background downloads keep rendering responsive. Missing art shows the
  title on a deterministic gradient seeded by its title ID, with AA title contrast.
- Hero backgrounds come from the [LaunchBox Games Database](https://gamesdb.launchbox-app.com/),
  community artwork credited in Settings > Credits. Settings > Artwork from the internet is on
  by default; turning it off persists locally, stops queued requests and hides downloaded art
  (an already-running HTTP operation may finish). No API key is needed.
- `tools/nxbox/build_art_index.py` downloads Metadata.zip on the development machine or weekly
  `art-index.yml` workflow, never on the console. It writes only filename metadata into
  `dist/art/backgrounds.json`. Fanart - Background ranks before Banner, then World/unregional,
  North America, Europe and other regions, with stable filename ordering. Edition variants
  with the same normalized name merge deterministically. No images are committed.
- The console downloads the compact index from this repository's main branch into
  `LocalState/community-art/backgrounds.json`, refreshes weekly and retains a validated old copy
  on failure. Matching strips trademarks, punctuation, `the` and edition suffixes, with Latin
  accent folding; the selected NACP title is tried before the English NACP title.
- A dedicated art worker downloads only from `images.launchbox-app.com`, disables redirects
  and cookies, center-crops/resizes to 1920x760 through WIC, and caches `<TITLEID>.hero-v1.bmp`.
  Missing or unusable art is cached for seven days; failed index refreshes retry after an hour.
  Arriving art crossfades over the blurred icon using the existing 400 ms hero curve. The scrim,
  gradient and local cover remain in place; there is no second online source.
- The previous store-art catalog and launcher artwork downloader are gone.
  Native launcher assets use explicit local `--icon` / `--banner` files, or NXbox's own logo.

Windows/MSVC build and console visual validation remain required; host tests exercise color
selection, gradient geometry, the parser's origin restrictions and the source-artwork ban.

### Increment 2 status: the mod store (2026-10-01)

Written, syntax-checked against stand-in headers, not yet compiled on Windows or run on the console.
The zip reader, the archive search and the HTML clean-up were also run on a host against a real zip.

- Code: `ui/mods.cpp` (GameBanana client, installer, zip reader), `ui/mods_screen.cpp` (the screen),
  `ui/widgets.cpp` (the ring, pills, hint bar, tabs and sheets that the library and the new screens
  share; they moved out of `library_screen.cpp` unchanged). It opens from the Mods pill or X, on a
  game, and returns with B.
- The game is found with `Util/Search/Results`, preferring the "(Switch)" record, and its id is cached
  in `LocalState\library\<TITLEID>.mods.json`, together with the installed mods and the disabled
  ones. All networking runs on worker threads (a search/list thread, a thumbnail thread, an install
  thread); thumbnails are decoded on the render thread, two per frame.
- Chips: Mais baixados (GameBanana's own order), Gráficos, Interface, Jogabilidade, Instalados with
  its count. LB/RB move between them. GameBanana's categories differ from game to game, so the three
  middle chips filter what is loaded by keyword on the root category (prefix match on its words), and
  the screen keeps loading pages while such a chip shows fewer than five rows (at most eight pages).
  Instalados comes from the record file, so it works offline.
- Install: the mod's files come from `Mod/<id>?_csvProperties=_sName,_nDownloadCount,_aFiles,_sText`.
  The first `.zip` is downloaded (progress on the row: "Baixando 64%"), unpacked into
  `LocalState\mods_tmp\<id>\x` (stored and deflate entries, CRC checked, zip64 and encrypted archives
  refused, `..` paths refused), and the shallowest folder holding `romfs`, `exefs` or `cheats` (up to
  six levels down, so `atmosphere\contents\<titleid>\` works) is moved to
  `LocalState\eden\load\<TITLEID16>\<mod name>\`. An archive with no such folder fails with a log
  line. A mod that only has `.7z` or `.rar` files shows "Formato ainda não suportado" on its row.
- On/off: X toggles an installed mod. Eden's disabled add-ons list (`Settings::values.disabled_addons`,
  keyed by title ID and folder name) is not reachable from `eden_settings.txt`, so the disabled names
  are kept in the record file and `Ui::ApplyDisabledMods` puts them in that list at boot (called in
  `RunGame` after the settings file). Nothing is renamed.
- Y opens the details sheet (author, category, count, first file with its size, description as plain
  text, 700 characters at most).
- Left out: `.7z` and `.rar`; a download resumes nothing (a cancelled one starts over); the count on
  the rows is likes when the listing carries no download count (the sheet shows downloads once the
  details arrive); no "Feito pro Xbox" badge (no field says it).
- Log lines: `UI mods ...`.

### Increment 4 status: SwitchSaveSync (2026-10-01)

Written, syntax-checked against stand-in headers, not yet compiled on Windows or run on the console.
The backend is `save_sync.h/.cpp`; `ui/savesync_ui.cpp` is the part the player sees.

- First run: when `LocalState\setup_done.txt` is missing, `RunLibrary` asks "Sincronizar saves com o
  SwitchSaveSync?" (Sim / Agora não) before the library shows, and writes `setup_done.txt` at the
  end (not when the window was closed meanwhile). Sim opens the sign-in screen; without
  `savesync.json` it explains that the OAuth client file is missing and goes on.
- Sign-in: `StartDeviceLogin()` then `PollDeviceLogin()` on a worker thread; the screen shows the
  verification URL, the user code large, a countdown to its expiry and the result. B cancels. No QR
  code (it would need an encoder; the code and the URL are enough).
- Settings uses a Shop-style category pane on the left and plain label/value/help rows on the
  right. The complete map (each setting appears once):

  | Category (PT / EN) | Rows, in order |
  | --- | --- |
  | Gráficos / Graphics | Renderer, default resolution, artwork from the internet |
  | Loja / Shop | Shop sources, USB install |
  | Online | Nextendo, Multiplayer |
  | Conta / Account | Profile picture, multiple accounts, Moru, SwitchSaveSync |
  | Sistema / System | Boot to system HOME menu, system interface, back to NXbox |
  | Sobre / About | Installed version/build and git SHA, Credits |

  D-pad/stick Up/Down selects a category or row. Right enters rows; Left returns to categories.
  A enters the category or opens/toggles its row. B closes the renderer choices, returns to categories,
  then the tab bar; B at the tab bar exits. LB/RB continues to cycle Library / Shop / Settings.
  `NXBOX_UI_SCRIPT` uses these same inputs: from Library, `RB RB DOWN` focuses Graphics;
  `RIGHT A` opens its renderer choices; `DOWN A` saves OpenGL. For Credits from the Settings tab
  bar: `DOWN DOWN DOWN DOWN DOWN DOWN RIGHT DOWN A`.

  Renderer choices are **DirectX 12 — recommended**, **OpenGL (Mesa) — compatibility** and a
  focusable but disabled **Vulkan — not available on Xbox** explanation. A on Vulkan does nothing.
  The first two replace `renderer_backend` (5 / 0) in `LocalState/eden_settings.txt`, retaining
  unrelated settings and removing the legacy `backend` alias. Changes apply on the next game launch;
  the file is reloaded before `MesaWindow` selects the API. D3D12 remains the default unless OpenGL
  is selected; `NXBOX_RENDERER` remains an explicit developer override. A conflicting per-title
  renderer override must agree with the window backend, as before.
  Resolution uses the existing choice sheet, saving `resolution_setup` globally; per-game overrides
  continue to win. System HOME/interface rows remain disabled without firmware. Back to NXbox
  updates the startup preference and also returns from a running guest when applicable.
  SwitchSaveSync signs in/out and sync is enabled exactly when configured and signed in.
- Before boot (only for a game chosen in the library): after the library returns, with sync enabled,
  `RunBootSync` reconciles the save behind a progress screen. On a conflict it shows both sides (time,
  files, size) and asks "Manter o save do Xbox" / "Usar o da nuvem" (focus starts on the newer side;
  B plays without syncing). The name given to SwitchSaveSync is `GameNameFromNames` over the NACP
  names, which the library cache now stores (`nacp_names` in `<TITLEID>.json`; an older record is
  rebuilt on the next scan). `eden_settings.txt` is applied first so the active profile is right.
- After the game: `SyncAfterExit` runs on the game's worker thread right after `RunGame` returns,
  which is after the guest stopped and `ShutdownMainProcess` released the save files. No screen
  (nothing owns the window then). Home suspends the app and the system may end it, and the window
  can be closed, so there is no safe point in those paths: `savesync_pending.txt` (title ID and name)
  is written before the boot and removed once the upload succeeded, and a leftover one is finished
  at the next launch, behind a progress screen, before the library shows.
- A game launched through `game.txt` with `skip_library.txt` has no title ID known to the UI and is
  not synced.
- Log lines: `UI setup ...`, `UI savesync ...`.

### Over-the-air updates (2026-10-01)

The library checks the latest GitHub release on an MTA worker. A newer package
adds an update pill to the right of the top navigation; move right from Settings
to focus it. Only A on that pill begins the download and Device Portal install.
A modal shows download percentage, installation/restart status, or a retryable
error. The worker cannot launch an install from the background check, and the
modal blocks game launches while a deployment is pending. PT and EN strings live
in the existing table. See [NXbox updates](nxbox-updates.md) for the release/tag
contract, portal.json schema, CSRF protocol and real-console validation limits.

### Install as game: one dashboard tile per game

A game of the library can become its own tile on the Xbox dashboard, with its own name and art,
that opens NXbox straight into that game.

How it works:

- NXbox registers the `nxbox` protocol (`dist/nxbox/AppxManifest.xml`). On activation,
  `uwp_boot.cpp` (`OnActivated`) parses `nxbox://play?title=<16 hex digits>`
  (`eden_uwp/protocol_uri.h`, unit-tested in `tests/port/protocol_uri.cpp`) and hands the title ID
  to `SetProtocolPlayTitle`. `Run()` pumps the dispatcher for up to 2 s so the activation is known
  before anything is shown.
- `RunGameView` takes a protocol-only path before setup, maintenance, USB, updates or SaveSync.
  It scans without NSZ conversion and passes the matching path directly to `RunGame`; it never
  opens the library or falls back to the last game. Missing games/keys show a localized error with
  B to exit. Pending cloud sync is left for a normal launch; protocol sessions use local saves.
- `ui/launch_screen.*` shows the cached name and local control icon on black, with no art downloads. The phase line shows lookup, keys, shader count and starting in PT/EN. The
  Direct2D CoreWindow target is released before OpenGL takes over; offscreen Direct2D frames are
  then presented through the game's context during shader loading. The final splash remains until
  the first guest frame. See [native tiles](nxbox-native-tiles.md) for ownership, diagnostic lines,
  cold/warm activation limits and console acceptance tests.
- The tile is a separate package, `NSPX.NXbox.Game.<TITLEID>`: `src/nxbox_launcher` is a tiny UWP
  app that reads `title.txt` from its package, calls `Launcher.LaunchUriAsync` with the URI above
  and exits as soon as acceptance arrives. It paints the packaged splash while waiting, without
  network work. One binary serves every game. `tools/nxbox/package_launcher.py` bundles it with
  `title.txt`, the display name and Square44/150, Wide310x150, StoreLogo and SplashScreen PNGs
  generated from explicit local images (NXbox's own logo when none are supplied), and signs it
  with the NXbox certificate.

Adding a tile:

1. NXbox itself must be installed in a version that has the protocol (this one or later).
2. Run the workflow **Package NXbox game tile** (Actions, Run workflow) with `title_id` (the 16
   digits the library shows / `LocalState\library\<TITLEID>.json`) and `name`.
   This workflow uses NXbox's own logo; local packaging accepts `--icon` and `--banner`.
3. Download the `nxbox-game-tile-<TITLEID>` artifact and install the `.appx` through the Device
   Portal like NXbox itself. Repeat per game.

Locally: `python tools/nxbox/package_launcher.py --exe nxbox-launcher.exe --title-id <ID> --name
"<Name>" --stage-only` stages the package without the Windows SDK (needs Pillow).

This improves presentation; the running dashboard identity is still NXbox and the OS still switches
between packages. Full per-game runtime packages are deferred until shared data, migration and
recovery are safe; [the analysis](nxbox-native-tiles.md) records the payload, path audit and blockers.

### Shop tab and source compatibility (2026-10-15)

The top navigation is **Library | Shop | Settings**. LB/RB cycles the three tabs, including
while browsing a shop. Left/Right on the focused top bar also selects a tab; Down enters its
content. No game shop is preconfigured.

Shop uses the Tinfoil structure with NXbox branding and the existing black theme, pills, ribbon
focus, art loader and animation helpers:

- The sidebar contains New Games, New Updates, New DLC, All, Search, Queue and Sources. Up/Down
  selects a section; Right/A enters its content, Left from the first grid column or B returns to
  the sidebar. Up from the first sidebar row/B returns focus to the top tabs.
- Content combines the configured shops into a four-column, two-row cover grid. Left/Right and
  Up/Down move between covers. X cycles name, total size and newest (reverse index order, not a
  publication date); Y opens the controller keyboard to search loaded names. Search covers All.
  Menu reloads the configured shops. Results arrive incrementally; the cursor preserves its title
  when new entries reorder the grid. Only visible covers retain GPU images, with at most two
  decodes per frame.
- A opens a full-page title view: banner/icon, name, application ID, total size, Game/Update/DLC
  availability and installed state, and one download action per package showing its title ID,
  version and size. Unknown metadata stays unknown. These indexes do not provide reliable
  publisher/description fields. Local filename or title ID/kind/version matching remains in use;
  a newer installed version satisfies an older package.
- View opens Queue; View returns to its previous page. Queue also has its own sidebar section.
  Up/Down chooses an item, X cancels it, and B returns to the sidebar. Progress, byte counts and
  speed are live for the active item; waiting items show 0%. The serial queue has eight slots and
  rejects installed packages and duplicate URL/destination entries. Downloads continue across
  top-level tab changes. Closing the library cancels the active transfer and discards waiting
  entries; partial files remain resumable. Incomplete packages retain `.partial` until complete.
- The header shows `NXbox - <section>`, the clock and free-space bars for system/external storage
  (Xbox storage names rather than Switch's microSD terminology). Storage queries run on an MTA
  worker. The footer shows the selected emulator profile name/avatar using read APIs, with button
  hints aligned right. No account or profile implementation is changed.
- The Sources section lists configured shops and their failure messages. A opens **Settings →
  Shop sources**, which only adds an address, imports `sources.txt` from USB, or removes a source
  with confirmation. Opening a saved row there never browses it. The empty Shop explains the next
  step and provides an Add source button. Existing `sources.json` and `sources.txt` (`URL` or
  `name|URL`, UTF-8 BOM/comments/blank lines allowed) remain compatible.

| Source/index feature | Behavior on Xbox |
| --- | --- |
| HTTP/HTTPS Tinfoil JSON | `files` string/object entries, relative/root/absolute URLs, `size`, percent-decoded `#name` display fragments |
| `directories` | Breadth-first recursive traversal: depth 3, 64 total indexes (including roots/failures), 5,000 packages |
| `headers`, `referrer` | Validated request headers and HTTP `Referer`, inherited by child requests and package downloads; authentication is retained in queued entries |
| `success` | Welcome message shown once per library session; repeated refreshes do not repeat it |
| `locations` | Additional source URLs (strings, array entries including `{url}`, or object values); persisted with duplicate checks and scheduled within the same traversal budget |
| `error` | Source failure text shown and retained against its address in Sources; other shops still load |
| HTTP directory listing | Apache/nginx/IIS anchor links to `.nsp`, `.nsz`, `.xci`, `.xcz`, including encoded names, quoted/unquoted attributes and HTML query entities; descendant directories followed within the same budget; parent, sorting and external-directory links ignored |
| `http(s)://user:pass@host/…` | Percent-decoded Basic authentication, explicit Authorization header; credentials removed before constructing the WinRT request URI |
| `ftp://`, `ftps://` | Saved/imported but explicitly reported as **not supported on Xbox** for directory browsing; no FTP list transport is implemented |
| `gdrive:` folder IDs | Saved/imported but explicitly reported unsupported: no Google Drive account/API or anonymous folder resolver is implemented |
| 1fichier landing links | Explicit unsupported message: no landing-page/token/account resolver is implemented; direct HTTP package URLs in ordinary indexes use the HTTP downloader |
| Payload beginning `TINFOIL` | Detected before JSON/HTML parsing; **encrypted shop index is not supported**; no decryption attempted |

The UWP `Windows.Web.Http.HttpClient` supports HTTP/HTTPS, and BackgroundTransfer's FTP support
is download-only, not directory enumeration. See [Microsoft's networking guidance](https://learn.microsoft.com/en-us/windows/uwp/gaming/work-with-networking-in-your-directx-game)
and [Background transfers](https://learn.microsoft.com/en-us/windows/uwp/networking/background-transfers).
Uncredentialed packages retain the shared downloader's removable-storage fallback; header-bearing
packages use a bounded streaming transfer with Range resume, a free-space check and removable-storage
fallback. Index responses are streamed with a 16 MiB cap.
A transfer that lacks space or a usable Content-Length fails visibly and retains its partial file.

The existing `Input` instance drives Shop and the controller keyboard. `NXBOX_UI_SCRIPT` remains
unchanged; no separate input poller or nested browsing loop is introduced. Console smoke sequence:
from Library press RB, Down (sidebar), Right (grid), A (details), A (enqueue), View (queue), RB
(Settings), LB (Shop, same queue). B returns to the sidebar, and its last row opens source management.
Also exercise no sources, a failed/encrypted source alongside a working shop, PT-BR/English, installed
packages, cancellation/resume, and a completed download appearing after returning to Library.

Validation on the host:

```sh
python3 -m unittest discover -s tests/port -p 'test_*source*'
python3 -m unittest discover -s tests/port -p 'test_*tinfoil*'
```

Fixture JSON/HTML/binary payloads live in `tests/port/fixtures/tinfoil_*`. Pure parsing, traversal,
authentication, metadata/grouping/sorting, installed-version and queue tests run on the host. The
application requires MSVC CI and a console run to validate WinRT networking, Direct2D rendering and
controller/script behavior; it cannot be built on this Mac.
