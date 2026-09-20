# First playable title: Windows Space Cadet Pinball

Target: execute the owner's original x86 PINBALL.EXE through prospero-win
without recompilation, display through the PS5 GPU, accept DualSense input
and support operator-controlled closure.

Input identity: owner's executable inspected on 2026-09-08. SHA-256:
`2bbc8234685fe2f6324040af6ea20123cf00c4a56882ce0d9074f0beefac67bc`.
PE32/i386, 281088 file bytes, 307200 image bytes, required base 0x01000000,
no base relocations. Static imports: 207 symbols across eight DLLs.
GDI32 supplies BitBlt/StretchDIBits and palettes; WINMM supplies waveOut,
MMIO and MCI. No static DirectDraw/Direct3D imports were found. LoadLibraryA
and GetProcAddress require further dynamic dependency analysis. Runtime
working set remains unverified.

Current status: **first playable** on FW 12.02. This exact executable now runs
continuously through the native x86 DBT, displays changing board frames through
AGC DMA/VideoOut and sends its original WaveMix effects through SceAudioOut.
The accepted fSELF hash, telemetry and private audiovisual evidence are in
[HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md). DualSense translation,
corrected GDI composition, persistent registry reload, bounded INI parsing and
ordered teardown are implemented and hardware-observed. Physical play has
confirmed launch, both flippers, scoring, ball loss, pause/resume and new-game
restart without an unintended runtime exit. This proves the first working game
for prospero-win; it does not make Pinball the scope of the project or claim
broad Windows compatibility. Intermittent pacing and presentation polish
remain. The continuous path has already crossed ten minutes without an abort.
MIDI is optional in this target:
music defaults off and the sole startup `MCI_OPEN` disables its menu item when
the runtime reports no sequencer; original PCM effects remain available.

## DualSense gameplay profile

| DualSense | Original Win32 action |
| --- | --- |
| L1 / R1 | left (`Z`) / right (`VK_OEM_2`) flipper |
| Cross | plunger (`Space`) |
| D-pad left / right / up | table nudge (`X`, `.`, extended `Up`) |
| Options | pause/resume (`F3`) |
| Square | new game (`F2`) |
| Create | orderly runtime exit (`WM_QUIT`, not a fabricated key) |

The gameplay keys match the defaults in the pinned public semantic source.
Every input sample is consumed chronologically and held keys are released on
disconnect, interception, controller-generation change and shutdown. The
manual acceptance sequence is Cross to launch, both shoulders, scoring and
ball loss, Options twice, then Square; every check has passed on hardware.
Create is tested last because it exits.

The file's SHA-1 is
`2a5b525e0f631bb6107639e2a69df15986fb0d05`, exactly the Windows XP target
identified by the public SpaceCadetPinball reconstruction. The reproducible
`pw-source-oracle/1` report pins the original-Win32 semantic revision
`6756c54d3b17bf41cab82a822125140aed2e3120`, maintained revision
`cb9b7b886244a27773f66b0b19fdc2998392565e`, MIT license and public PDB
GUID/age. Its segment-1 offsets plus the PE `.text` RVA reproduce ten public
symbol addresses, including Ghidra's independently identified entry,
`WinMain` and window procedure. See `PINBALL_SOURCE_ORACLE.json`.

An independent host objdump inspection confirms stripped relocations,
no TLS directory and no delay-import directory. It does contain a bound
import directory and preserves the original name lookup tables. Rebind all
IAT entries from those tables; existing bound Windows addresses cannot be
called on PS5. The IAT is at RVA 0x1000 in .text, so binding must precede
final RX protection. Load-configuration and x86 exception/CRT semantics
still require inspection. Header stack reserve/commit: 256/64 KiB; heap
reserve/commit: 1 MiB/4 KiB. These are header requests, not measured usage.

## Host mapping result (2026-09-08)

