# Game compatibility

The homebrew is firmware agnostic and uses no firmware-specific offsets.
Firmware below identifies the reported test environment, not an app requirement.
These are reported console results, not promises that a whole game works.
All reports below come from one PS5 on firmware **12.02**. Other firmware,
long sessions, multiplayer and complete campaigns remain unverified.

Profiles and recipes live in
[prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles).
The snapshot below was checked against its `main` revision
`3571b268f9bd4104d301cee58e1264577d699ddf` on 2026-10-01. Profile validation
checks configuration syntax; it does not prove gameplay compatibility.

## Reported games

| Game / profile | Backend | Tested scope | Controls reported | Date / limitations |
| --- | --- | --- | --- | --- |
| [Wine Minesweeper](https://github.com/mpereiraesaa/prospero-win-profiles/blob/3571b268f9bd4104d301cee58e1264577d699ddf/profiles/minesweeper.profile) | GDI, PE64 | Played | Stick pointer | Date and exact runtime identity not recorded in the profiles table |
| [Space Cadet Pinball](https://github.com/mpereiraesaa/prospero-win-profiles/blob/3571b268f9bd4104d301cee58e1264577d699ddf/profiles/pinball.profile) | GDI, PE32 | Played fullscreen | DualSense | Game edition, date and exact runtime identity not recorded in the profiles table |
| [Warcraft III: Reign of Chaos 1.27a](https://github.com/mpereiraesaa/prospero-win-profiles/blob/3571b268f9bd4104d301cee58e1264577d699ddf/profiles/warcraft-iii-reign-of-chaos.profile) | DXVK 2.6.2 / RADV, PE32 | Menu, skirmish, cinematics with sound; widescreen | DualSense `warcraft3`, USB keyboard/mouse | 2026-09-29; no measured game performance or full campaign completion |
| [OpenArena 0.8.8, official Windows build](https://github.com/mpereiraesaa/prospero-win-profiles/blob/3571b268f9bd4104d301cee58e1264577d699ddf/profiles/openarena-088.profile) | OpenGL, PE32 | `aggressor` match with bots; no rejected draw or present failure | USB keyboard/mouse | 2026-10-01; other maps and multiplayer unverified |
| [Half-Life 1](https://github.com/mpereiraesaa/prospero-win-profiles/blob/3571b268f9bd4104d301cee58e1264577d699ddf/profiles/half-life.profile) | OpenGL, PE32 | `c1a0` scene, scripted movement and audio | USB keyboard/mouse; profile selects XInput | 2026-10-01; edition not recorded, campaign progression and controller gameplay unverified |
| [Counter-Strike 1.6](https://github.com/mpereiraesaa/prospero-win-profiles/blob/3571b268f9bd4104d301cee58e1264577d699ddf/profiles/counter-strike-16.profile) | OpenGL, PE32 | Main menu, core fonts, 1920x1080 at 60 fps with vsync | USB keyboard/mouse | 2026-10-01; profile reports a non-Steam Cataclysm 1.04 package; matches and networking unverified |

**Save/load, sustained play and orderly exit have not been recorded per game
in this table.** A playable report must not be read as verification of these
features. No tagged public runtime release was available at this snapshot;
these results describe development builds. Use a release's own validation
record to determine which results apply to its exact artifacts.

OpenGL games require a runtime built with the optional PS5 OpenGL SDK and
the legacy compatibility support described in
[the OpenGL build notes](docs/WINE_PS5_BUILD.md#opengl).
The combined OpenGL/RADV run is documented there; this does not establish
that every build supports both backends.

## Benchmarks

7-Zip 25.01, nbench and Ooura's pi program completed on 2026-09-28 in PE32
and PE64 variants. These are controlled workloads, not game performance
predictions. See [the benchmark method and results](docs/DBT_BENCHMARK.md).

## Add or update a result

Open a [game report](https://github.com/mpereiraesaa/prospero-win/issues/new?template=game-report.yml)
with the game edition/version, runtime version, firmware, profile revision,
graphics backend, controls, steps played, duration, save/load and exit
results, and a diagnostic attachment. Report where testing stopped even if
the game worked up to that point. A menu-only result stays menu-only until
gameplay is tested.

Submit profile/recipe changes to prospero-win-profiles and runtime problems
to prospero-win. Compatibility updates should link to the report, preserve
earlier limitations until retested, and name the release or full source
commit and artifact hashes. Never replace missing evidence with a guessed
version or test result.
