# p97 — Firmware, keys and games during setup

Implemented in the owner's `p97/firmware-setup` worktree, based on `rc/next`.
No commit, push, GitHub CLI, console access, or console deployment was performed.

The full implementation diff, including new host tests and the portable policy header,
is in [firmware-setup-p97.patch](firmware-setup-p97.patch). This report and the patch
are review artifacts; the patch does not include itself or this report.

## Screen behavior

First run is now: save-sync question → optional sign-in → keys/firmware/games →
button layout → `setup_done.txt`. Existing completed setups retain the previous
button-layout migration behavior. The new screen is also the first entry in
Settings → System. During emulation, that entry requests a return to the library
before configuration, following the existing account-editing pattern.

All three rows remain visible, even when files are already installed:

| Row | Status | Action |
| --- | --- | --- |
| `Keys (prod.keys / title.keys)` | Installed filenames and highest loaded master-key revision, e.g. `prod.keys + title.keys (r19)`; otherwise `Não selecionado` | `Selecionar` |
| `Firmware` | Installed firmware display version; otherwise `Não selecionado` | `Selecionar` |
| `Pasta de jogos` | Selected folder path; otherwise `Não selecionado` | `Selecionar` |

`r19` is a cryptographic key revision, not a claimed Switch firmware version.
Keys files do not have a universal firmware display-version field. No key bytes
are displayed or written to the report. A detected file is a suggestion, not a
claim that its contents are valid or compatible.

Up/down moves between rows and `Continuar`. A opens Files in selection mode;
its title is `Escolher pasta`. A enters folders, X chooses the current folder,
and B goes back/cancels. For keys, A on `prod.keys`/`title.keys` also chooses that
file; for firmware, A on a ZIP chooses the archive. Game-folder selection accepts
an empty folder. The Files view lists console storage and every accessible
removable drive, including drives without `NXbox` directories.

Suggestions appear under each row, one visible at a time with an `(index/count)`
suffix. LB/RB cycles them; `X Instalar · LB/RB Outras sugestões` explains the action.
Each suggestion opens the same confirmation used after a manual selection.

| State | Portuguese text | English text |
| --- | --- | --- |
| Title | Keys, firmware e jogos | Keys, firmware and games |
| Scan | Procurando arquivos nos dispositivos… | Looking for files on your drives… |
| No keys suggestion | Keys: LocalState\eden\keys ou NXbox\prod.keys (também switch\ ou raiz). | Keys: LocalState\eden\keys or NXbox\prod.keys (also switch\ or drive root). |
| No firmware suggestion | Firmware: LocalState\firmware ou NXbox\firmware (também firmware na raiz). | Firmware: LocalState\firmware or NXbox\firmware (also firmware at drive root). |
| No games suggestion/default source | Jogos: LocalState\games ou NXbox\games no dispositivo. | Games: LocalState\games or NXbox\games on your drive. |
| Confirmation | Instalar estes arquivos? Os arquivos instalados serão substituídos. | Install these files? Installed files will be replaced. |
| Confirm action | Instalar | Install |
| Working | Instalando… | Installing… |
| Success | Instalado. Você já pode continuar. | Installed. You can continue. |
| Failure | Não foi possível instalar. Confira os arquivos e as keys e tente de novo. | Could not install. Check the files and keys, then try again. |
| Inaccessible NAND/chunks | Acesso ao dump negado: use o NxbHelper em um PC para convertê-lo. | Existing localized `FirmwareConvertDump` text |
| Ready | Você pode alterar estas escolhas em Configurações → Sistema. | You can change these choices in Settings → System. |
| Continue | Continuar | Continue |

Verification/copy progress uses the existing install callback. ZIP/NAND extraction
shows the working text before a numeric total is available. Controls wait for
scan/install completion; B requests cancellation during firmware verification or
copy. After success or failure, selection and retry are available again.

`Continuar` and B leave the screen without any readiness prerequisite once the
scan or install has finished. Missing files do not block setup completion. The
single explanatory line next to Continue is chosen as follows:

- Without `prod.keys`: `Sem prod.keys, jogos criptografados não abrem; sem firmware, o menu do Switch não abre.`
- With keys but without firmware: `Sem firmware, o menu do Switch e os applets não abrem.`
- Without a custom game folder: `Jogos: LocalState\games ou NXbox\games no dispositivo.`
- Otherwise: `Você pode alterar estas escolhas em Configurações → Sistema.`

