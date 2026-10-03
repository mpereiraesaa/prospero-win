<p align="center">
  <img src="assets/prospero-win-banner.svg" alt="prospero-win — Windows compatibility runtime for PS5 homebrew" width="900">
</p>

<p align="center">
  <a href="https://github.com/mpereiraesaa/prospero-win/actions/workflows/ci.yml"><img src="https://github.com/mpereiraesaa/prospero-win/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <img src="https://img.shields.io/badge/platform-PlayStation%205-1677E8" alt="Platform: PlayStation 5">
  <img src="https://img.shields.io/badge/status-experimental-orange" alt="Experimental">
  <img src="https://img.shields.io/badge/license-LGPL--2.1--or--later-blue" alt="License: LGPL-2.1-or-later">
</p>

**prospero-win runs Windows games on a jailbroken PS5.** It's a homebrew app
that carries its own copy of [Wine](https://www.winehq.org/). 32-bit Windows
code goes through our own x86 translator, and Direct3D goes through
[DXVK](https://github.com/doitsujin/dxvk) on the console's Vulkan. You pick a
game from its launcher and play it with a DualSense, or with a USB keyboard
and mouse.

It's experimental. A handful of games run well, many won't run yet, and it
has only been tested on one console, on firmware 12.02.

## What runs

Half-Life, Counter-Strike 1.6, OpenArena and Warcraft III are playable
above 60 fps. Pinball and Minesweeper are playable too, and Half-Life 2 runs
at 60 fps from the train to the canals, with the DualSense or a keyboard and
mouse; chapters after that haven't been tested yet.

See [game compatibility](COMPATIBILITY.md) for versions, graphics backends
and controls. Translator benchmark results are in
[DBT benchmarks](docs/DBT_BENCHMARK.md).

Game profiles, controller presets and install recipes live in
[prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles).
If you get a game running, a profile there is the best way to share it.

## Try it

You need a PS5 that can run homebrew: an FTP server and ELF loader
(for example from [ps5-payload-dev](https://github.com/ps5-payload-dev)), a
loader that installs apps from `/data/homebrew` (such as
[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)), and the
[Lapy JB daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon), which
gives the app access to `/data` where your games live. You also need your own
copy of the game, and a Linux PC to install it on.

The app itself is a zip on the [Releases page](https://github.com/mpereiraesaa/prospero-win/releases).
[Getting started](docs/GETTING_STARTED.md) walks through the whole setup, and
[installing games](docs/INSTALLING_GAMES.md) covers adding a game.

### Other firmwares

Everything so far was tested on firmware 12.02. The app itself doesn't use
firmware-specific offsets. The parts that depend on firmware are the
jailbreak, the loader and the Lapy JB daemon (which lists 3.00–12.00). If you
try another firmware, please open an issue saying what worked and what
didn't, with the firmware version. That's exactly the information we're
missing.

## How it works

```text
Windows game (.exe)
  └─ Wine: its Windows DLLs, its Unix side and its server, all inside the PS5 app
       ├─ 32-bit code ─► prospero-win's x86 translator (Wine's WoW64 CPU)
       ├─ Direct3D 8–11 ─► DXVK ─► Vulkan (RADV) ─► the TV
       ├─ 2D drawing (GDI), movies ─► the app's own display path ─► the TV
       └─ sound, DualSense, USB keyboard and mouse ─► the PS5's own services
```

Each game starts in a fresh process with its own Wine prefix, and closing it
takes you back to the launcher. [Architecture](docs/ARCHITECTURE.md) and
[Wine on the PS5](docs/WINE_PS5_BUILD.md) go into the details.

## Building from source

Host contributions need x86_64 Linux, a C compiler, Clang, Make, Python 3
and PyYAML. No console or SDK is needed for these checks.

```sh
python3 tools/check_setup.py
make -j2 all            # host tests, the publication audit, whitespace
tools/build_native.sh   # the PS5 app (needs the pinned PS5 payload SDK)
tools/build_wine_ps5.sh # Wine's PS5 modules
```

[Development](docs/DEVELOPMENT.md) covers the toolchains and checks. Pull
requests are welcome: [contributing](CONTRIBUTING.md) explains the ground
rules.

## What this project is not

It doesn't bypass DRM or anti-cheat, it doesn't load kernel drivers, and it
doesn't ship games. Please don't open issues or pull requests with Windows
binaries, game files or keys.

## License

[LGPL-2.1-or-later](LICENSE), like Wine. See the [licensing notes](LICENSING.md)
and [third-party notices](NOTICE.md). `PPSA99995` is a local title ID we
picked, not one assigned by Sony.
