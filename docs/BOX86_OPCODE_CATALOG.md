# Box86 opcode coverage catalog

This is a reproducible source-dispatch census plus representative, offline decode probes. It is a planning aid, not a claim of universal x86 compatibility.

- Box86 commit: `b05cb3ab54a610e09acc594e3aca0419d9ab9e4a`
- Box86 license: MIT (root LICENSE; file-level exceptions must be audited)
- Box86 dispatch rows with interpreter labels: 1577
- Rows with ARM DynaRec labels: 2409
- Prospero accepted representative probe rows: 319
- Prospero decoder probe only calls `pw_x86_translate`; it never executes emitted code.

## Decode-probe snapshot

The probe tries bounded byte candidates over the supported prefix/map cross-product. A mask bit means the first instruction consumed the candidate byte after the opcode; that byte may be an immediate rather than ModRM. This is a navigation aid only, not an instruction-form support claim.

| Prefix | Map | Rows with candidate-byte consumption |
|---|---|---:|
| `64` | `primary` | 2 |
| `66` | `0f` | 78 |
| `66` | `primary` | 34 |
| `f0` | `0f` | 1 |
| `f0` | `primary` | 3 |
| `f2` | `0f` | 3 |
| `f3` | `0f` | 6 |
| `none` | `0f` | 70 |
| `none` | `primary` | 122 |

## Primary opcode family bands

These are navigation bands, not claims that every instruction in a band is implemented.

| Primary-byte range | Broad family |
|---|---|
| `00–3F` | ALU, compare and accumulator forms |
| `40–5F` | 32-bit INC/DEC and register stack operations |
| `60–6F` | Legacy stack, string and miscellaneous operations |
| `70–7F` | Short conditional branches |
| `80–8F` | Group ALU operations and ModRM data movement |
| `90–9F` | NOP/XCHG and flag operations |
| `A0–AF` | Moffs, string, and TEST operations |
| `B0–BF` | Immediate-to-register moves |
| `C0–CF` | Shifts, returns, interrupts and groups |
| `D0–D7` | Shifts, rotates and XLAT |
| `D8–DF` | x87 escape maps |
| `E0–EF` | Loop, port and control-transfer operations |
| `F0–FF` | Lock/repeat prefixes, flag and grouped operations |

## Map/source index

| Map/prefix selector | Box86 interpreter | Box86 ARM DynaRec |
|---|---|---|
| `primary` | `src/emu/x86run.c` | `src/dynarec/dynarec_arm_00.c` |
| `0f` | `src/emu/x86run0f.c` | `src/dynarec/dynarec_arm_0f.c` |
| `66` | `src/emu/x86run66.c` | `src/dynarec/dynarec_arm_66.c` |
| `67` | `src/emu/x86run67.c` | `src/dynarec/dynarec_arm_67.c` |
| `segment-64/65` | `src/emu/x86run64.c` | `src/dynarec/dynarec_arm_64.c`, `src/dynarec/dynarec_arm_65.c` |
| `f0` | `src/emu/x86runf0.c` | `src/dynarec/dynarec_arm_f0.c` |
| `f20f` | `src/emu/x86runf20f.c` | `src/dynarec/dynarec_arm_f20f.c` |
| `f30f` | `src/emu/x86runf30f.c` | `src/dynarec/dynarec_arm_f30f.c` |
| `660f` | `src/emu/x86run660f.c` | `src/dynarec/dynarec_arm_660f.c` |
| `66f0` | `src/emu/x86runf066.c` | `src/dynarec/dynarec_arm_66f0.c` |
| `640f` | `src/emu/x86run640f.c` | `src/dynarec/dynarec_arm_64.c`, `src/dynarec/dynarec_arm_0f.c` |
| `6466` | `src/emu/x86run6466.c` | `src/dynarec/dynarec_arm_64.c`, `src/dynarec/dynarec_arm_66.c` |
| `6467` | `src/emu/x86run6467.c` | `src/dynarec/dynarec_arm_64.c`, `src/dynarec/dynarec_arm_67.c` |
| `6766` | `src/emu/x86run6766.c` | `src/dynarec/dynarec_arm_67.c`, `src/dynarec/dynarec_arm_66.c` |
| `66d9` | `src/emu/x86run66d9.c` | `src/dynarec/dynarec_arm_66.c`, `src/dynarec/dynarec_arm_d9.c` |
| `66dd` | `src/emu/x86run66dd.c` | `src/dynarec/dynarec_arm_66.c`, `src/dynarec/dynarec_arm_dd.c` |
| `66f20f` | `src/emu/x86run66f20f.c` | `src/dynarec/dynarec_arm_66.c`, `src/dynarec/dynarec_arm_f20f.c` |
| `x87-d8` | `src/emu/x86rund8.c` | `src/dynarec/dynarec_arm_d8.c` |
| `x87-d9` | `src/emu/x86rund9.c` | `src/dynarec/dynarec_arm_d9.c` |
| `x87-da` | `src/emu/x86runda.c` | `src/dynarec/dynarec_arm_da.c` |
| `x87-db` | `src/emu/x86rundb.c` | `src/dynarec/dynarec_arm_db.c` |
| `x87-dc` | `src/emu/x86rundc.c` | `src/dynarec/dynarec_arm_dc.c` |
| `x87-dd` | `src/emu/x86rundd.c` | `src/dynarec/dynarec_arm_dd.c` |
| `x87-de` | `src/emu/x86runde.c` | `src/dynarec/dynarec_arm_de.c` |
| `x87-df` | `src/emu/x86rundf.c` | `src/dynarec/dynarec_arm_df.c` |

## How to read it

- A Box86 `case` label is recorded as source evidence only; nested switch labels may be ModRM group selectors, not opcode bytes. The two engines are listed independently.
- A successful Prospero probe means that one concrete byte stream entered the bounded decoder. It does not establish every register/memory/width/prefix variant.
- The candidate masks vary the bits normally used by ModRM.reg and choose a register-shaped or memory-shaped following byte; they are deliberately named candidates because some non-ModRM instructions consume that byte as an immediate.
- Execution coverage remains owned by the existing host tests; this catalog does not infer a passing semantic test for every source label.
- Box86's ARM code is not a backend for this project. Reuse the inventory and semantic/test reference; implement semantics in Prospero's x86-64 emitter.

## Licensing note

No Box86 source is vendored by this catalog generator. The root repository is MIT, but `src/emu/x86primop.c` carries a separate Realmode X86 Emulator Library notice. Audit the exact file and preserve its notices before copying any implementation. This repository remains LGPL-2.1-or-later.

## Regenerate

```sh
git clone https://github.com/ptitSeb/box86.git /tmp/box86
git -C /tmp/box86 checkout b05cb3ab54a610e09acc594e3aca0419d9ab9e4a
make box86-catalog BOX86_SOURCE=/tmp/box86
```

The generated JSON is deterministic for the pinned source commit and current Prospero decoder. Do not update the pin without regenerating, reviewing new file-level notices, and validating the parser against the changed dispatch layout.
