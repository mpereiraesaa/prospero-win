# Installing games

Games are installed on the PC and played on the PS5. Installers start other
processes, show wizards and ask for keys; the PC has the same Wine as the
title (the pinned WoW64 build, `wine-11.17-54-g490f6d5`), so the prefix it
writes is one the console reads as is. The console's Wine does not start
processes (patch 0550 turns process creation off; whether a title could
fork has not been measured), so an installer could not run there today.

## The PC's Wine

`tools/build_host_wine.sh` builds it: the revision the title runs, with the
same patch series (the PS5-only parts compile only for the console), for the
desktop, with X11, FreeType, Vulkan and audio, so installers open their
windows on the PC and DXVK can be tried there. It is installed stripped, as
Wine's distribution packages are, and prints the path of its `wine`:

```sh
tools/build_host_wine.sh --source <pinned Wine checkout> --jobs 8
```

Installs run as the user `prospero`, as the title does
(`native/wine64_main.c`), so the prefix's profile folder is
`C:\users\prospero` on both.

## Recipes: Lutris installer scripts

A game's recipe is a [Lutris installer
script](https://github.com/lutris/lutris/blob/master/docs/installers.rst),
the YAML format the Lutris community maintains for thousands of Windows
games. A script from lutris.net can be used as it is or adapted; recipes
hold no game files and no keys. `tools/pw_install.py` runs a script with the
pinned host Wine instead of Lutris's Wine builds:

```sh
python3 tools/pw_install.py recipe.yml --library ~/prospero-library \
    --wine <wine build>/build-wow64/wine --file installer=/path/to/setup.exe
```

- **`--library DIR`** mirrors `/data/prospero-win` on the console. The game
  gets its own prefix, `DIR/prefixes/<slug>` (the script's `$GAMEDIR`), and
  a profile, `DIR/profiles/<slug>.profile`, whose `prefix = <slug>`.
- **`files`**: a `N/A:` file (the user's own copy) is given with
  `--file ID=PATH`; `http(s)` files are downloaded into `$CACHE`, which is
  deleted afterwards (`--keep-cache` keeps it). Steam sources are refused.
- **Interactive installers** open their window on the PC: the user clicks
  through them and types keys there. Nothing records what is typed.
- **`input_menu`** takes `--input ID=VALUE`, asks on a terminal, or uses its
  `preselect`.
- **`$RESOLUTION`** is `--resolution` (1920x1080 by default).

Supported directives: `move`, `copy`, `merge`, `extract` (zip and tar
built in, anything else through `7z`), `chmodx`, `execute`, `write_file`,
`write_config`, `write_json`, `input_menu`, `insert-disc` (`--disc DIR`).
Wine tasks: `create_prefix`, `wineexec` (with `return_code`),
`winetricks` (pinned to 20260125), `set_regedit` (`REG_SZ`, `REG_DWORD`,
`REG_BINARY`), `delete_registry_key`, `set_regedit_file`, `winekill`,
`eject_disc`. Anything else is refused, never skipped: a recipe either runs
as written or says what it needs.

Differences from Lutris, all forced by the console:

- `arch: win32` still makes a WoW64 prefix: the pinned Wine is WoW64 only,
  and it runs 32-bit programs there.
- `wine: {dxvk: true, dxvk_version: ...}` installs that DXVK release
  (pinned by SHA-256; 2.6.2 today) into the prefix's `system32` and
  `syswow64`, native in the prefix's registry and in the profile's
  `dll_overrides`. Scripts that turn DXVK off for OpenGL (`-opengl`) need
  changing: the console has no OpenGL.
- As in Lutris, Wine adds no menu entries or file associations to the PC
  (`winemenubuilder.exe=d`).

## The `prospero` block

Lutris ignores unknown top-level keys, so a recipe carries what only the
console needs in a `prospero` block, which becomes the profile's
`[display]` and `[input]` sections:

```yaml
prospero:
  name: Warcraft III          # the launcher's name (default: the script's name)
  display: {desktop: 1920x1080, scaling: fit, view: window}
  input: {preset: mouse, mode: keyboard}
```

The profile's `[application]` section comes from the script's `game`
section: `exe` (under `drive_c`, written as a `C:\` path), `args`,
`working_dir` (default: the exe's folder), and the PE architecture read from
the exe. The generated profile passes prospero-win-profiles'
`tools/check_profiles.py`.

## Prefix size

`wineboot` copies Wine's PE modules into the prefix: about 1.5 GB each from
a development build, which carries debug information, and about 300 MB from
`build_host_wine.sh`'s stripped installation.

## To the console and back

`tools/pw_prefix.py` copies a game to `/data/prospero-win` over the
console's FTP server and brings back what the console changed:

```sh
python3 tools/pw_prefix.py push <slug> --library ~/prospero-library --host <PS5 IP>
python3 tools/pw_prefix.py pull <slug> --library ~/prospero-library --host <PS5 IP>
python3 tools/pw_prefix.py status <slug> --library ~/prospero-library --host <PS5 IP>
```

- **push** sends `prefixes/<slug>` and `profiles/<slug>.profile`, and adds
  the profile to `profiles.lst` when the console has one. A manifest,
  `LIBRARY/.pw/<slug>.json`, records each file's size and SHA-256, so later
  pushes send only what changed; `--delete` also removes what the PC no
  longer has. Each file's size is checked after it is stored.
- The console's prefix is the game's live state (settings in the registry,
  saves beside the game). A push refuses a console prefix this PC never
  synced, or one whose registry changed since the last sync: **pull**
  first, which fetches every file the console changed. `--force` overrides.
- A title cannot make symbolic links: prospero-win keeps them in a
  `.pw-symlinks` table per directory. A push writes the links inside the
  prefix there (`dosdevices`' `c:` and `z:`); links out of it (Wine's
  Desktop or Documents into the PC's home) become empty directories, and
  serial and parallel port links are left out.
- In the lab, claim the console in the shared mailbox around a push, as
  for any console operation.