The original binary above now maps through the loader at 0x01000000 without
relocations. The host run reported 307200 reserved bytes, nine graph nodes
(one image plus eight unimplemented host bindings), 75 host pages, zero WX
pages, four protection calls and balanced file ownership (one open/close).
Mapped checksum: 0x1bd76edecdb503e8 (the loader's existing checksum format).
This is host mapping evidence only: no guest instruction was executed and
no import address was bound. The subsequent PS5 result below covers the
16 KiB protection granularity versus the host's 4 KiB pages.

The VM backend now advertises optional exact-address reservation. It uses
an mmap hint, checks the returned address and releases an alternative
placement. A collision returns failure without replacing the existing
mapping. Synthetic tests hold a live sentinel-filled reservation while
attempting collision, then verify its bytes and test reuse after release.
Nonrelocatable images request their required base; relocatable fixtures
retain the existing arbitrary-placement path to keep rebase tests meaningful.

## PS5 mapping result (FW 12.02, 2026-09-08)

Run: `20260908T145242477Z_PPSA99995_prospero-win_0x1021ed623a4eb`.
Source commit: `013f328`. Native foundation: `37dd53602bdead63936f718004555ba10154be48`.

- Linked ELF SHA-256: `e2f50373caa17180a2df3b62ce191b1073105b73a2ff8e9ab35a219b5d5d836a`.
- fSELF SHA-256: `a42636fe23e3f56236f54ce1ca6426d298ea56212f3624850f7de0b1a90d05b4`.
- Transcript SHA-256: `c05b8973e6ba718a6022c67fd0ac8a5b5fe9524d715360cb40508a467cc36799`.
- Original file SHA-256 is pinned above; FTP verified all staged bytes.
- Actual/preferred base both 0x01000000; no relocations; 207 static imports.
- Image 307200 bytes; reservation 311296 bytes; 19 pages of 16384 bytes.
- Three verified sections; 279612 compared bytes; zero byte/zero-tail/alias
  mismatches; checksum 0x1bd76edecdb503e8 matches the host result.
- Three pages merge section/header contributors; one page is WX; four
  protection calls. WX remains a measured limitation, not a solved one.
- Nine graph nodes, eight host bindings, one mapped and released image;
  opens=closes=1, failures=0; clean BYE with reason pe-map-complete.
- Post-run supervisor status: no active BigApp; all four services healthy.

The independent validator accepted the manifest with:

```sh
python3 tools/validate_pe_map_evidence.py /private/path/run.json \
  --root pinball.exe --expect-modules 9 --expect-local 0 --expect-host 8 \
  --allow-i386 --allow-wx --expect-compat32 refused
```

This proves mapping the original PE32 file in the native title. It does not
prove guest execution, bound imports, rendering or playability. The gate
intentionally exits after the measurement; the eventual game runs until
operator closure.

## Inventory

Keep executable and resources outside this public project:

```sh
sha256sum /private/path/PINBALL.EXE
make inspect-only PE_INPUT=/private/path/PINBALL.EXE
```

This reads headers, layout and ordinary import names/ordinals without
mapping guest memory, resolving DLLs or executing code. Host/local labels
describe provisional policy, not Windows search-order conformance.
Malformed imports must return failure.

Record hash, version, machine, subsystem, image size, relocations, static
imports and resource filenames in a private inventory. Separately inspect
TLS, resources, delay imports and GetProcAddress/LoadLibrary calls; the
inspector does not enumerate those yet. Imports are not a full runtime trace.

## Acceptance and reference

Follow the named compatibility stages in ROADMAP.md. First output comes from
the original executable. **In-game** means simultaneous changing graphics and
original PCM; it does not mean playability. **First playable** additionally
requires operator control of ball launch, both flippers, scoring and restart
with usable timing, plus persistence, stable sessions and structured lifecycle
evidence. Video alone does not prove artifact identity, renderer completion,
audio or cleanup.

[SpaceCadetPinball](https://github.com/k4zmu2a/SpaceCadetPinball) provides
MIT-licensed reconstructed source, the public PDB dump and a modern SDL
implementation. Because the target identity and public symbol addresses match,
the pinned pre-SDL revision is a verified semantic oracle for package ordering;
Ghidra and the executable remain authoritative for instructions and ABI.
A native port is separate from Windows binary compatibility. Original
resources are not included upstream or here. Reproduce the privacy-safe report:

```sh
python3 tools/build_source_oracle.py \
  --config references/spacecadet_pinball.json \
  --reference-dir /path/to/SpaceCadetPinball --image /private/PINBALL.EXE
```

## Graphics direction

Implement observed graphics calls through a reusable AGC backend. Surface
upload/composition may combine CPU work with GPU presentation; report the
actual division. Showing Pinball does not prove Direct3D acceleration or
complete GDI/DirectDraw support.

Wine and DXVK are references or potential component sources under their
licences. D3D-to-AGC and DXVK plus Vulkan-on-AGC are later alternatives to
measure. DRM, anti-cheat and kernel drivers are out of scope.

## Reuse the Xash3D platform work

Reviewed ps5-xash3d through commit
`ff6530cdc38067b1cd210c6aa71c43f66d50a3c6`. Half-Life 1 gameplay is now
operator-validated on the owned FW 12.02 console with the native AGC renderer,
DualSense controls and live game audio. The Xash3D roadmap still keeps Phase 7
open for remaining fidelity, performance, long-soak and release work; that
polish boundary does not reduce the platform layer from a playable hardware
baseline to an unproven prototype.

The reusable design came from the public ps5-xash3d audio and input adapters;
its engine-specific SNDDMA bridge is not part of prospero-win. This project's
[hardware validation](HARDWARE_VALIDATION.md), [guest ABI](GUEST_ABI.md) and
[architecture](ARCHITECTURE.md) documents describe the adopted contracts.

- Audio: reuse the PCM ring/worker lifecycle, blocking-output ownership,
  whole 256-frame submission, resampling continuity and teardown tests.
  The current core is stereo S16, 44100-to-48000 Hz, not a general Windows
  audio backend. Adapt the formats/rates actually requested by Pinball.
- WinMM adapter: implement guest WAVEHDR layout, queue ownership,
  prepare/unprepare, pause/restart/reset and completion callbacks/messages.
  Copying PCM out of the producer ring is not proof it has played; preserve
  buffer lifetime and completion semantics. Route guest callbacks through
  the runtime rather than executing guest code on the AudioOut worker.
- MMIO and WaveOut are implemented for the target's required PCM route. MIDI
  synthesis is not fabricated: the target makes one optional `MCI_OPEN`, gets
  `MCIERR_DEVICE_NOT_INSTALLED` and continues with music disabled, matching its
  own capability fallback.
- Input: reuse chronological ScePad batches, press/release transitions,
  neutralisation on disconnect/interception/generation change and exact
  close ownership. Adapt events to Win32 key/mouse messages and window
  callbacks, rather than Xash movement actions. Pinball has no static
  DirectInput import, so a DirectInput implementation is not the first task.
- Preserve the tested contracts and ps5log/1 counters. Adapted backends need
  their own prospero-win hardware evidence; Xash evidence is the baseline.

This reuse claim covers PS5 platform behavior, not Win32 behavior: prospero-win
must still implement WAVEHDR/callback, keyboard/message and guest-thread ABIs.
It should not rebuild the already validated AGC, ScePad, SceAudioOut, memory,
clock or teardown foundations from scratch.

The source files currently declare GPL-3.0-or-later; prospero-win declares
LGPL-2.1-or-later. Before extracting source, resolve and document compatible
component licensing/provenance. No source has been copied by this review.
