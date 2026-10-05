# Provenance and attribution

## Licence

prospero-win is LGPL-2.1-or-later; see `LICENSE` and `LICENSING.md`. Every
source file carries an SPDX identifier. `native/pw_videoout_ps5.c` also
includes code adapted from SDL under the zlib licence (see below), and the
`wine_sync_ordinary` test program and its build script are MIT-licensed, as
their SPDX lines say.

## Independently authored

The IA-32 translator, its memory contract, the title, its launcher, the
profile parsers, the presentation and pad code, the Wine PS5 shims
(`wine/ps5`), the host tools and the synthetic PE encoder in this repository
are written for this project. No proprietary SDK file is copied into it.

## Derived from Wine

Wine is LGPL-2.1-or-later, like this project. The patches in `wine/patches`
apply to the pinned upstream revision at build time. Two modules are derived
from Wine source and say so in their headers, keeping the original copyright
and licence notices:
the PE side of the WoW64 CPU backend (`wine/wowprospero/cpu.c`, from
`dlls/wow64cpu`) and the PS5 audio driver (`wine/wineps5`, whose stream,
buffer and timing code follows `dlls/wineoss.drv`).

The PE/COFF structures the test encoder writes are described by Microsoft's
published PE format specification. Only field offsets and semantics are
used; no Microsoft code, header or binary is included.

`references/spacecadet_pinball.json` pins the MIT-licensed public
[SpaceCadetPinball](https://github.com/k4zmu2a/SpaceCadetPinball) history and
PDB dump as an external semantic reference. The checked source-oracle manifest
contains commit/file identities, public symbol addresses and aggregate API
name occurrences only. No source file, PDB, game resource or executable is
vendored. Copyright in that external project remains with Andrey Muzychenko
and its contributors.

## Reused components

- `native/ps5log/` is the `ps5log/1` logging client the native runtime uses
  to stream its log lines to a PC. It was written by prospero-win's author
  for his own PS5 homebrew tools and is included here under this project's
  LGPL-2.1-or-later. It is pinned by SHA-256 in `tools/audit_publication.py`;
  an intentional update must revise the client and its recorded digests
  together.
- The pixel-address permutation in `native/pw_videoout_ps5.c`, which turns a
  linear frame into the tiled layout VideoOut scans out, is adapted from the
  tilemap code of SDL's PS5 video backend, Copyright (C) 2026 John Törnblom,
  under the zlib licence. The file keeps that notice in full; the rest of it is
  this project's own code.
- The native shell, linker script, CRT and signing tool come from
  [BlackBearReloaded's PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
  pinned by commit in `tools/build_native.sh` and fetched at build time.
  None of it is vendored here.

## Measured platform facts reused

Several design decisions follow limits measured on the console on FW 12.02
while porting other software, before this project started: the libc heap
ceiling and the resulting use of anonymous mappings for large allocations,
the unusability of libc directory listing on the read-only application
image, the measured support for read-write to read-execute transitions, and
the rule that a title must not return from `main()`. Each is cited where it shapes the
code.

## Not included

No game data, no third-party DLL, no firmware dump, no disassembly and no
decompiled output. Windows binaries are private build inputs supplied by
path at build time.
