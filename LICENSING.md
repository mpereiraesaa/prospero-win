# Licence: LGPL-2.1-or-later

**Decided.** prospero-win is licensed under the GNU Lesser General Public
License, version 2.1 or (at your option) any later version. The full text is
in [`LICENSE`](LICENSE); every source file carries an
`SPDX-License-Identifier: LGPL-2.1-or-later` line.

## Why this one

This project is a compatibility layer. Its whole purpose is to be combined,
at run time, with proprietary third-party code — the game executable and the
vendor DLLs it manually maps, whose imports it binds to its own
implementations. Three properties follow from that, and LGPL-2.1-or-later is
the licence that satisfies all three.

**It permits the combination the project exists to perform.** A strong
copyleft on the layer itself would invite exactly the argument that followed
Wine for years, and is why Wine relicensed from GPL to LGPL in 2002. The
same reasoning applies here, for the same reason.

**It keeps improvements to the layer itself shared.** The value of a
compatibility layer accumulates in a long tail of small fixes: this import
resolves differently, that structure has a different layout, this game needs
that quirk. Wine's ecosystem works because those fixes flow back. A
permissive licence would allow a closed fork to absorb that tail without
returning anything, and for this kind of project that is the failure mode
worth guarding against. Note the split in prior art: the pure translators
(box64, FEX-Emu) chose permissive terms, while the API reimplementation
(Wine) chose LGPL. prospero-win is mostly the second kind of work.

**It maximises what can be brought in.** Version 2.1 rather than 3 is a
deliberate choice about inbound compatibility:

- Wine is LGPL-2.1-or-later, so its code and headers can be incorporated
  directly if that ever helps.
- QEMU, which the [technical details](docs/TECHNICAL_DETAILS.md) identify as
  prior art for
  32-bit instruction behaviour, is GPL-2.0-**only** in large parts. LGPL-2.1
  code can be combined with GPL-2.0-only code (the result is GPL-2.0);
  LGPL-3.0 cannot be combined with it at all. Choosing 2.1 preserves that
  option, and it is a real one on the translation route.
- DynamoRIO, the prior art for x86-32 to x86-64 mangling, is BSD-3-Clause,
  and Zydis is MIT. Both are compatible either way, so neither constrains
  the choice.

What version 3 would have added — an explicit patent grant and the
anti-tivoization terms — is of little value to a project that ships no
hardware, and it would have cost the QEMU option. The `or later` clause
leaves downstream users free to adopt version 3 if they prefer it.

## What this means in practice

- Linking prospero-win into a program, including a proprietary one, does not
  make that program LGPL. The game it runs is unaffected by this licence.
- Modifying prospero-win itself and distributing the result requires
  publishing those modifications under the same terms.
- Nothing here grants any right to distribute game data, vendor DLLs, or
  firmware-derived material. Those are private build inputs and never enter
  this repository; see [`NOTICE.md`](NOTICE.md).

## Attribution

Copyright is retained by the contributors. The `LICENSE` text is the FSF's
verbatim LGPL-2.1, taken from
`https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt` and cross-checked
against an independent copy; the two agree on every operative term.
