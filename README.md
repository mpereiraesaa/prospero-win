# prospero-win

prospero-win is an experimental Windows compatibility runtime for native
PlayStation 5 homebrew. It combines Wine's Windows subsystem with a PS5-native
platform layer:

```text
Windows application
  -> Wine PE modules
     -> prospero-win CPU and platform boundary
        -> PS5 threads, memory, files, audio, input and graphics

Direct3D -> DXVK -> ps5-vulkan -> AGC / VideoOut / gfx1013
```

PE32 programs execute through the project's IA-32 dynamic binary translator.
PE64 support will use native x86-64 instructions once its Windows-to-native ABI,
loader, callback and exception boundaries are complete.

The first compatibility target is the original Windows Space Cadet Pinball
executable, running without recompilation. Pinball is a compatibility regression
target for the general runtime, not a project-specific architecture. DRM,
anti-cheat, kernel drivers and distribution of proprietary game files are out
of scope.

## Current status

On an owned PS5 running firmware 12.02, the direct Win32 bootstrap makes
Pinball the first playable title:

- the PE32 image executes through the IA-32 DBT;
- GDI output reaches a 1920x1080 AGC/VideoOut presentation path;
- WinMM/WaveMix PCM reaches SceAudioOut asynchronously;
- DualSense input is translated into Win32 key messages;
- registry state persists in title-owned storage; and
- close and relaunch work without rebooting the console.

The DBT includes hashed block lookup, direct block chaining, cross-block guest
register residency, dead-flag elimination and lazy arithmetic flags. Exact
host matrices finish with identical guest CPU state in every supported mode,
and bounded hardware runs reach video, audio and input teardown cleanly.

The opt-in native Wine bootstrap has now run on the PS5 with a generated PE32
profile. An explicit DBT marker confirms it reached the `app.exe` entrypoint;
the fixture then stopped at `NtTerminateThread` with guest status 1 after
598,649 guest instructions across 2,981 blocks. `ps5log/1` recorded a clean
close with no gaps or cleanup failures. The runner still labels process
termination `unsupported` as a gate stop, so this does not claim general
application compatibility.

This proves the generated fixture reached its entrypoint, not that arbitrary
Windows apps run. Persistent prefixes, Wine loader-list/TLS attach validation,
broader thread and object services, and a user-supplied copy-and-run path
remain open.
Sharing the x86-64 ISA also does not remove the Windows/native ABI boundary for
future PE64 applications.

The next public milestone is a native copy-and-run path: a shared Wine runtime,
an application manifest, an isolated persistent prefix and a user-supplied
installed application directory. Explorer, graphical installers and store
clients are not prerequisites for this first generic workflow.

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

Private applications are copied only into the ignored `dist/` tree. The
fail-closed publication audit rejects Windows binaries, captures, telemetry
transcripts, private paths and unreviewed files from the repository.

## Documentation

Start with [technical details](docs/TECHNICAL_DETAILS.md), then see
[architecture](docs/ARCHITECTURE.md), [Wine integration](docs/WINE_INTEGRATION.md),
[hardware validation](docs/HARDWARE_VALIDATION.md),
[telemetry](docs/TELEMETRY.md) and [development](docs/DEVELOPMENT.md).

Contributions are welcome; read [CONTRIBUTING.md](CONTRIBUTING.md) before
opening a change.

prospero-win is licensed under LGPL-2.1-or-later. See
[LICENSING.md](LICENSING.md) and [NOTICE.md](NOTICE.md). `PPSA99995` is a local
development identifier, not an official Sony assignment.
