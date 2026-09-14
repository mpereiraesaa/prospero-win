# Hardware validation

This document records the public acceptance boundary. Raw telemetry, Remote
Play captures, proprietary input files and operator notes are not repository
inputs.

## Environment

- Owned PlayStation 5 running firmware 12.02.
- Native title identity: `PPSA99995` / `prospero-win`.
- First compatibility target: an original, privately supplied Space Cadet
  Pinball PE32 installation.
- Structured evidence: `ps5log/1` over TCP, correlated with exact artifact
  hashes and optional private Remote Play observation.

## First-playable result

The target reaches its complete table, animated game state and original PCM
effects. Physical testing confirmed plunger launch, both flippers, scoring,
ball loss and replacement, pause/resume and new-game restart. Persistent
registry state was saved and reloaded across launches.

The native path reports:

- PE32 mapping and relocation followed by IA-32 DBT execution;
- changing GDI frames presented through AGC DMA and VideoOut;
- non-empty WinMM/WaveMix PCM converted and submitted to SceAudioOut;
- chronological connected Pad samples;
- bounded cache, heap, window, surface, audio-queue and message storage;
- orderly release of Pad, AudioOut, GDI, VideoOut, AGC memory, DBT, PE images
  and guest virtual memory.

This establishes a first-playable compatibility target, not broad Windows
compatibility or pixel-perfect rendering.

## Current dynarec acceptance

Direct chaining, cross-block guest-register residency and lazy arithmetic
flags were tested both independently and as one stack. Exact private host
traces finish with identical CPU state in eager and lazy modes at 461,087
unchained steps and 1,609,088 chained steps.

The public generated-Wine gate adds a four-mode chaining/residency parity
matrix: every mode exits through `NtTerminateThread` after exactly 598,404
guest instructions and 2,981 blocks. This is host-side DBT evidence; a native
Wine process on PS5 remains a separate hardware milestone.

The final bounded lazy-flags candidate used:

- linked ELF SHA-256
  `eb0e1ebf80055673061a004df84400b30d565fee49bb0edfa36a4338aebb97bd`;
- fSELF SHA-256
  `32c2768dfb9fce90944952b5d80d92741de149e3c8fb873ec1243d6a88114373`;
- chaining, residency and lazy flags enabled with a 64-block quantum;
- a 30-second validation-only deadline.

The strict runtime validator accepted the run after 115,502,074 retired guest
instructions, 273 presented frames, 938 complete audio blocks, connected Pad
samples and 2,073,632 measured deferred-flag safepoint commits at the final
heartbeat. Teardown reported success for every owned subsystem and ended with
a gap-free `BYE reason=validation-deadline`.

Repeated Pinball A/B runs did not establish a percentage speedup because the
changing simulation state varied the total work more than the eager/lazy
difference. Controlled host benchmarks are near-neutral. The accepted claim is
correctness, real target use of deferred state and no observed hardware
regression.

## Evidence rules

A screenshot or video proves appearance only. Runtime acceptance requires:

1. exact linked-ELF and fSELF identity;
2. exact package deployment and a fresh title mount after content changes;
3. a gap-free `ps5log/1` stream with the expected title and application;
4. changing AGC/VideoOut frames and completed AudioOut work;
5. no classified abort, signal or ownership error;
6. either an orderly in-process `BYE` or an explicitly classified external
   system close.

Run `tools/validate_runtime_evidence.py` against the captured transcript.
Private application files and raw evidence must remain outside the repository.
