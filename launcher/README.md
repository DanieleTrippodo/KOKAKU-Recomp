# KOKAKU Recomp Setup

Native Windows x64 preparation utility. Port by **DDD**.
Support: https://ko-fi.com/X8X4FMCO5

1. Open **KOKAKU Recomp Setup.exe**.
2. Select your Ghost in the Shell: Stand Alone Complex **USA ULUS10020 ISO**.
3. Select a parent folder. A new **KOKAKU Recomp** folder will be created there.
4. Choose **PREPARE GAME**. When complete, open the output folder.
5. Double-click **KOKAKU Recomp.exe** to play.

The setup runs offline and needs no compiler, Python, font installation or
administrator privileges. Use a writable local folder. The output contains
the existing native port, its required libraries, and data from your own ISO.
It does not include the modloader, mods, personal settings or saved games.
Keep the entire output folder together when moving it.

Only the USA revision matching the bundled AOT profile is supported. Disc ID,
file sizes and SHA-256 hashes are checked. A different region, modified game
data, unsupported revision or damaged/truncated ISO is rejected.
The ISO is opened read-only. An existing output folder is never overwritten.
Cancellation removes only the temporary folder created by that operation.
No disc image or original game data is contained in this setup EXE.

The game requires Windows x64 and a DirectX 12 compatible GPU. This packaging
does not change the current port's gameplay compatibility; the full campaign
has not yet been validated.

## Rebuild (developers)

Sources are in `launcher/`. Run `python launcher/build.py` from the workspace.
The script uses `tools/w64devkit/bin` and the current `KokakuNative.exe` build.
It strips a copy of that executable and selects only the explicit runtime
allowlist. Game source inputs are recorded as hashes in `usa-manifest.json`.
The build never packages anything from `extracted/`, `mods/` or `SAVEDATA/`.

The developer build also uses Pillow to encode the approved PNG artwork as
multi-resolution Windows ICO files (16 through 256 pixels). `assets/setup.ico`
is linked into the setup EXE and used by its window. `assets/game.ico` is added
to the packaged game EXE before its payload hash is computed. Both embedded
icon groups are read back and verified during every build. No icon installation
or separate ICO file is required on the player's computer.

Departure Mono 1.500 is embedded under the SIL OFL. Source:
https://github.com/rektdeckard/departure-mono/releases/tag/v1.500

Diagnostic CLI (same extraction engine as the UI):
`"KOKAKU Recomp Setup.exe" --verify "game.iso"`
`"KOKAKU Recomp Setup.exe" --prepare "game.iso" "parent folder"`
Exit codes: 0 success, 1 error, 2 cancelled. See stdout/stderr when redirected.
