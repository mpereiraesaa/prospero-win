<p align="center">
  <img src="assets/prospero-win-banner.svg" alt="prospero-win — Windows compatibility runtime for PS5 homebrew" width="900">
</p>

<p align="center">
  <a href="https://github.com/mpereiraesaa/prospero-win/actions/workflows/ci.yml"><img src="https://github.com/mpereiraesaa/prospero-win/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <img src="https://img.shields.io/badge/platform-PlayStation%205-1677E8" alt="Platform: PlayStation 5">
  <img src="https://img.shields.io/badge/status-experimental-orange" alt="Experimental">
  <img src="https://img.shields.io/badge/license-LGPL--2.1--or--later-blue" alt="License: LGPL-2.1-or-later">
</p>

**prospero-win — Windows gaming on your PS5**

Bring PC classics to the big screen. **prospero-win runs Windows games
locally on a homebrew-enabled PlayStation 5**, with DualSense controls,
keyboard and mouse support, and a launcher for your game library.

Built to bridge generations of Windows gaming—from 2000s classics to newer
32-bit and 64-bit software—with compatibility expanding game by game.

## What's supported

- **32-bit Windows games** through our custom x86 translator.
- **64-bit Windows applications** running natively on the PS5's x86-64 CPU
  through [Wine](https://www.winehq.org/).
- **Direct3D 8, 9, 10 and 11** graphics through
  [DXVK](https://github.com/doitsujin/dxvk) and Vulkan.
- **OpenGL games** with the optional OpenGL-enabled runtime.
- **Classic 2D games and Windows applications** through GDI.
- **DualSense controls**, including Xbox-style XInput support, analog
  sticks, triggers and rumble.
- **Custom controller mappings** for games originally designed for
  keyboard and mouse.
- **USB keyboard and mouse**, usable alongside the controller.
- **Game audio** through the PS5's audio output.
- **A game launcher** with individual profiles, graphics settings and
  control presets.
- **Separate game installations**, with tools to transfer games and
  synchronize saves between PC and console.
- **Community installation recipes and profiles** to make supported games
  easier to set up.

See [controls](docs/CONTROLS.md) for input modes and mappings, and
[installing games](docs/INSTALLING_GAMES.md) for library and save transfers.

## Already playable on PS5

- **Half-Life 2:** 60 FPS at High settings, tested with DualSense through
  Kleiner's lab.
- **Half-Life, Counter-Strike 1.6 and OpenArena:** playable above 60 FPS.
- **Warcraft III:** playable above 60 FPS, with DualSense or keyboard and
  mouse.
- **Space Cadet Pinball and Wine Minesweeper:** playable.

**Experimental, open source, and growing.** Compatibility depends on the
game and its requirements; support for 32-bit and 64-bit software does not
yet mean every modern Windows game works. Currently tested on one PS5 on
firmware **12.02**.

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
       ├─ 64-bit code ─► runs natively on the PS5's x86-64 CPU
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
