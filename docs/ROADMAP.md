# Public direction

prospero-win does not publish its internal task queue or chronological
bring-up plan. The public direction is intentionally concise:

- keep Pinball (PE32) and Minesweeper (PE64) stable as hardware regressions;
- extend compatibility from evidence produced by independent applications,
  fixing gaps in Wine's PS5 patches and drivers rather than per title;
- make startup and frame pacing competitive through the DBT and the
  presentation path, measured with exact control comparisons;
- use DXVK over `ps5-vulkan` for Direct3D rather than building another D3D
  implementation.

Current capabilities and limitations are documented in
[ARCHITECTURE.md](ARCHITECTURE.md) and
[HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md). Only completed, repeatable
results are promoted into the public status in the project README.
