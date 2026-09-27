# DBT benchmark: 7-Zip

7-Zip's built-in benchmark (`7za b`) is the yardstick that box86, box64 and
FEX publish. It is mostly integer code, with little SSE and few library
calls, so it measures the translator itself. Here it compares prospero-win's
i386 DBT (`wine/wowprospero`) with the same 32-bit binary running natively.

## Method

`tools/bench_7zip.sh` downloads the official 7-Zip 25.01 "extra" package,
pinned by SHA-256. The package is never committed: 7-Zip is LGPL with the
unRAR restriction, and its `License.txt` is extracted beside it. The script
runs `7za b -mmt1 -md22` on one pinned CPU and parses the results with
`tools/bench_7zip.py`. The benchmark uses one thread because the DBT
translates per thread.

| `--config` | What runs |
|---|---|
| `wow64cpu` | i386 `7za.exe` under Wine WoW64 with Wine's `wow64cpu.dll`: the CPU runs it natively. This is the baseline. |
| `wowprospero` | The same binary with `wowprospero.dll`: our DBT. |
| `pe64` | The x64 `7za.exe` from the same package under Wine. |
| `linux` | The host's own `7z` (p7zip 23.01, GCC). A different build, for reference only. |

The two i386 configurations use the same Wine build, prefix and binary. Only
the WoW64 CPU backend differs: the script sets
`HKLM\Software\Microsoft\Wow64\x86`. `--modes` sets `PW_WOW_MODES`, whose
digits are chaining, register residency, lazy flags, the indirect target
table and the flat guard; digits left out stay on.

```
tools/bench_7zip.sh --config wow64cpu    --wine <wine> --prefix <prefix> --runs 3
tools/bench_7zip.sh --config wowprospero --wine <wine> --prefix <prefix> --runs 3
tools/bench_7zip.sh --config wowprospero --wine <wine> --prefix <prefix> --modes 1011
```

The `wowprospero` prefix needs `wowprospero.dll` installed
(`docs/WINE_INTEGRATION.md`).

## Results (2026-09-27)

Host: i7-12700H, one P-core, the pinned Wine (11.17-54) built for the host
in WoW64 mode with `wowprospero`. The machine was
shared with other builds (load 2–4), so runs vary by about ±10%.

Three interleaved rounds, medians. The ratings are in MIPS.

| Configuration | Compress | Decompress | Total | vs native |
|---|---|---|---|---|
| native (`wow64cpu`) | 5458 | 4355 | 4906 | 100% |
| DBT, main plus PUSHFD/POPFD only | 596 | 437 | 498 | 10.2% |
| DBT, all fixes below (`1111`) | 625 | 534 | 579 | 11.8% |
| DBT, no residency (`1011`) | 649 | 564 | 593 | 12.1% |

DBT/native for the full-fix build: **11.5% compressing, 12.3% decompressing**.

Modes, one run each (matrix run, same session):

| `PW_WOW_MODES` | Compress | Decompress | Total |
|---|---|---|---|
| `1111` (default) | 577 | 516 | 567 |
| `1110` (no indirect table) | 535 | 598 | 567 |
| `0111` (no chaining) | 319 | 292 | 305 |
| `1011` (no residency) | 674 | 621 | 648 |
| `1101` (no lazy flags) | 622 | 576 | 599 |
| `0000` (none) | 361 | 329 | 345 |

References from the same session: x64 `7za.exe` under Wine rated 5066 /
5736 / 5401, and Linux p7zip 5664 / 4250 / 4957.

### What made it run at all

Before these fixes, main could not run the benchmark. 7-Zip's CPU detection
(`pushfd; btc [esp], 21; popfd`) hit an unsupported instruction and the
process died with an illegal instruction:

- **PUSHFD/POPFD** (`9C`/`9D`) are emulated beside the other implicit-stack
  forms in the single-instruction fallback (`src/pw_x86_hostexec.c`). The
  arithmetic, DF, AC and ID bits round-trip, so the ID toggle reads back as
  supported.
- **ROL/ROR r/m32** (`C1`/`D1`/`D3` `/0` and `/1`) are translated. Before,
  every one of them fell back, re-translating the block each time: 23.7M
  fallbacks per run, mostly `ror eax, 16` in the LZMA coder.
