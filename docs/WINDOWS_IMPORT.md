# Importing an installed Windows game

The Windows importer packages an existing game folder into a Prospero Win
library. It does not run the game's installer or require Wine on Windows. It
starts from a clean prefix template made once on Linux with the pinned host
Wine, then copies the game's files, installs DXVK when selected, writes any
registry strings you specify, and generates the console profile. The current
command-line importer uses Python 3.11 or later and the standard library.

## Make the prefix template on Linux

Build the pinned host Wine and `wowprospero.dll` as described in
[installing games](INSTALLING_GAMES.md). Then run:

```sh
python3 tools/pw_import_windows.py template \
  --wine .deps/wine-host/install/usr/bin/wine \
  --cpu-dll .deps/wine/build/dlls/wowprospero/x86_64-windows/wowprospero.dll \
  --output prefix-template.zip
```

The template contains a clean WoW64 prefix, the console CPU DLL, and the Wine
`C:`/`Z:` drive mappings. Its ZIP contains ordinary files, including link
tables that Windows can extract without symlink privileges. The tool refuses
host Wine versions other than the pinned revision. Rebuild the template when
the console's Wine revision changes. The generated ZIP is a local artifact;
do not commit it to Git.

Copy `prefix-template.zip`, `tools/pw_import_windows.py`, and
`tools/pw_prefix.py` to the Windows PC. Keep the two Python scripts in the
same folder. For a Direct3D game, also copy the pinned DXVK release archive
(`dxvk-2.6.2.tar.gz`); the importer verifies its SHA-256 before use.

## Import the installed game on Windows

From PowerShell, in the repository folder (adjust the paths and game name):

```powershell
py -3 tools\pw_import_windows.py import `
  --template .\prefix-template.zip `
  --game-dir 'C:\GOG Games\Titan Quest - Anniversary Edition' `
  --exe TQ.exe `
  --install-path 'C:\Games\TitanQuestAE' `
  --library "$env:USERPROFILE\prospero-library" `
  --slug titan-quest-windows `
  --name 'Titan Quest (Windows)' `
  --graphics dxvk `
  --dxvk-archive .\dxvk-2.6.2.tar.gz `
  --input-mode xinput
```

`--exe` is relative to `--game-dir`. `--install-path` is where the game will
appear on the console's `C:` drive. Choose `--graphics gdi` for a software or
DirectDraw game, or `opengl` for an OpenGL game; DXVK needs its archive. For
keyboard-driven games, use `--input-mode keyboard` and optionally
`--input-preset path\to\game.input`. The preset is copied into the library.

Some installed games need registry values. Add each string with
`--reg-string 'HIVE\Key|Value name|Value'`, for example:

```powershell
--reg-string 'HKLM\Software\Bethesda Softworks\Fallout3|Installed Path|C:\Games\Fallout3GOG\'
```

For 32-bit games in this 64-bit prefix, a key may also be required under
`HKLM\Software\Wow6432Node`. The importer does not automatically copy a
Windows installation's registry, save files, Windows services, or installed
redistributables. Start with games that run from their installed folder and
add their required registry strings explicitly. Inspect their installer
metadata or documentation to find those values.

The output mirrors the console layout:

```text
prospero-library/
  prefixes/titan-quest-windows/
  profiles/titan-quest-windows.profile
  input/<preset>.input        # only if --input-preset was given
```

## Send it to the console

With the console's FTP server running:

```powershell
py -3 tools\pw_prefix.py push titan-quest-windows `
  --library "$env:USERPROFILE\prospero-library" `
  --host 10.168.2.52 --port 2121
```

The template already contains `wowprospero.dll`, so this push needs no
`--cpu-dll`. The push also copies a local input preset named by the profile;
it refuses to overwrite a different preset on the console unless you give
`--force`. Subsequent pushes and pulls use the same commands as Linux-built
profiles. Console gameplay compatibility still depends on the game and its
graphics and input APIs.
