# Case study: Space Cadet Pinball

Space Cadet Pinball is prospero-win's first playable compatibility target. It
is useful because one unchanged PE32 application exercises the loader, IA-32
DBT, Win32 imports, GDI presentation, PCM audio, input, persistence and orderly
shutdown. It is a regression target, not the architecture or scope of the
project.

The original Windows executable and its resources are not distributed here.
Testing uses an owner-supplied installation whose identity is checked before
building a native package.

## Verified result

On an owned PS5 running firmware 12.02, the original executable runs without
recompilation through the native PE32 path:

- its IA-32 code executes through the dynamic binary translator;
- software GDI composition is uploaded and presented through AGC/VideoOut;
- original PCM effects reach SceAudioOut through the asynchronous audio queue;
- DualSense events enter the Win32 message path;
- registry state survives a title restart; and
- operator close performs ordered video, audio, input and memory teardown.

Physical play has verified launch, both flippers, scoring, ball loss,
pause/resume and new-game restart. The accepted artifact and telemetry
requirements are recorded in [hardware validation](HARDWARE_VALIDATION.md).
This result does not imply general GDI, WinMM or Windows compatibility, and it
does not demonstrate Direct3D.

## Input profile

| DualSense | Win32 action |
| --- | --- |
| L1 / R1 | left / right flipper |
| Cross | plunger |
| D-pad left / right / up | table nudge |
| Options | pause or resume |
| Square | new game |
| Create | orderly `WM_QUIT` |

Events preserve press/release order. Held keys are neutralized on disconnect,
controller-generation change and shutdown; Create is a lifecycle action rather
than a fabricated guest key.

## What the target taught the runtime

The executable has no static DirectDraw or Direct3D dependency. Its visible
path uses GDI operations such as `BitBlt` and `StretchDIBits`, while audio uses
WinMM/WaveMix PCM. That made it possible to validate the CPU and native
platform layers before the separate DXVK and `ps5-vulkan` path is ready.

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

Pinball currently uses the direct Win32 bootstrap described in
[technical details](TECHNICAL_DETAILS.md). New application support is intended
to use Wine's PE modules and the common native service boundary instead of
adding title-specific wrappers. The validated AGC, AudioOut, Pad, telemetry and
teardown ownership rules remain reusable in both paths.