- **movdqa/movaps with a memory operand** are translated, with an inline
  alignment check. A misaligned address is the guest's access violation at
  0xffffffff, which is how Windows reports the CPU's #GP. Before, Wine's
  i386 `memcpy` fell back on every 16-byte store.

After these, 6.5M fallbacks per run remain, and they take about 3% of the
DBT's time.

### Speed-ups since

Each row is an interleaved A/B against the build before it, on the same
machine load: three rounds, medians, total MIPS. Native (`wow64cpu`) rated
4989 in the same session.

| Change | Before | After | Gain | vs native after |
|---|---|---|---|---|
| Flat guard: one compare per access (`PW_WOW_MODES` 5th digit) | 557 | 622 | +12% | 12.5% |
| Guest EIP stored only before instructions that can stop the block | 617 | 657 | +6% | 13.2% |
| No statistics counters in translated code (`PW_WOW_STATS` keeps them) | 657 | 726 | +10% | 14.6% |
| Conditional branches on the producer's flags | 650 | 788 | +21% | 19.5% |
| Guest addresses with one `lea` | 788 | 804 | +2% | 19.9% |
| Shifts and rotates by a constant as the host instruction | 797 | 872 | +9% | 18.5% |
| Flat-guard misses out of line, after the block | 872 | 960 | +10% | 20.4% |
| RCL/RCR and 16-bit rotates by a constant (and the 16-bit count fix) | 958 | 977 | +2% | 20.0% |
| Global residency: seven guest GPRs in the same host registers in every block | 956 | 1006 | +5% | 21.3% |
| Same-ISA re-encoder: pinned GPRs, native flags, copied instructions | 987 | 1204 | +22% | 28.6% |
| Re-encoder: lock, atomic xchg/cmpxchg/xadd/cmpxchg8b, fs: | 1237 | 1212 | noise | 23.0% |

- **Flat guard.** wowprospero's guest is one identity-mapped range, and the
  stack range and the one region are both that range, so every access
  inside it is valid. The guard is `lea edx,[rax-low]; cmp edx,span-width;
  jbe`, instead of about nine instructions and four branches. An access
  outside the range still goes through the region table, which records the
  fault as before.
- **EIP stores.** Every guest instruction stored the guest EIP twice: its own
  address before it, for a fault, and the next one after it. Only an
  instruction that can stop the block in the middle (a refused access, a
  failed helper) needs the first, and only the last instruction needs the
  second. The translator emits each block twice: the first emission finds
  those instructions, and the second, which is kept, stores EIP only there.
  In the benchmark's hot loops that removes most of the stores.
- **Statistics counters.** Every block exit added to `step_retired` and
  `step_transitions`, and residency added to the `reg_*` counts. These are
  read-modify-writes of memory on every block transition, for statistics
  wowprospero never reads. `pw_x86_engine_set_counters` leaves them out;
  `PW_WOW_STATS=1` keeps them. The chain budget and the link slots stay.
- **Branches on the producer's flags.** A conditional branch merged the
  pending flags into EFLAGS in memory (eight instructions and three loads)
  and then tested EFLAGS. When the instruction just before the branch
  produced every flag the condition reads, those flags are still in the
  host: in the host flags themselves when the producer defines all six
  (`jcc` as is), or in `rcx`, which holds its captured flags, otherwise
  (`test ecx` and at most five instructions). The pending flags stay
  pending, exactly as before. From here the native baseline is taken from
  the same rounds (4032 MIPS, on another core); the ratio uses it.
- **Addresses.** A guest address was built with `mov eax, disp`, an `add`
  for the base, and a load, `shl` and `add` for a scaled index. It is now
  the base (and index) loaded into `eax` (and `edx`) and one `lea`, whose
  32-bit destination wraps the sum modulo 2^32 exactly as the guest does.
- **Constant shifts.** A shift or rotate by an immediate went through the
  variable-count path: commit every pending flag, then about twenty
  instructions that select at run time which flags the count defines. With
  a nonzero constant count the defined flags are known when translating, so
  the host instruction runs as is and its defined flags are deferred like
  any producer's (native baseline 4712 MIPS in these rounds).
