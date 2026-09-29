# Case study: Space Cadet Pinball

Space Cadet Pinball is prospero-win's first playable compatibility target. It
is useful because one unchanged PE32 application exercises the IA-32 DBT
under Wine's WoW64 layer, GDI through Wine's PS5 user driver, PCM audio,
input, persistence and orderly shutdown. It is a regression target, not the
architecture or scope of the project.

The original Windows executable and its resources are not distributed here.
Testing uses an owner-supplied installation whose identity is checked before
building a native package.

## Verified result

On an owned PS5 running firmware 12.02, the original executable runs without
recompilation through Wine in the title: its IA-32 code and Wine's i386
modules execute through the DBT, Wine's PS5 user driver hands its frames to
the title, which scales them to the full screen, and DualSense buttons reach
it as keys through the profile's bindings. It was first made playable on the
earlier direct Win32 runtime, since removed. The accepted artifact and
telemetry requirements are recorded in
[hardware validation](HARDWARE_VALIDATION.md). This result does not imply
general Windows compatibility, and it does not demonstrate Direct3D.

## Input profile

The `pinball` profile in [prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles) uses the shared
preset `input/pinball.input`:

| DualSense | Key |
| --- | --- |
| L1 / R1 | Z / slash: left / right flipper |
| Cross | space: plunger |
| D-pad left / right / up | X / period / up: table nudge |
| Options | F3: pause or resume |
| Square | F2: new game |

Holding Options+Create closes the game (Alt+F4) and returns to the launcher.

## What the target taught the runtime

The executable has no static DirectDraw or Direct3D dependency. Its visible
path uses GDI operations such as `BitBlt` and `StretchDIBits`, while audio uses
WinMM/WaveMix PCM. That made it possible to validate the CPU and native
platform layers before the separate DXVK path over `ps5-vulkan` or RADV was
ready.

The target also provided realistic instruction coverage for the PE32 DBT. The
sanitized aggregate fixture is stored at
[`tests/fixtures/pinball_x86_coverage.json`](../tests/fixtures/pinball_x86_coverage.json),
and the x87 regression test requires every observed startup form to remain
translated. The public source-oracle fixture at
[`tests/fixtures/pinball_source_oracle.json`](../tests/fixtures/pinball_source_oracle.json)
contains only public symbol identities and aggregate source references.

The maintained MIT-licensed
[SpaceCadetPinball](https://github.com/k4zmu2a/SpaceCadetPinball) project was
used as a semantic reference for high-level package and message behavior.
Ghidra analysis of the owner-supplied executable remained authoritative for
machine instructions and ABI. No executable bytes, decompiler output or game
resources are included in this repository.

## Architectural boundary

Nothing in the runtime is specific to Pinball: its bindings are a profile and
an input preset. Gaps it exposes are fixed in the DBT or in Wine's PS5
patches and drivers, where other applications benefit too, never with
title-specific code.
