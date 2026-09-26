# Prefix registry storage design

This note separates the current in-memory registry adapter and `registry.pwrg`
snapshot from Wine's on-disk prefix registry. It describes the pinned Wine
reference at commit
[`490f6d5dcbb2a5047345b8af88d114bbcaad69a8`](https://github.com/wine-mirror/wine/tree/490f6d5dcbb2a5047345b8af88d114bbcaad69a8).

## Pinned Wine file contract

At this revision, [`server/registry.c`](https://github.com/wine-mirror/wine/blob/490f6d5dcbb2a5047345b8af88d114bbcaad69a8/server/registry.c)
loads three text files from the Wine configuration directory:

| File | Registry root loaded |
| --- | --- |
| `system.reg` | `\Registry\Machine` |
| `userdef.reg` | `\Registry\User\.Default` |
| `user.reg` | the current user's `\Registry\User\S-...` branch |

Wine writes these branches using its `WINE REGISTRY Version 2` text format
(see [`save_all_subkeys`](https://github.com/wine-mirror/wine/blob/490f6d5dcbb2a5047345b8af88d114bbcaad69a8/server/registry.c#L1970)).
It is similar to REGEDIT import/export text but has Wine-specific Unicode
escapes, optional key modification times and additional string encodings. The
server saves the three roots periodically (30-second timer) and during
registry shutdown. Class registrations are registry branches in the machine
and/or current-user trees, not a fourth `classes.reg` file in this pinned
implementation.

`PwPrefixLayout` predates this audit and currently also exposes a
`classes.reg` path. Do not treat that path as a Wine hive or write Wine data
there. Reconciling the old layout is a separate integration change; this
adapter uses the three pinned filenames above and ignores the legacy fourth
slot.

## Current project state

The profile prefix currently creates directories and names hive paths, but no
PS5 storage adapter reads or persists Wine's real registry text files. The
Wine registry callbacks exercised by the host gate are run-local. The
`registry.pwrg` file is a separate little-endian serialization of the
project's bounded `PwRegistry` emulator state. It is not Wine's format, not a
Wine hive, and must not be loaded as one or renamed to a `.reg` filename.

## Adapter boundary

`PwPrefixRegistryStore` is deliberately a byte-preserving storage seam. A
future Wine registry integration owns parsing and serialization; the storage
provider owns directory-relative reads and atomic replacement of one named
file. The current host tests exercise prefix isolation, exact filename
selection, missing-file behavior and output preservation on errors. No Wine
parser, converter, registry-server integration, PS5 file backend or
multi-file transaction is implemented here. Atomicity is per file only; a
consistent three-file checkpoint or crash-recovery policy remains future
work. The caller must arrange Wine's normal save/close path before treating a
prefix as cleanly persisted.
