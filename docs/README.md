# Documentation

## Playing

- [Getting started](GETTING_STARTED.md): what you need on the PS5, installing
  the app, your first game, getting a log.
- [Installing games](INSTALLING_GAMES.md): installing a game on your PC with
  a Lutris recipe and copying it to the PS5.
- [Controls](CONTROLS.md): the DualSense presets, Xbox controller mode, USB
  keyboard and mouse.

## How it works

- [Architecture](ARCHITECTURE.md): the layers, the repository, the app and
  the x86 translator.
- [Wine integration](WINE_INTEGRATION.md): Wine's WoW64 layer with the
  translator as its 32-bit CPU, profiles and prefixes, graphics.
- [Wine on the PS5](WINE_PS5_BUILD.md): building Wine for the PS5, the patch
  series, the display, sound and controller drivers, and the console
  bring-up notes.
- [Telemetry](TELEMETRY.md): the log the app sends, record by record.

## Working on it

- [Development](DEVELOPMENT.md): checks and builds.
- [Hardware validation](HARDWARE_VALIDATION.md): what counts as working on
  the console, and the evidence behind it.
- [DBT benchmark](DBT_BENCHMARK.md): how fast the translator is, measured
  against native code.
- [Box86 opcode catalog](BOX86_OPCODE_CATALOG.md): the translator's
  instruction coverage checklist.
- [Pinball case study](CASE_STUDY_PINBALL.md): the first game that ran.