English equivalents are defined in `en.inc`. All new screen text uses `Text::`
entries. The screen uses existing theme tokens, plain text rows, focus rings,
pills, sheets and controller input; no UI/runtime dependency was added.

## Files actions and installation paths

The existing X action menu gains `Instalar como firmware` / `Install as firmware`
for folders and ZIP archives, and `Instalar keys` / `Install keys` for the two
conventional keys filenames. The confirmation shows the selected path, A installs,
and B cancels. The result remains in the Files status line. Existing file actions
remain available. Installation notifies the library's existing change signal,
refreshing its scan and firmware status.

Firmware startup and manual installation share the implementation in
`firmware.cpp`. Manual requests constrain discovery to the selected folder or
exact selected source file. The selected folder may contain nested independent
sets; the existing installer chooses its best verified compatible set. Manual
requests bypass the automatic-install fingerprint shortcut. Empty, invalid or
older selections report failure, including when firmware is already installed.

The original `Sources::Extract`, BIS-key NAND conversion, `VerifyFirmware`, and
`InstallFirmware` paths are retained. NCA decryption with the currently loaded
keys and integrity verification remain the compatibility check before replacing
installed content; the existing version comparison prevents downgrade. This
change does not invent a second firmware/key-version compatibility table.

Keys use `FirmwareManager::InstallKeys` and reload the existing key manager.
The shared key importer now creates its destination, accepts conventional
filenames with uppercase extensions, canonicalizes prod/title destination names,
and skips copying a file over itself. Its existing companion-file behavior is
preserved. Existing automatic startup discovery/import remains operational.
Library scans are stopped before opening these configuration flows so a key
reload does not race their key readers.

The game-folder choice is stored as UTF-8 in `LocalState\games_folder.txt`.
The library reads it in addition to its existing console and USB game sources,
using broker-aware `Files::List`/`Resolve`, with an eight-level recursion limit.
Selecting a folder references its games in place; it does not copy the games.
An unplugged or inaccessible selected drive is skipped without losing other
library sources.

## Discovery policy

The portable `setup_policy.h` takes the existing firmware IO abstraction. It
scans console storage and all accessible removable roots, at most eight child
levels, skipping reparses, resource-fork files, `__MACOSX`, temporary extraction
folders and archived firmware folders. Overlapping roots are deduplicated, and
an inaccessible subtree does not hide candidates on other drives. Installed
`LocalState\eden` data is excluded by the UWP caller.

Candidates are:

- A direct parent containing a loose `.nca` or chunked `.nca` directory.
- Each `.zip`, `rawnand.bin`, first `rawnand.bin.00`, or `eMMC/00` file.
- Each `prod.keys` or `title.keys` file, case insensitive.
- A direct parent containing `.nsp`, `.xci` or `.nsz` games.

An empty folder, unrelated file or RAR is not an automatic suggestion. A ZIP or
NAND candidate still has to pass the existing extraction/verification checks;
scanning never opens or modifies its payload. Empty folders remain selectable.

## Diff map

| Files | Change |
| --- | --- |
| `src/eden_uwp/setup_policy.h` | Portable candidate scan and guarded setup phases |
| `src/eden_uwp/firmware.{h,cpp}` | Shared selected-source entry point, scan bridge and keys status/install bridge |
| `src/eden_uwp/file_manager.cpp` | Enumerate complete removable-drive roots; retain console storage on enumeration failure |
| `src/eden_uwp/ui/files_screen.{h,cpp}` | Selection mode, firmware/keys actions, confirmation, progress and results |
| `src/eden_uwp/ui/savesync_ui.{h,cpp}` | Three-row setup screen and first-run integration |
| `src/eden_uwp/ui/library_screen.{h,cpp}`, `settings_model.h` | System entry, scan lifetime and refreshed status |
| `src/eden_uwp/ui/library.cpp` | Persisted additional game-folder scanning |
| `src/eden_uwp/ui/strings.h`, `lang/{en,pt_br}.inc` | Localized states/actions |
| `src/frontend_common/firmware_manager.cpp` | Small shared keys-import robustness changes |
| `src/eden_uwp/CMakeLists.txt` | Register policy header |
| `tests/port/firmware_setup.cpp`, `test_firmware_setup.py`, `CMakeLists.txt` | Executable host policy tests, Python runner and CTest target |

