# Architecture

prospero-win runs Wine itself inside a native PS5 title. Wine owns the Windows
subsystem; prospero-win owns i386 execution (the DBT behind Wine's WoW64
layer) and the platform boundary: VideoOut, the DualSense, AudioOut, files
and memory.

```text
PE32 application + Wine i386 PE modules            guest IA-32
  -> wowprospero: the prospero-win DBT              Wine's WoW64 CPU backend
     -> wow64 / wow64win thunks                     Wine x86_64 PE modules
        -> ntdll.prx, win32u.prx, wineserver.prx    Wine's Unix side, native PS5
           -> PS5 user driver, audio driver, XInput -> the title's sinks
              -> VideoOut, AudioOut, Pad            native/ adapters

PE64 application -> Wine x86_64 PE modules -> the same Unix side
```

## Repository layout

```text
native/            the title
  wine64_main.c       launcher, one game per process, Wine start, frame/pad/audio loop
  pw_wine_library     profiles and input presets from /data/prospero-win
  pw_wine_display     frame box, pointer, bindings and XInput state between Wine and the title
  pw_videoout_ps5     scanout, tiling and scaling into VideoOut (AGC copy and flip)
  pw_pad_ps5          DualSense ownership, edges, sticks, triggers, rumble
  pw_audio_ps5        the main AudioOut port Wine's driver plays on
  pw_data_mount       requests /data from the Lapy JB daemon
  ps5log/             vendored `ps5log/1` client, pinned by digest

src/               portable code, host-tested
  pw_x86_*, pw_x87, pw_guest_fp, pw_vm*   the IA-32 DBT and its memory backend
  pw_game_profile, pw_app_profile          profile and input-preset parsing
  pw_profile_catalog                       the profiles.lst index
  pw_wine_launch, pw_wine_start            launch arguments, starting ntdll.prx
  pw_launcher_render, pw_present           the launcher picture, frame placement and scaling
  pw_pad, pw_audio_mix                     pad edge tracking, the Wine audio mixer

wine/
  patches/            the PS5 patch series (docs/WINE_PS5_BUILD.md)
  ps5/                PRX loader, heap, threads, working directory, compat and sink shims
  wowprospero/        Wine's i386 CPU backend around the DBT
  wineps5/            Wine's PS5 audio driver

examples/wine/     profiles and input presets to copy to /data/prospero-win
```

The title is built by `tools/build_native.sh`; Wine's PRXs by
`tools/build_wine_ps5.sh`, and the host WoW64 build it needs by
`tools/build_wine_runtime.sh` ([development](DEVELOPMENT.md)).

## The title

The title runs one game per process. Started by the system it shows the
launcher without loading Wine; choosing a game restarts the title with
`sceSystemServiceLoadExec` and the game's arguments, and closing the game
restarts it into the launcher, so every game starts Wine in a clean process.
The launcher stays in the title's sandbox and reads a copy of the library in
`/download0/prospero-win`, which a game (granted `/data`) or a `sync=1` run
writes. [WINE_PS5_BUILD.md](WINE_PS5_BUILD.md#starting-wine-in-the-title)
describes the profiles, the prefix layout, scaling, bindings and closing.

In a game, the title loads `ntdll.prx`, starts `__wine_main` on its own
thread and becomes the platform side of Wine's drivers: the PS5 user driver
hands it frames and the cursor, the audio driver hands it mixed grains, and
the xinput patch reads its pad state. The main thread shows frames, reads the
pad and reports telemetry ([TELEMETRY.md](TELEMETRY.md)).

## Isolation boundaries

The packaged title supplies the outer process and filesystem boundary.
`/app0` is the immutable application image; writable storage is the title's
`/download0` and, when granted, `/data/prospero-win`. If one title hosts
several Windows applications, they share that sandbox; prospero-win does not
create a jail per program.

PE32 code passes through the DBT, which validates the code it translates
and publishes translated code W^X. PE64 code runs directly on the host CPU
and shares the title's address space, so it is limited to trusted inputs.
Native speed is not itself an isolation boundary.

## Why the portable code imports almost nothing

`src/` includes only `<stddef.h>`, `<stdint.h>`, `<string.h>` and
`<limits.h>`. It has no allocator, no formatter and no case-folding helper
from the platform.

On this firmware a system library exports many symbols that are placeholders
or subtly wrong, and treating "provided by `libSceLibcInternal`" as a green
check has already cost a full session. The cheapest way not to inherit that
class of bug is not to import the symbol: names are compared with an in-tree
ASCII fold, and memory comes from the injected `PwVmBackend`, never from
`malloc`. `tests/test_native_contract.py` fails if a portable source includes
an unexpected header or calls a forbidden symbol. Code that needs threads or
the platform lives in `native/` or `wine/ps5/`.

## Translated block ownership

`PwX86Engine` owns one executable arena and fixed-capacity cache metadata.
Compilation takes the maximal valid prefix up to the explicit block limits,
emits while the arena is writable and publishes the entry only after copying.
Blocks retain every guest instruction end offset, so a fault midway reports
only the retired prefix. Lookup uses an open-addressed table keyed by guest
PC; direct jumps and calls are chained, and returns and indirect jumps probe
the table from translated code.

`wowprospero` keeps one engine per host thread; two host threads never
execute or mutate the same `PwX86State`. A memory change that touches pages
translations were made from discards only the affected translations
(`wine/wowprospero/code_pages.h`). Guest register residency and lazy flags
are reconciled before control leaves translated code.

Instructions the translator does not cover are executed by
`pw_x86_hostexec`, which rewrites them for the x86-64 host inside a stub that
loads and stores the guest state; `tools/dbt_differential` checks both paths
against each other ([Wine integration](WINE_INTEGRATION.md)).
