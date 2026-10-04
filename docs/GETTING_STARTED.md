# Getting started

This guide takes you from a jailbroken PS5 to a Windows game on your TV.

## What you need

On the PS5:

- A jailbreak that lets you run homebrew, with an **FTP server** and an
  **ELF loader** (for example `ftpsrv` and `elfldr` from
  [ps5-payload-dev](https://github.com/ps5-payload-dev)). The steps below
  assume FTP on port 2121 and elfldr listening on local port 9021.
- A **homebrew app loader** that installs apps placed in `/data/homebrew`,
  such as [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus).
- The app's bundled one-shot Lapy helper is sent to elfldr on each process
  startup. No resident Lapy daemon is required. If elfldr is unavailable, the
  title logs the failure and returns to Home before loading Wine.

On your PC:

- **Linux** (a VM or WSL works too), to install games with Wine before
  copying them over.
- **Your own copy** of the game you want to play.

## 1. Install the app

prospero-win is a folder called `PPSA99995`, with the app (`eboot.bin`), its
Wine runtime and its modules. Upload the whole folder to `/data/homebrew/` on
the PS5 over FTP, then let your loader register it. It appears on the home
screen as **prospero-win**.

Download `prospero-win-<version>.zip` from the
[Releases page](https://github.com/mpereiraesaa/prospero-win/releases) and
unzip it to get the folder. If no release is listed yet, or you want the
latest code, build the pieces and put them together with
`tools/package_release.sh` ([packaging the app](DEVELOPMENT.md#packaging-the-app)).

To update, replace the whole `PPSA99995` folder. Your games and settings live
in `/data/prospero-win` and are kept.

## 2. Open it once

Start prospero-win from the home screen. You should see its launcher, a
blue "Prospero Win" screen with a list of games. The first time the list is
empty, and it asks you to add profiles to `/data/prospero-win/profiles`.

## 3. Add a game

Each game has two parts on the PS5:

```text
/data/prospero-win/
  prefixes/<game>/     the game's Wine prefix: its C: drive and its registry
  profiles/<game>.profile
  profiles/profiles.lst   the launcher's list, one profile file name per line
  input/<preset>.input    controller layouts shared by games
```

The easiest path is a game that already has a recipe in
[prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles).
You run the recipe on your PC, which installs the game into a fresh prefix
exactly as the PS5 will run it, then copy it over with one command.
[Installing games](INSTALLING_GAMES.md) explains how, with Warcraft III as
the example.

Copy the controller presets too: put the `.input` files from
prospero-win-profiles' `input/` folder into `/data/prospero-win/input/`.
[Controls](CONTROLS.md) explains what they do and how to write your own.

## 4. Play

In the launcher, move with the d-pad and press **Cross** to start a game. The
first start takes a while; a spinner shows while Wine and the game load.

- **DualSense:** what each button does depends on the game's preset (for
  Warcraft III, the left stick moves the pointer, Cross selects and Circle
  gives orders). Games made for an Xbox controller can get the DualSense as
  one. See [controls](CONTROLS.md).
- **USB keyboard and mouse:** plug them into the PS5 and they work in any
  game, alongside the controller. You can plug them in after the game starts.
- **Close a game:** hold **Options + Create** for a second. The game gets a
  chance to close normally (Alt+F4) and you return to the launcher. Quitting
  from the game's own menu works too.

Saves and settings stay in the game's prefix on the PS5.

## If something goes wrong

- **The game closes back to the launcher right away.** Something it needs is
  missing or not supported yet. The log (below) usually says what, and it's
  the most useful thing to attach to an issue.
- **Black screen with sound.** The game is probably drawing in a way the app
  can't show yet. Please open an issue with the game and its version.
- **No games in the launcher.** Check that `profiles/profiles.lst` lists your
  profile's file name, and that the profile's `prefix =` matches the folder
  under `prefixes/`. Check that elfldr is listening on port 9021 and that the
  packaged app contains `lapy.elf`; if the request does not
  complete, the title stops before loading profiles or starting a game.

### Getting a log

The app saves a log of each run on the console, so you don't need anything
running on your PC. The launcher's status line says whether saving works, and
pressing Options there shows where the files are. Copy them with any FTP client
from `/data/prospero-win/logs/`:

- `session-0.log` to `session-7.log` are the last eight runs (launcher and
  games each count as a run). `next.txt` holds the number of the next one, so
  the newest is the one just before it.
- A long run keeps its newest megabyte in `session-N.log` and the megabyte
  before that in `session-N.previous.log`; older lines are dropped.

Each file starts with a `PW_REPORT/1` line naming the build, the game and the
run, so you can tell which file belongs to which game.

You can also watch the log live on your PC. The app sends it over the network
as plain text lines. Put a
`dev.conf` in `/data/homebrew/` on the PS5 (or next to the app's `eboot.bin`;
the repository has [`dev.conf.example`](../dev.conf.example)) with your PC's
address:

```ini
DEV_ENABLED=1
DEV_SERVER=<your PC's IP address>
DEV_PORT=9300
```

Then, on the PC, before starting the app:

```sh
nc -lk 9300 > prospero-win.log
```

Each game run appears as one session, from `HELLO` to `BYE`. When the PC
isn't listening, the app keeps saving on the console and reconnects in the
background every five seconds, without pausing the game.
[Telemetry](TELEMETRY.md) lists what the records mean. To see Wine's own
messages for one game, add a `[debug]` section to its profile, for example
`winedebug = err+all,+loaddll`.

## Firmware

The app doesn't hard-code firmware offsets. What depends on your firmware is
the jailbreak, the app loader and the Lapy JB daemon, each of which documents
the firmwares it supports. If something doesn't work on yours, an issue with
the firmware version and what happened is really helpful.
