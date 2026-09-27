# Documentation

This directory contains the public technical documentation for prospero-win.
Raw captures, telemetry transcripts, reverse-engineering workspaces,
proprietary inputs, agent reports, chronological bring-up notes and internal
planning stay outside the standalone repository.

## Start here

- [Architecture](ARCHITECTURE.md): layers, repository layout, the title, the
  DBT's ownership rules.
- [Wine integration](WINE_INTEGRATION.md): Wine's WoW64 layer with the DBT as
  its i386 CPU, measured platform facts, profiles and prefixes, graphics.
- [Wine on the PS5](WINE_PS5_BUILD.md): building Wine's Unix side, the patch
  series, the user, audio and XInput drivers, starting Wine in the title.
- [Development](DEVELOPMENT.md): local gates and builds.
- [Hardware validation](HARDWARE_VALIDATION.md): accepted claims and evidence
  requirements.
- [Telemetry](TELEMETRY.md): the title's `ps5log/1` records.
- [Pinball case study](CASE_STUDY_PINBALL.md): the first playable PE32 target.
- [Box86 opcode catalog](BOX86_OPCODE_CATALOG.md): generated DBT coverage
  checklist.

Historical phase plans and day-to-day compatibility notebooks are deliberately
not part of the public documentation surface.