Only changed C++ ranges were formatted with clang-format; the new Python test
was formatted with Ruff. No unrelated whole-file reformat was retained.

## Validation performed on the Mac

- Firmware unittest group: **24 passed**, including the new compiled scan/state test.
- CMake Debug target `firmware-setup-tests`: built successfully; CTest
  `firmware-setup-policy`: **1/1 passed**.
- Broader pytest selection: **42 passed, 5 subtests passed, 1 pre-existing failure**.
  The failure is `test_button_layout.py::test_setup_migration_cards_and_settings_strings`:
  it requires the literal substring `Setting::XboxButtonIcons, Setting::ButtonLayout`.
  `HEAD:src/eden_uwp/ui/settings_model.h` already has a newline between those items;
  checking the exact assertion against HEAD returns false. That unrelated test
  was left unchanged.
- `git diff --check`: passed.
- The new host test compiles with `-Wall -Wextra -Wshadow -Wconversion -Werror -pedantic`.
  Its CMake MSVC target uses `/W4 /WX /we4456 /we4457 /we4459 /we4244`.
- New executable tests cover candidate grouping, multiple/overlapping drives,
  case-insensitive extensions/names, empty folders, unrelated files, NAND and
  eMMC suggestions, reparse/temporary/archive exclusions, inaccessible subtrees,
  recursion bounds, picker/confirmation cancellation, forbidden transitions while
  busy, retry after result, and continuing with all prerequisites missing.
- Firmware extraction/integrity behavior is also exercised by the existing
  synthetic firmware tests. No personal keys, NAND or firmware dump was used.

Reproduction:

```sh
python3 -m unittest discover -s tests/port -p 'test_firmware*.py'
cmake -S tests/port -B /tmp/nxbox-p97-host -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/nxbox-p97-host --target firmware-setup-tests -j 2
ctest --test-dir /tmp/nxbox-p97-host -R firmware-setup-policy --output-on-failure
```

MSVC/UWP compilation and rendered console behavior have **not** been verified
on this Mac. The host warnings check the portable policy, not Windows-only UI
translation units. The broader pytest invocation used isolated test tooling
(`uv run --with pytest --with fonttools`), without project runtime dependencies.

## Console check plan (not executed)

1. Build the UWP target with C4456/C4457/C4459/C4244 treated as errors. Confirm
   all changed Windows-only translation units compile. Deploy to a test install.
2. With `setup_done.txt` absent, test both save-sync answers. Verify the three
   rows appear before button layout, in PT-BR and EN, with legible long paths,
   no overlapping row text, correct focus rings and controller hints.
3. Start without any keys/firmware/games. Verify each placement sentence,
   `Selecionar` → `Escolher pasta`, B navigation, and Continue with nothing chosen.
   Confirm the marker is only written after the remaining button-layout step.
4. Reopen via Settings → System after setup. Verify installed firmware version,
   keys filenames/revision, persisted game path, retries and B/Continue behavior.
5. Attach two drives, including one with no NXbox directory. Place candidates in
   both standard layouts and arbitrary nested folders. Cycle every suggestion
   with LB/RB, and verify each X action confirms the selected source only.
6. Install keys, then a complete firmware folder and ZIP; check progress,
   installed version, Home/Mii availability and success. Repeat from Files X
   actions, and verify the library updates without restarting the app.
7. Test valid NAND with BIS keys, split NAND/eMMC, denied/chunk-invisible dumps,
   missing/older keys, truncated ZIP, incomplete firmware, downgrade, full disk,
   unplugged source and cancellation. Confirm failures/conversion guidance and
   retry eligibility; verify invalid input does not replace a valid installation.
   Cancellation during the existing destructive copy stage can leave a partial
   installation: check recovery by reinstalling the original valid source.
8. Choose an arbitrary game folder through Files. Verify its NSP/XCI/NSZ games,
   nested games, duplicate avoidance, persistence after restart, and safe skipping
   after removal of the drive. Existing console/USB sources should still appear.
9. Verify selecting keys already at their installed path does not self-copy,
   uppercase conventional filenames work, and `title.keys` alone reports the
   remaining missing prod.keys condition. Test a failed import followed by retry.
10. Regression-check Files copy/move/rename/delete/create and FTP, then normal
    game launch, save sync, button-layout migration and automatic startup firmware.
