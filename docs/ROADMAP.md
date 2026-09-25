# Public direction

prospero-win does not publish its internal task queue or chronological
bring-up plan. The public direction is intentionally concise:

- keep the first playable PE32 target stable as a hardware regression;
- extend the validated native Wine smoke from its generated PE32 fixture to
  user-supplied applications through the PS5 platform boundary;
- provide isolated persistent prefixes and a manifest-driven copy-and-run
  workflow for user-supplied applications;
- extend compatibility from evidence produced by independent applications;
- use DXVK over `ps5-vulkan` for Direct3D rather than building another D3D
  implementation; and
- add PE64 execution after its Windows/native ABI, loader and exception
  contracts are complete.

Current capabilities and limitations are documented in
[TECHNICAL_DETAILS.md](TECHNICAL_DETAILS.md). Only completed, repeatable results
are promoted into the public status in the project README. Detailed scheduling,
experiments and unpublished compatibility targets remain outside this
repository.
