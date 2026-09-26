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
PlayStation 5 homebrew. It combines Wine's Windows subsystem with a PS5-native
platform layer, translating 32-bit x86 code while connecting guest applications
to native system services.

```text
Windows application → Wine → prospero-win → PS5 services
                                      └── graphics: GDI / DXVK → ps5-vulkan → AGC
```

## Current status

The first playable title is the original Windows Space Cadet Pinball
executable. It is a compatibility regression target for the general runtime,
not a project-specific architecture. On an owned
PS5 running firmware 12.02, the direct Win32 path has been validated for
playable video, asynchronous audio, DualSense input, persistent registry state,
and close/relaunch.

The IA-32 dynamic binary translator includes direct block chaining, cross-block
guest-register residency, dead-flag elimination and lazy arithmetic flags.
Supported host execution modes produce identical guest CPU state in the exact
regression matrix.

The separate native Wine bootstrap has reached a generated PE32 fixture's
entrypoint on hardware. That fixture then stopped at an unsupported
`NtTerminateThread` service. This validates the bootstrap path, not general
Wine application compatibility. Persistent prefixes, broader Wine services,
and a user-supplied copy-and-run workflow remain in progress. PE64 applications
also need their Windows/native ABI and loader boundaries completed.

## Next milestone

Run a user-supplied installed application from an isolated, persistent Wine
prefix using a shared runtime and application manifest. Graphical installers,
Explorer and store clients are not prerequisites for this first reusable
workflow.

## Build and test

```sh
make -j2 all
make -j2 sanitize
make inspect-only PE_INPUT=/private/path/APPLICATION.EXE
```

To build a native package, provide legally obtained Windows files from an
external directory:

```sh
PW_FOUNDATION_READY=1 \
PW_STAGE_INPUT=/private/path/application \
PW_ROOT_MODULE=application.exe \
tools/build_native.sh
```

Private applications are staged only under the ignored `dist/` tree. Do not
commit Windows binaries, game data, captures, telemetry transcripts, SDK files
or private paths.

## Documentation

- [Technical details](docs/TECHNICAL_DETAILS.md)
- [Architecture](docs/ARCHITECTURE.md) · [Wine integration](docs/WINE_INTEGRATION.md)
- [Hardware validation](docs/HARDWARE_VALIDATION.md) · [Roadmap](docs/ROADMAP.md)
- [Development](docs/DEVELOPMENT.md) · [Contributing](CONTRIBUTING.md)

Pinball is a compatibility test, not the scope of the project. DRM,
anti-cheat, kernel drivers and distribution of proprietary game files are out
of scope. Hardware claims are tied to exact artifacts and structured
`ps5log/1` evidence; screenshots alone do not establish runtime correctness.

Licensed under [LGPL-2.1-or-later](LICENSE) · See [licensing notes](LICENSING.md)
and [third-party notices](NOTICE.md). `PPSA99995` is a local development
identifier, not an official Sony assignment.
