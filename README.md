<p align="center">
  <img src="assets/prospero-win-banner.svg" alt="prospero-win — Windows compatibility runtime for PS5 homebrew" width="900">
</p>

<p align="center">
  <a href="https://github.com/mpereiraesaa/prospero-win/actions/workflows/ci.yml"><img src="https://github.com/mpereiraesaa/prospero-win/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <img src="https://img.shields.io/badge/platform-PlayStation%205-1677E8" alt="Platform: PlayStation 5">
  <img src="https://img.shields.io/badge/status-experimental-orange" alt="Experimental">
  <img src="https://img.shields.io/badge/license-LGPL--2.1--or--later-blue" alt="License: LGPL-2.1-or-later">
</p>

**prospero-win** is an experimental Windows compatibility runtime for native
PlayStation 5 homebrew. It runs Wine itself inside a PS5 title: Wine's PE
modules, its Unix side and its object server, with prospero-win's IA-32
dynamic binary translator as Wine's WoW64 CPU for 32-bit code.

```text
Windows application → Wine (PE + Unix side, in the title) → prospero-win → PS5 services
      PE32 code ──► prospero-win DBT (WoW64 CPU)        graphics: GDI → PS5 user driver → VideoOut
```

## Current status

On an owned PS5 running firmware 12.02, the title boots into its own
launcher, which lists the games described by profiles under
`/data/prospero-win`. Each game runs in its own title process through Wine.
The original Windows Space Cadet Pinball (PE32, through the DBT) has been
played full-screen with the DualSense, and Wine's Minesweeper (PE64) with a
stick-driven cursor drawn by Wine's PS5 user driver. A profile chooses the
prefix, the desktop size and scaling, the button bindings or an XInput
controller, and the pointer.

The DBT runs 7-Zip's benchmark at 91% of native speed on an x86-64 host,
nbench at 92–97% and a Super PI-style pi program at 98%. On the console it
rates 7-Zip at about 3360 MIPS, an estimated 85–93% of the console's native
speed across these benchmarks ([DBT benchmark](docs/DBT_BENCHMARK.md)). Fonts, audio through Wine's PS5
driver and the XInput controller are built and host-tested; their console
validation is recorded in [hardware validation](docs/HARDWARE_VALIDATION.md)
as it lands.
Direct3D through DXVK over `ps5-vulkan` is not yet available.

## Build and test

```sh
make -j2 all          # host tests, publication audit, whitespace
make -j2 sanitize
tools/build_native.sh # the PS5 title (needs the pinned payload SDK)
```

Wine's PRXs are built by `tools/build_wine_ps5.sh`; see
[development](docs/DEVELOPMENT.md). Games are user-supplied: copy an
installed game into a prefix under `/data/prospero-win` and add a profile.
Profiles checked on the console, their input presets and the benchmark
programs' build are kept in [prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles). Do not commit Windows binaries,
game data, captures, telemetry transcripts, SDK files or private paths.

## Documentation

- [Architecture](docs/ARCHITECTURE.md) · [Wine integration](docs/WINE_INTEGRATION.md)
- [Wine on the PS5](docs/WINE_PS5_BUILD.md): build, patches, drivers, profiles
- [Hardware validation](docs/HARDWARE_VALIDATION.md) · [Roadmap](docs/ROADMAP.md)
- [DBT benchmark](docs/DBT_BENCHMARK.md) · [Development](docs/DEVELOPMENT.md) · [Contributing](CONTRIBUTING.md)

Pinball is a compatibility test, not the scope of the project. DRM,
anti-cheat, kernel drivers and distribution of proprietary game files are out
of scope. Hardware claims are tied to exact artifacts and structured
`ps5log/1` evidence; screenshots alone do not establish runtime correctness.

Licensed under [LGPL-2.1-or-later](LICENSE) · See [licensing notes](LICENSING.md)
and [third-party notices](NOTICE.md). `PPSA99995` is a local development
identifier, not an official Sony assignment.