- **Cold paths.** Each guarded access carried its miss path inline: a call
  to the region table with five register saves, about sixty bytes that the
  hit jumped over. The miss is now a `ja` to a stub after the block's last
  exit, which calls the table and jumps back, so the hit falls through and
  the hot code is a quarter the size. A refused push or pop shares the same
  mechanism.
- **Global residency.** The per-block allocator gave each block its own three
  resident registers, so nearly every linked exit met a different contract
  and went through reconciliation (store, then reload). Now every block
  without a helper call holds the same guest GPRs in the same host
  registers, r8-r10 and the callee-saved r12-r15, so linked blocks hand
  them over in place; a block with a helper keeps an empty contract. Seven
  fit (r11 is the emitter's scratch); leaving out EDX measured best (1004
  and 1009 total MIPS against 971 and 972 for leaving out EDI), and
  `PW_WOW_RESIDENT=<hex mask>` picks others. Generated code is entered
  through `pw_x86_run_block`, which saves the callee-saved registers. Two
  interleaved rounds, native not in these rounds (4731 above); the gain is
  in decompression (+15%), while compression is 5% slower, because this
  emitter still copies a resident value through `eax` for most operations
  and stores every resident register before each instruction that can
  fault. `PW_WOW_MODES` residency `2` keeps the per-block allocator.
