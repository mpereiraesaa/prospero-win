# Game compatibility

The maintainer results below were tested on one PS5 on firmware **12.02**.
Player reports are listed separately with their own firmware and tested
scope. The app itself uses no firmware-specific offsets.

Profiles, controller presets and install recipes live in
[prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles).

## Games

| Game | Status | Code | Graphics | Controls |
| --- | --- | --- | --- | --- |
| Half-Life | Playable, above 60 fps | 32-bit | OpenGL | USB keyboard/mouse |
| Counter-Strike 1.6 | Playable, above 60 fps | 32-bit | OpenGL | USB keyboard/mouse |
| OpenArena 0.8.8 | Playable, above 60 fps | 32-bit | OpenGL | USB keyboard/mouse |
| Warcraft III: Reign of Chaos 1.27a | Playable, above 60 fps | 32-bit | DXVK / RADV | DualSense, USB keyboard/mouse |
| Half-Life 2 | Playable, 60 fps at High settings | 32-bit | DXVK / RADV | DualSense, USB keyboard/mouse |
| Grand Theft Auto IV: The Complete Edition | Playable, about 55 fps in the city | 32-bit | DXVK / RADV | DualSense |
| Space Cadet Pinball | Playable | 32-bit | GDI | DualSense |
| Wine Minesweeper | Playable | 64-bit, native | GDI | DualSense |

64-bit programs run natively on the console's x86-64 CPU; 32-bit ones run
through prospero-win's x86 translator.

Half-Life 2 was played from the train through Kleiner's lab with the
DualSense, and an automated run reached the canals; both held 60 fps, which
is the console's 60 Hz output, with High texture, model and water detail, 4x
MSAA, 16x anisotropic filtering and HDR. One known issue: the menu logo shows
an apostrophe where its ² should be.

Grand Theft Auto IV: The Complete Edition runs its open city at 1920x1080
on the console's 60 Hz output at roughly 54–55 fps. Quiet streets come close
to 59 fps; gunfights and crowded areas dip into the mid-40s. Getting into
the game takes about 90–95 seconds of loading. The FusionFix mod works too,
but it needs Microsoft's own D3DX9 and D3DCompiler DLLs, and with its default
settings the city runs about 22% slower. The game's profile in
prospero-win-profiles explains the setup and the optional mods.

OpenGL games draw through Mesa's Zink on top of Vulkan; see
[the OpenGL build notes](docs/WINE_PS5_BUILD.md#opengl).

## Player reports

These results come from the linked reports and have not been independently
reproduced by the maintainer.

### Kerbal Space Program 1.12.5

[Report #686](https://github.com/mpereiraesaa/prospero-win/issues/686)
covers the base game, Windows 64-bit, on firmware **13.60** with EtaHEN,
Kstuff lite and ShadowMount+. The player names prospero-win **v0.2.0**;
the attached session log identifies title build
`644e32e400c555e340d507383398c5d21b39b0b0`.

With DXVK, USB keyboard and mouse, the player reached the main menu, used
the Vehicle Assembly Building and completed a short test flight. Sound,
icons, menus and saves were reported working. The frame counter read 60 at
1280x720; no 1080p frame-rate measurement or extended play duration was
provided. Slight graphical artifacts were reported, with the cause unknown.

On first launch, KSP's `settings.cfg` selected 1280x720 despite the profile's
1920x1080 desktop and launch arguments. With the game closed, setting
`SCREEN_RESOLUTION_WIDTH = 1920` and `SCREEN_RESOLUTION_HEIGHT = 1080` in
that file corrected the oversized UI, soft picture and confined mouse
pointer. The mouse wheel remains unsupported; use keyboard alternatives
for zoom and scrolling ([controls](docs/CONTROLS.md#usb-keyboard-and-mouse)).

## Benchmarks

7-Zip 25.01, nbench and Ooura's pi program completed on 2026-09-28 in PE32
and PE64 variants. These are controlled workloads, not game performance
predictions. See [the benchmark method and results](docs/DBT_BENCHMARK.md).

## Add or update a result

Open a [game report](https://github.com/mpereiraesaa/prospero-win/issues/new?template=game-report.yml)
with the game version, runtime version, firmware and controls, and attach a
log. Send profile and recipe changes to prospero-win-profiles.
