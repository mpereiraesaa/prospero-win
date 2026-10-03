# Game compatibility

Tested on one PS5 on firmware **12.02**. The app itself uses no
firmware-specific offsets.

Profiles, controller presets and install recipes live in
[prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles).

## Games

| Game | Status | Graphics | Controls |
| --- | --- | --- | --- |
| Half-Life | Playable, above 60 fps | OpenGL | USB keyboard/mouse |
| Counter-Strike 1.6 | Playable, above 60 fps | OpenGL | USB keyboard/mouse |
| OpenArena 0.8.8 | Playable, above 60 fps | OpenGL | USB keyboard/mouse |
| Warcraft III: Reign of Chaos 1.27a | Playable, above 60 fps | DXVK / RADV | DualSense, USB keyboard/mouse |
| Space Cadet Pinball | Playable | GDI | DualSense |
| Wine Minesweeper | Playable | GDI | DualSense |
| Half-Life 2 | Playable at 60 fps from the train through Kleiner's lab; an automated run reached the canals at 60 fps. The menu logo shows an apostrophe instead of its ² | DXVK / RADV | DualSense, USB keyboard/mouse |

OpenGL games need a runtime built with the optional PS5 OpenGL SDK; see
[the OpenGL build notes](docs/WINE_PS5_BUILD.md#opengl).

## Benchmarks

7-Zip 25.01, nbench and Ooura's pi program completed on 2026-09-28 in PE32
and PE64 variants. These are controlled workloads, not game performance
predictions. See [the benchmark method and results](docs/DBT_BENCHMARK.md).

## Add or update a result

Open a [game report](https://github.com/mpereiraesaa/prospero-win/issues/new?template=game-report.yml)
with the game version, runtime version, firmware and controls, and attach a
log. Send profile and recipe changes to prospero-win-profiles.