- **Same-ISA re-encoder** (`src/pw_x86_reencode.c`). Host and guest are
  both x86, so a block is re-emitted rather than emulated. The eight guest
  GPRs are pinned for a whole chain (eax ecx edx ebx ebp esi in their own
  registers, esp in r12, edi in r13), and the guest's arithmetic flags stay
  in RFLAGS, saved only on the way back to C. ALU, mov, shift, imul,
  movzx/movsx, setcc, cmov, bt and bswap forms are copied with the ModRM
  and REX adjusted for r12 and r13. A memory operand becomes `[r11]` after
  the flat guard; the guest range is identity-mapped. The glue between
  instructions is flag-free (mov, lea, xchg, jrcxz), and the guard, the
  only thing that compares, saves the flags around itself with
  `lahf`/`seto` whenever a later instruction reads them. Push, pop, call,
  ret and leave are translated onto r12. The first instruction it does not
  take ends the block, and the old emitter translates that one; the two
  meet through their canonical entries. `PW_WOW_MODES` 6th digit `0` turns
  it off. Three interleaved rounds with native (median 4210 MIPS under
  this machine's load); compression +27%, decompression +13%.
- **Atomics and fs.** Wine's own code ended many re-encoded blocks at a lock
  prefix or an `fs:` access to the TEB. The lock forms, xchg with memory,
  cmpxchg, xadd and cmpxchg8b now run as the host instruction on `[r11]`,
  and an `fs:` operand adds the guest's fs base before the guard. 7-Zip's
  hot loops use neither, so its rating does not move (three rounds,
  native median 5281); the gain is in how much of Wine stays re-encoded.

### After these changes

Main at the start of this work against main with all of the above, three
interleaved rounds against native on the same core, medians:

| Configuration | Compress | Decompress | Total | vs native |
|---|---|---|---|---|
| native (`wow64cpu`) | 5358 | 4104 | 4731 | 100% |
| DBT before (#155) | 557 | 522 | 540 | 11.4% |
| DBT after (`11111`) | 991 | 915 | 953 | **20.1%** |
| after, no residency (`10111`) | 1029 | 851 | 940 | 19.9% |
| after, no lazy flags (`11011`) | 970 | 858 | 915 | 19.3% |

The DBT is 1.76× faster: 18.5% of native compressing and 22.3%
decompressing. Residency is now neutral (it was a 2–14% loss), and lazy
flags now pay 4%, because producers feed branches directly.

## Comparison with published numbers

Published ratios, total rating as a share of native:

| Translator | Host | 7-Zip vs native | Source |
|---|---|---|---|
| box86 | Raspberry Pi 400 (ARM) | 3117 / 6157 = 51% | [box86.org, 2022-03][b] |
| box64 | Raspberry Pi 400 (ARM) | 3084 / 5787 = 53% | [box86.org, 2022-03][b] |
| box64 | Apple M1, Linux | 57% | [box86.org, 2022-03][b] |
| Rosetta 2 | Apple M1 | 71% | [box86.org, 2022-03][b] |
| FEX (x86 / x86-64) | Raspberry Pi 400 | 19% / 26% (FEX of 2022) | [box86.org, 2022-03][b] |
| QEMU user (x86 / x86-64) | Raspberry Pi 400 | 11% / 16% | [box86.org, 2022-03][b] |
| **prospero-win DBT** | i7-12700H (x86-64) | **20%** (12% before) | this page |

These are indicative only. The others translate x86 to ARM on other
hardware, and FEX has improved a lot since 2022. Our host is x86-64, so the
guest instructions could nearly be copied. Today we sit at QEMU's level:
roughly 4× short of box64.

## Where the time goes

Measured with a temporary rdtsc probe on the dispatcher (perf is not
available on this host), full-fix build:

- **97% of the DBT's time is in translated code**, 3% in fallbacks and
  translation. The dispatcher is not the bottleneck: blocks run about 135
  guest instructions per dispatcher return, and 3.9G linked transitions carry
  32G guest instructions (about 8 instructions per block).
- So the cost is the code we emit. The likely causes, in order, are the
  follow-ups below.

## Follow-ups

1. **The memory guard on every access** (done: the flat guard above). `memory_address_width` runs about 9
   instructions and 4 branches before each guest load or store, even when
   the whole address space is one region, as in wowprospero. Under WoW64 the
   guest is identity-mapped, so a single bounds check, or none with a fault
   handler (as box64 and FEX do), would remove most of it.
2. **Register residency is slower here.** It is 2–14% slower than without
   it, in both the matrix and the A/B runs. Only 3 host registers hold guest
   registers, and the spills and reloads around helpers and exits probably
   cost more than they save. Profile it and widen or rework it.
3. **Flag capture.** Every flag-producing instruction whose flags are live
   captures them with `pushfq; pop`. In the one mode run, lazy flags off
   (`1101`) beat the default, a hint that the deferred bookkeeping does not
   pay for itself in this code; it needs repeated runs.
4. **Guest registers live in memory.** Most instructions load and store
   `[rdi+gpr*4]`. Mapping all 8 guest GPRs to host registers, as box64 does,
   is the structural fix.
5. **Remaining fallbacks.** RCL/RCR and the 16-bit rotates are not
   translated yet.

### Still open

- **Guest registers in memory.** Most instructions still load and store
  `[rdi+gpr*4]`. Mapping all eight guest GPRs to host registers for a whole
  block or chain, with spills at exits, helpers and faults, is the
  structural step toward box64's ~50%. Residency (three host registers) is
  the partial version and is now neutral.
- **The memory guard.** The flat guard is one `lea`, `cmp` and `ja` per
  access. Removing it needs a reserved 4 GiB guest range and a fault
  handler that turns a host fault into the guest's access violation at the
  right EIP. That handler has to live beside Wine's own signal handling on
  the PS5, so it needs the console to validate.
- **Chain quantum.** A chain yields to the dispatcher every 64 blocks. 1024
  measured +9% more (974 to 1055 total MIPS); the cost is how long a
  thread runs before it notices a flush or a signal, which needs measuring
  with Pinball first.
- **Flag capture.** Producers still capture flags with `pushfq; pop` when
  the branch is not adjacent.

## On the console

`examples/wine/profiles/sevenzip-bench.profile` runs the same benchmark
under the title:

1. Copy the i386 `7za.exe` from the pinned package to the prefix's
   `drive_c/Tools`.
2. Add the profile to the library's `profiles/` directory, then start the
   title and sync the library (Triangle).
3. Choose "7-Zip benchmark". The profile's `arguments` line
   (`b -mmt1 -md22`) is passed to the program. The title forwards its
   standard output (fd 1) to ps5log as `STDOUT` lines. This path has not yet
   been run on the console.
4. When the program exits, the title returns to the launcher. Feed the
   `STDOUT` lines to `tools/bench_7zip.py`, which reads the `Avr:`/`Tot:`
   rows.

The native baseline for the PS5 has to come from another x86-64 machine
with the same CPU family: the console cannot run the binary without the DBT.

[b]: https://box86.org/2022/03/box86-box64-vs-qemu-vs-fex-vs-rosetta2/
