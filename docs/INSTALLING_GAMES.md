# Installing games

Games are installed on your PC and played on the PS5. Installers are full
Windows programs: they open wizards, ask for CD keys and write the registry.
Your PC runs them with the same Wine the PS5 uses, so the result is a prefix
the console reads as it is. You then copy it over with one command, and copy
it back when you want to keep your saves on the PC too.

(The PS5 side doesn't run installers itself: its Wine has process creation
turned off, and installers start other programs.)

## Once: the PC's Wine

Build the Wine that matches the PS5's (same revision and patches, built for
the desktop so installer windows appear on your screen):

```sh
tools/build_host_wine.sh --source <pinned Wine checkout> --jobs 8
```

It prints the path of its `wine` when it's done. Installs run as the user
`prospero`, as the PS5 app does, so the prefix's user folder is
`C:\users\prospero` on both.

## Installing a game: Warcraft III as the example

Each game has a recipe in
[prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles)
(`recipes/`). A recipe is a [Lutris installer
script](https://github.com/lutris/lutris/blob/master/docs/installers.rst),
the format the Lutris community uses for thousands of Windows games. It holds
no game files and no keys, only the steps.

```sh
python3 tools/pw_install.py ../prospero-win-profiles/recipes/warcraft-iii-reign-of-chaos.yml \
    --library ~/prospero-library --wine <path printed above> \
    --file installer=/path/to/Warcraft\ III/Installer.exe
```

Blizzard's installer opens on your screen. Enter your CD key there, keep the
default folder and close it when it's done. The recipe then adds what the game
needs on the PS5: LAV Filters for the cinematics, the RenderEdge widescreen
fix, DXVK for Direct3D, and the resolution.

You end up with:

```text
~/prospero-library/
  prefixes/warcraft-iii-reign-of-chaos/        the game's Wine prefix
  profiles/warcraft-iii-reign-of-chaos.profile the profile the launcher reads
```

`~/prospero-library` mirrors `/data/prospero-win` on the PS5.

## Copying it to the PS5

```sh
python3 tools/pw_prefix.py push warcraft-iii-reign-of-chaos \
    --library ~/prospero-library --host <PS5 IP> \
    --cpu-dll <Wine build>/dlls/wowprospero/x86_64-windows/wowprospero.dll
```

This copies the prefix and the profile over FTP and adds the game to the
launcher's list. The first push takes a while, since a prefix is a few
hundred MB; later pushes send only what changed. `--cpu-dll` is prospero-win's
translator DLL, which 32-bit games need inside the prefix. The first push asks
for it, and later pushes send it again only when it changes.

Then copy the controller presets (prospero-win-profiles' `input/*.input`) to
`/data/prospero-win/input/` if you haven't yet.

### Big games

Modern games can be tens of GB, and the PS5's FTP server isn't fast: in our
tests it managed about 17 MB/s, so a 23 GB game took around 25 minutes.
Two things make that easier.

**A push can be stopped and picked up again.** It notes each file as soon as
the PS5 has it, so if the push is interrupted (you press Ctrl+C, the PC goes
to sleep, the network drops), run the same command again and it carries on
from where it stopped instead of starting over. Memory use stays small no
matter how big the game's files are.

If a push from an older version of `pw_prefix.py` was interrupted, there's no
record of what it sent. `--trust-size` treats any file the PS5 already has
at the right size as sent, and `--force` lets the push take over a PS5 copy
it doesn't have a record for:

```sh
python3 tools/pw_prefix.py push <game> --library ~/prospero-library --host <PS5 IP> \
    --force --trust-size
```

Only use this to finish a copy you started yourself. It checks file sizes,
not contents: a file that has the right size but different bytes is left as
it is. Registry files are always checked and sent.

**You can also send the files with ps5upload.** If you use
[ps5upload](https://github.com/phantomptr/ps5upload), `pw_prefix.py` can send
the game's files through it instead of FTP. Its authors report about 30 MB/s
on a standard PS5 and 60 MB/s on a PS5 Pro. On our standard PS5 it wasn't
faster than FTP: both sent a 1.2 GB test folder at about 16 MB/s over the
same network. It may help more on a faster connection. You need two things
from it:

- its payload, `ps5upload.elf`, running on the PS5. Send it with your ELF
  loader as you would any payload. Keep the FTP server running as well.
- its command-line client, `ps5upload-lab`, on your PC. Build it in
  ps5upload's `engine/` folder with `cargo build --release -p ps5upload-lab`.

Then add `--transport ps5upload` to the push:

```sh
python3 tools/pw_prefix.py push <game> --library ~/prospero-library --host <PS5 IP> \
    --transport ps5upload --ps5upload-client <ps5upload>/engine/target/release/ps5upload-lab
```

(You can also set `PS5UPLOAD_LAB` to the client's path, or put it on your
`PATH`.) The game's files travel through ps5upload a few GB at a time, and
each batch is recorded once the PS5 has stored it, so these pushes can be
resumed too. The registry, the symbolic-link tables, `wowprospero.dll` and
the profile still go over FTP, and FTP is also used afterwards to check that
every file arrived at the right size. If ps5upload's payload isn't running,
the push stops before sending anything and tells you so.

## Keeping saves in sync

On the PS5 the prefix is the game's live copy: settings and saves change
there. Before pushing again, bring those changes back:

```sh
python3 tools/pw_prefix.py pull warcraft-iii-reign-of-chaos --library ~/prospero-library --host <PS5 IP>
python3 tools/pw_prefix.py status warcraft-iii-reign-of-chaos --library ~/prospero-library --host <PS5 IP>
```

A push refuses to overwrite a PS5 prefix that changed since your last sync,
so you don't lose saves by accident. `--force` overrides that when you really
mean it.

## A game without a recipe

Look for the game on [lutris.net](https://lutris.net/): most Windows games
already have a script, and it often works as it is. Save it as a `.yml` file,
add a `prospero` block for the PS5 (below), and run it the same way. A few
things differ from Lutris:

- **Direct3D goes through DXVK.** `wine: {dxvk: true}` installs DXVK 2.6.2
  into the prefix. Games that use OpenGL instead (`-opengl` and the like) can
  select the experimental PS5 WGL backend when Wine was built with the PS5
  OpenGL SDK.
  Set `prospero: {graphics: opengl}` in the installer; this choice skips DXVK
  installation even if the Lutris `wine` section has `dxvk: true` (see the
  [OpenGL build notes](WINE_PS5_BUILD.md#opengl)).
- **32-bit games** (`arch: win32`) still get a 64-bit prefix. The PS5's Wine
  is WoW64 only, and it runs 32-bit programs inside a 64-bit prefix.
- **Installers that show a web page** (a license, for example) need
  `install_gecko: true` on `create_prefix`.
- **Anything the script asks** (`input_menu`) can be answered with
  `--input ID=VALUE`; `$RESOLUTION` is `--resolution`, 1920x1080 by default.

If a recipe uses something `pw_install.py` doesn't support, it stops and says
which step, rather than skip it and leave a half-installed game.

When it works, please send the recipe to prospero-win-profiles.

### The `prospero` block

Lutris ignores top-level keys it doesn't know, so a recipe carries the PS5's
settings in a `prospero` block. They become the profile's `[display]` and
`[input]` sections:

```yaml
prospero:
  name: Warcraft III          # the name in the launcher
  graphics: dxvk              # gdi, dxvk, opengl, or auto for this game
  display: {desktop: 1920x1080, scaling: fit}
  input: {preset: warcraft3}
```

`graphics` takes precedence over Lutris's `wine.dxvk` setting. `dxvk` installs
the pinned DXVK release; `opengl` selects Wine's builtin OpenGL driver for the
game even if Lutris requested a native `opengl32` override, and requires an
SDK-linked title. `display` also takes `view`, and
`show_fps: false` hides the frame-rate counter a game shows in the top-left
corner by default. `refresh: 120` lets an OpenGL game present at 120 Hz on a
display that supports it (the default is 60). `opengl_thread: true` runs an
OpenGL game's graphics work on its own CPU core, beside the game, which helps
busy scenes in games such as Half-Life and Counter-Strike (off by default).

The profile's `[application]` section comes from the script's `game` section:
the executable, its arguments and its working folder.

## Reference

### What `pw_install.py` supports

- **Directives:** `move`, `copy`, `merge`, `extract` (zip and tar, anything
  else through `7z`), `chmodx`, `execute`, `write_file`, `write_config`,
  `write_json`, `input_menu`, `insert-disc` (`--disc DIR`).
- **Wine tasks:** `create_prefix`, `wineexec` (with `return_code`),
  `winetricks` (pinned to 20260125), `set_regedit` (`REG_SZ`, `REG_DWORD`,
  `REG_BINARY`), `delete_registry_key`, `set_regedit_file`, `winekill`,
  `eject_disc`.
- **Files:**
  - `N/A:` files are your own copies, given with `--file ID=PATH`.
  - `http(s)` files are downloaded.
  - A file written as `{url: ..., filename: ..., sha256: ...}` is kept in the
    download cache and checked against its hash on every install. That's our
    addition (Lutris ignores it), for mods and DLLs from third parties.
  - Steam sources aren't supported.
- **Menus:** as in Lutris, Wine adds no menu entries or file associations to
  your PC.

### Details of `pw_prefix.py`

- **Manifest:** `~/prospero-library/.pw/<slug>.json` records each file's size
  and SHA-256, which is how pushes send only what changed. `--delete` also
  removes files your PC no longer has.
- **Progress:** a push saves the manifest as it goes: every 200 files or
  256 MB sent, after each ps5upload batch, and when it's stopped with Ctrl+C
  or `kill`. Even if the push is killed outright, it loses at most the files
  sent since the last save. Those files are sent again on the next push.
- **Memory:** files are read, hashed and sent 4 MB at a time, whatever their
  size. Only the registry files are read whole, because a push and a pull
  rewrite one line in them (see below).
- **ps5upload:** each batch (up to 2,000 files or 4 GB) is one ps5upload
  transaction. The batch is sent from a temporary folder of hard links to
  the game's files in `~/prospero-library/.pw/stage/`, so nothing is copied
  on your PC. If the client stops waiting before the PS5 confirms the
  batch, the push asks the payload's management port (9114) whether it went
  through.
- **Symbolic links:** a PS5 app can't make them, so prospero-win keeps them
  in a `.pw-symlinks` file per folder:
  - links inside the prefix (the `c:` and `z:` drives) are written there;
  - links pointing outside it (Wine's Desktop and Documents into your home
    folder) become empty folders.
- **The one registry difference:** the PS5 can't run 32-bit code directly, so
  the prefix's WoW64 CPU setting
  (`HKLM\Software\Microsoft\Wow64\x86`) names prospero-win's translator
  there and Wine's `wow64cpu.dll` on your PC. Push and pull switch it for you.

### Prefix size

A prefix holds its own copy of Wine's DLLs: about 300 MB with
`build_host_wine.sh`'s stripped build, plus the game.
