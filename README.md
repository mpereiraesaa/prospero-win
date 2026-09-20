# prospero-win

prospero-win is an experimental Windows compatibility runtime for PlayStation
5 homebrew. Its target architecture runs Wine's Windows subsystem on PS5:
PE32 code uses the project's IA-32 dynamic translator, PE64 will use native
x86-64 plus ABI bridges, and platform services connect Wine to the console.
Direct3D will run through DXVK over the companion `ps5-vulkan` project.

The first compatibility target is the original Windows Space Cadet Pinball
executable, running without recompilation. Pinball is a bring-up target for the
general runtime, not a project-specific architecture. DRM, anti-cheat, kernel
drivers and distribution of proprietary game files are out of scope.

## Current status

On an owned PS5 running firmware 12.02, the direct Win32 bootstrap makes
Pinball the first playable title:

- the PE32 image executes through the IA-32 dynamic binary translator;
- GDI output is composed and presented at 1920x1080 through AGC and VideoOut;
- WinMM and WaveMix PCM reaches SceAudioOut through an asynchronous worker;
- DualSense input is translated into Win32 key messages;
- registry state persists in title-owned storage;
- close and relaunch work without rebooting the console.

The DBT includes hashed block lookup, direct block chaining, cross-block guest
register residency, dead-flag elimination and real lazy arithmetic flags.
Resident ALU operations execute directly in `r8d`-`r10d`; tiny and helper-heavy
blocks stay canonical when residency would only add transitions. Exact mode
matrices finish with identical guest CPU state. Bounded hardware runs also
reach video, audio and input teardown cleanly.

The reusable Wine path now has an executable host checkpoint. A pinned i386
Wine distribution contains `ntdll`, `kernelbase`, `kernel32` and the revision's
76 NLS data files, all covered by a validated manifest. prospero-win publishes
Wine's syscall and Unix-call dispatchers, services 32 NT call shapes, maps
runtime and application-local images from separate namespaces, and constructs
the initial PE32 process/thread state.

A generated public PE32 executable with two DLLs and a dependency diamond now
runs through Wine's own loader, reaches its own entry point under the DBT,
returns `1`, and exits through `NtTerminateThread`. The accepted host run uses
all four chaining/residency combinations and retires exactly 598,404 guest
instructions over 2,981 translated blocks in each. Chaining cuts dispatcher
returns from 120,931 to 15,787. With chaining and residency enabled, the
measured resident-state path performs 12,656 canonical stores and 61,805
reconciliation spills; both counters and emitted code size are pinned by the
host gate so future allocator work is measurable.

This is not yet a Wine boot on PS5 or broad Windows compatibility. Wine's
loader lists and attach ordering still need independent validation, the prefix
is not persistent, and general threads, objects and waits remain incomplete.
Pinball still uses the hardware-validated direct bootstrap, PE64 application
execution is incomplete, and Direct3D awaits DXVK over `ps5-vulkan`.

Building Wine successfully is an important input, not a loadable PS5 runtime by
itself. PE32 Wine and applications still enter through the DBT, while PE64 will
need a Windows-to-native calling-convention, exception and callback bridge even
though both sides use x86-64 instructions. Wine's Unix-facing services must
also be connected to PS5-native VM, file, thread, object and lifecycle adapters.

The next release target is a native **copy-and-run** path: stage the shared Wine
runtime in the title, copy an already installed portable application into a
title-owned `drive_c`, select its EXE through a manifest, boot it through Wine,
persist its prefix, then close and relaunch cleanly. A desktop shell, graphical
installer and store client are not prerequisites for that milestone.

## Build and test

```sh
make all
make sanitize
make inspect-only PE_INPUT=/private/path/APPLICATION.EXE
```

To build a native package, provide your own legally obtained Windows files
from an external directory:

```sh
PW_FOUNDATION_READY=1 \
PW_STAGE_INPUT=/private/path/application \
PW_ROOT_MODULE=application.exe \
tools/build_native.sh
```

Private applications are copied only into the ignored `dist/` build tree.
The fail-closed publication audit rejects Windows binaries, captures, telemetry
transcripts, private paths and unreviewed files from the repository.

## Documentation

Start with the [documentation index](docs/README.md), then see the
[architecture](docs/ARCHITECTURE.md), [compatibility roadmap](docs/ROADMAP.md),
[Wine integration](docs/WINE_INTEGRATION.md),
[hardware evidence](docs/HARDWARE_VALIDATION.md) and
[development workflow](docs/DEVELOPMENT.md).

Contributions are welcome; read [CONTRIBUTING.md](CONTRIBUTING.md) before
opening a change.

prospero-win is licensed under LGPL-2.1-or-later. See
[LICENSING.md](LICENSING.md) and [NOTICE.md](NOTICE.md). `PPSA99995` is a
local development identifier, not an official Sony assignment.
