# Documentation

This directory contains the public technical documentation for prospero-win.
Raw captures, telemetry transcripts, reverse-engineering workspaces,
proprietary inputs, agent reports, chronological bring-up notes and internal
planning stay outside the standalone repository.

## Start here

- [Technical details](TECHNICAL_DETAILS.md): current implementation, execution
  paths, boundaries and known limitations.
- [Architecture](ARCHITECTURE.md): component ownership, memory model and native
  adapter contracts.
- [Wine integration](WINE_INTEGRATION.md): Wine, PE32/PE64 and DXVK boundaries.
- [Development](DEVELOPMENT.md): local gates and native build workflow.
- [Hardware validation](HARDWARE_VALIDATION.md): accepted claims and evidence
  requirements.
- [Telemetry](TELEMETRY.md): the `ps5log/1` runtime contract and validators.
- [Pinball case study](CASE_STUDY_PINBALL.md): the first playable PE32 target
  and the reusable contracts it validates.

`SUPPORT_MATRIX.json` is a machine-readable view of current component support
and dependencies. It contains evidence, not an internal delivery plan.

Historical phase plans and day-to-day compatibility notebooks are deliberately
not part of the public documentation surface.
