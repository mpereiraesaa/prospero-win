# DBT benchmark: 7-Zip

7-Zip's built-in benchmark (`7za b`) is the yardstick that box86, box64 and
FEX publish. It is mostly integer code, with little SSE and few library
calls, so it measures the translator itself. Here it compares prospero-win's
i386 DBT (`wine/wowprospero`) with the same 32-bit binary running natively.
nbench and a Super PI-style pi program cover floating point.

## Current state (2026-09-28)

Host (i7-12700H, one P-core), the DBT against the same i386 binary run
natively under Wine:

| Benchmark | vs native | Details |
|---|---|---|
| 7-Zip, total rating | **91%** | [Direct links, copied operands and superblocks](#direct-links-copied-operands-and-superblocks) |
| nbench x87, integer / FP index | **94% / 97%** | [Floating point](#floating-point-nbench-and-pi) |
| nbench SSE2, integer / FP index | **92% / 93%** | [Floating point](#floating-point-nbench-and-pi) |
| pi, 4.2M digits, x87 and SSE2 | **98%** | [Floating point](#floating-point-nbench-and-pi) |

On the console (FW 12.02), 7-Zip rates 3361–3369 total MIPS against 1383
before this work, and the DBT runs at an estimated 85–93% of the console's
native speed; the console figures are [below](#on-the-console). The
sections after the first results record each change in order.

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

## First results (2026-09-27)

The starting point, before the re-encoder and the changes below.

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
| Re-encoder: returns and indirect calls stay pinned | 1237 | 1316 | +6% | 24.9% |
| Chain quantum 64 to 1024 linked blocks per dispatcher return | 1299 | 1416 | +9% | 29.2% |
| Fault markers instead of the flat guard in re-encoded blocks | 1607 | 1881 | +17% | 32.3% |

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
- **Fault markers.** A re-encoded access no longer runs the flat guard
  (`lea`, `cmp`, `ja`, and `lahf`/`seto`/`sahf` around them when the flags
  are live). An 8-byte `nopl` before the access points at its
  refused-access path instead; the access itself faults when it falls
  outside the guest range, and wowprospero moves the RIP there, so the
  guest gets the access violation the guard reported, now with its real
  flags too. Only faults outside the guest range are taken: one inside it
  (a guard page, a write watch) stays Wine's. On a Unix host wowprospero
  installs a SIGSEGV handler that chains to Wine's; on the PS5, where the
  previous action `sigaction` reports lies outside ntdll, Wine's own
  handler calls wowprospero first (patch 0610). On
  the console the null test's loads and stores are resumed exactly as on
  the host. (The markers themselves were later replaced by a fault table:
  see [below](#direct-links-copied-operands-and-superblocks).) Three rounds
  each, medians 1607/1607/1627 against 1881/1961/1877, native 5819 in the
  same session; compression gains 24% to 30%.
  `PW_WOW_FAULT_MARKERS=0` keeps the guard.
- **Pinned returns.** A ret or indirect call left the pinned state, looked
  the target up and re-entered it through its canonical entry: about a
  hundred instructions. The dispatcher now also records each re-encoded
  block's chain entry in a table of 65536 slots (the low 16 bits of the
  PC), and a dynamic exit looks there first without touching the flags
  (`movzx`, `lea`, `not`, `xchg`, `jrcxz`), so a hit jumps to the target
  with the state still in registers. Same rounds as above; compression
  gains 16% (1457-1556 against 1105-1290), decompression is unchanged.
- **Chain quantum.** A chain returned to the dispatcher every 64 linked
  blocks. wowprospero now allows 1024 (`PW_WOW_QUANTUM` overrides it).
  Pinball renders and plays with no `err:` lines. Three rounds, native
  median 4850; decompression +20%. (Re-encoded chains later dropped the
  budget altogether: see unbounded chains below.)

### After the re-encoder

Main with #164-#170 against native, three interleaved rounds each (the
rows above), total MIPS: from 956 (20% of native) to 1416 (29.2%), about
1.5x. Compression is at 26-30% of native and decompression at 34%.

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

### Direct links, copied operands and superblocks

A sampling profiler (`PW_WOW_PROFILE=<file>`, host only: SIGPROF every
millisecond of CPU, with the hottest blocks' host and guest bytes) showed
where the time went: about 98% in translated code, and there in a few
fixed costs around the guest's own instructions. Each row is an
interleaved A/B against the build before it (two or three rounds,
medians, total MIPS); the machine's load moved between sessions, so the
share of native is from the session of the row.

| Change | Before | After | Gain | vs native after |
|---|---|---|---|---|
| Guest EIP stored by the refused-access path, not before each access | 1886 | 1945 | +3% | 36% |
| Direct links: exits are rel32 jumps the engine points at their target | 1987 | 3509 | +77% | 65% |
| Memory operands copied with the guest's address size; a fault table instead of markers | 3692 | 4347 | +18% | 75% |
| `rcx` kept in `r9` around `jrcxz` instead of `xchg` | 4306 | 4474 | +4% | 77% |
| Unbounded chains: no budget in re-encoded code | 4317 | 4630 | +7% | 86% |
| Stack accesses without the guard's `lea` | 4931 | 4970 | +1% | 85% |
| Guest calls and returns on a host call stack | 4706 | 4706 | 0% | 82% |
| Superblocks with self-linking side exits | 4706 | 5064 | +8% | **91%** |

In the last session, three interleaved rounds: native 6715 / 4706 / 5739
(compress / decompress / total), the DBT 5979 / 4444 / 5211: **89%
compressing, 94% decompressing, 91% in total**, from 32% at the start of
this work. The same `7za` compressing 25 MB of Wine sources and
binaries (`a -mx=5 -mmt1`) writes a byte-identical archive under the DBT
and natively, in 7.08 s against 6.64 s, and the DBT extracts it back
unchanged.

- **EIP on refused accesses.** Every guest access stored its EIP first.
  The refused-access path of each access now stores it instead, as its
  first instruction, so a fault inside the guest range that goes to Wine
  still reports it exactly: the fault handler reads it from there
  (`pw_x86_cold_path_eip`).
- **Direct links.** A linked exit loaded the chain budget, decremented and
  stored it, exchanged `rcx` twice around `jrcxz` and jumped through the
  link slot in memory: about 11 instructions for blocks of about 8. An
  exit is now one `jmp rel32` (a taken branch uses the `jcc`'s own), which
  the engine rewrites whenever it sets the slot (`sync_direct`).
- **Copied operands.** A memory operand was a `lea` into `r11`, the 8-byte
  marker and the access through `r11`. With a `0x67` prefix the 32-bit
  effective address, zero-extended, is the guest address, so the
  instruction is copied with its own addressing (esp and edi mapped to
  `r12` and `r13`), which also removes a cycle from every pointer chase.
  Each block ends with a table of its accesses and their refused-access
  paths, and the engine finds the block of a host fault through a map of
  the first block over every 256 bytes of the arena.
- **Unbounded chains.** wowprospero notices another thread's code flush
  when `run()` starts, not at a dispatcher return, and Wine suspends a
  thread with a signal: nothing needs the dispatcher between linked
  blocks. Re-encoded blocks therefore spend no budget unless
  `PW_WOW_TRACE`, `PW_WOW_HOSTEXEC_ALL` or `PW_WOW_QUANTUM` is set.
- **Stack accesses.** A push stores below esp first, then moves esp with
  one `lea`: two instructions instead of three, and one step on esp's
  dependency chain.
- **Call stack.** A guest call makes a host call to its callee on a
  per-thread call stack, and a guest ret a host ret, so the CPU's return
  predictor works; the landing after each call checks the guest's return
  address and falls back to the lookup. It changes nothing in 7-Zip, whose
  hot calls were already predicted, but on an i386 call benchmark a
  function called from eight sites goes from 68% of native to 110%, and
  calls through a table from 65% to 74%. `PW_WOW_CALL_STACK=0` turns it
  off.
- **Superblocks.** A block went to another block at every conditional
  branch. It now goes on past it, up to 32 instructions, and the branch is
  a side exit that links itself: the first time its target is in the chain
  table, it rewrites its own `jcc` to jump there. The fallthrough path runs
  straight on. `PW_WOW_SUPERBLOCKS=0` ends blocks at every branch.

Tried and dropped: aligning blocks to 64 bytes and chain entries to 32
(+1%, within the noise), and 64-instruction blocks (no change). Advertising
every host CPU feature to the guest, as `wow64cpu` does, makes 7-Zip report
the same features as native but does not change its rating: the gap is not
in the code paths the guest picks.

### Floating point: nbench and pi

7-Zip is integer code. For the x87 and SSE that games of 2000-2020 run,
two more benchmarks, each built for i386 with mingw in an x87 build (the
default, as engines of the early 2000s were) and an SSE2 build
(`-msse2 -mfpmath=sse`):

- **nbench** (BYTEmark 2.2.3): ten tests, an integer and a floating-point
  index. Built from source; not committed.
- **pi**: Takuya Ooura's `pi_fftca` (Gauss-Legendre with FFT
  multiplication in double precision, Super PI's method), at 4.2M digits.
  Super PI and PiFast themselves are no longer downloadable, and Super PI
  needs a window. The digits it writes are compared with native's.

Before this, the emitter ran x87 through a software FPU, one C call per
instruction, and most SSE went to the host one instruction at a time:
pi took more than 900 s against 7.5 s natively, and nbench's FP index was
at 17-19% of native. Now the guest's x87, MMX and SSE state is the host
FPU's while re-encoded code runs (`native_fp`: `pw_x86_run_block_fp` loads
and saves it around each entry from C), and the re-encoder copies x87,
MMX and SSE up to SSE4.1, the string instructions (the host's own, with
rdi and r13 swapped around them), fwait, sahf and lahf. Re-encoded and
emitted blocks then meet only through C, which moves the FP state between
the two. `PW_WOW_NATIVE_FP=0` leaves FP to the emitter.

Host, the same session, native first:

| Benchmark | Native | DBT | vs native |
|---|---|---|---|
| pi x87, 4.2M digits | 7.51 s | 7.63 s | 98% |
| pi SSE2, 4.2M digits | 5.51 s | 5.60 s | 98% |
| nbench x87, integer index | 249.5 | 235.2 | 94% |
| nbench x87, FP index | 124.3 | 120.5 | 97% |
| nbench SSE2, integer index | 254.3 | 232.9 | 92% |
| nbench SSE2, FP index | 142.7 | 132.8 | 93% |

The pi digits are identical to native's. The lowest nbench test is STRING
SORT at 83% (its memmove crosses to the emitter for std and cld when it
copies backwards). 7-Zip does not change (4685 against 4650 MIPS, two
rounds).

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
| **prospero-win DBT** | i7-12700H (x86-64) | **91%** (29%, 20% and 12% before) | this page |

These are indicative only. The others translate x86 to ARM on other
hardware, and FEX has improved a lot since 2022. Our host is x86-64, so the
guest instructions can nearly be copied, which is most of why the ratio is
higher than the ARM translators'.

## Where the time goes

`PW_WOW_PROFILE` on the current build (7-Zip, one run): 98% of the samples
are in translated code, 93% of them in the bodies of re-encoded blocks
(the guest's own instructions, copied) and 4% in their exits; blocks from
the older emitter take 0.3%. The hottest blocks are the match finder's
hash-chain loads, whose samples sit on the load after a cache miss, as
they would natively. What remains is mostly what copying cannot remove:
the prefixes on copied instructions, the jump at the end of each
32-instruction block, and the landing check after each call.

Before this work, a temporary rdtsc probe on the dispatcher had shown the
same 97% in translated code; the follow-ups below were written then.

## Follow-ups

Written when the emitter translated everything, before the re-encoder. The
re-encoder copies the guest's instructions with its registers in host
registers (4), and emitted blocks now take 0.3% of 7-Zip's samples, so 2
and 3 matter only for code the re-encoder does not take.

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

- **The call landing.** Each return checks the guest's address with a
  flag-free sequence (5 instructions); where the flags are dead at the
  call's next instruction, a `cmp`/`jne` would do.
- **Cold code between hot blocks.** Every exit has its own stub and
  reconciliation path, so a 32-instruction block takes about 3 KiB, most
  of it never run. Sharing one leave routine per engine would put the hot
  code of consecutive blocks closer together.
- **What the re-encoder does not take yet** ends its block and goes to the
  emitter, which stores and reloads the pinned state: push/pop of 16-bit
  operands, pusha/popa, std and cld, and register forms that need REX
  with ah-bh (`movzx edi, ah`). (div and idiv are re-encoded now: the
  divisor is checked before the host's instruction, and a divide error goes
  back to C the way it did before. So are ah-bh beside a memory operand,
  through `r10b` with BMI2's flag-free `rorx` where the address needs REX,
  and `bt`/`bts`/`btr`/`btc` on memory with a register offset, whose unit is
  found and guarded before the host's instruction runs on it.) Under native
  FP such a crossing also
  moves the FP state through C, as in nbench's STRING SORT.
- **The guard's flag save.** When flags are live across a memory access,
  the guard wraps its compare in `lahf`/`seto` and `sahf`; a flag-free
  bounds check (for example with `bextr`/`lea` and `jrcxz`) would remove
  that.
- **Flag capture.** Producers still capture flags with `pushfq; pop` when
  the branch is not adjacent.

## On the console

The `sevenzip-bench` profile from
[prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles) runs the same benchmark under the title:

1. Copy the i386 `7za.exe` from the pinned package to the prefix's
   `drive_c/Tools`.
2. Add the profile to the library's `profiles/` directory, then start the
   title.
3. Choose "7-Zip benchmark". The profile's `arguments` line
   (`b -mmt1 -md22`) is passed to the program. The title forwards its
   standard output (fd 1) to ps5log as `STDOUT` lines.
4. When the program exits, the title returns to the launcher. Feed the
   `STDOUT` lines to `tools/bench_7zip.py`, which reads the `Avr:`/`Tot:`
   rows.

nbench and pi run the same way, with the `nbench-x87` and `pi-x87`
profiles; that repository's `benchmarks/build.sh` builds all three
programs from their pinned sources.

The console cannot run the i386 programs without the DBT, but it runs PE64
programs natively (the title runs x64 code as is). The same repository's
`sevenzip-bench-x64`, `nbench-x64` and `pi-x64` profiles run x64 builds of
the same sources, which give the console's native speed.

Measured on 2026-09-28 (FW 12.02) with the changes above through native
FP; the last 7-Zip round and the FP benchmarks also had the
working-directory fix (#201). Three rounds of 7-Zip, one of each FP
benchmark, ps5log `20260928T114656843Z`, `20260928T120539847Z`,
`20260928T123134239Z`, `20260928T123150939Z` and `20260928T123626260Z`:

| Benchmark | PS5, DBT | Host, DBT | PS5 / host |
|---|---|---|---|
| 7-Zip, total MIPS | 3361–3369 | 4685 | 0.72 |
| nbench x87, integer index | 166.7 | 235.2 | 0.71 |
| nbench x87, FP index | 81.6 | 120.5 | 0.68 |
| pi x87, 4.2M digits | 13 s | 7.63 s | 0.59 |

pi's timer counts whole seconds. The console runs the same DBT at 0.6–0.7
of the host's speed.

### Native baseline on the console

The x64 builds ran natively on the console the same day (ps5log
`20260928T134849016Z`, `20260928T143813974Z`, `20260928T144309104Z` and
`20260928T144325507Z`; two more i386 7-Zip rounds in between rated 3365
and 3363). An x64 build is not the i386 one: it has more registers and uses
SSE2 for floating point. On the host, where both builds run natively, the
x64 builds are faster by the factor in the fourth column, so the console's
i386 native speed is estimated as its x64 speed divided by that factor:

| Benchmark | PS5, i386, DBT | PS5, x64, native | Host: x64 / i386, both native | DBT vs native (est.) |
|---|---|---|---|---|
| 7-Zip, total MIPS | 3361–3369 | 5148–5152 | 1.30–1.36 | 85–89% |
| nbench, integer index | 166.7 | 185.5 | 1.03 | 93% |
| nbench, FP index | 81.6 | 114.9 | 1.21–1.28 | 86–91% |
| pi, 4.2M digits | 13 s | 8 s | 1.4 (7 s / 5 s) | about 85% |

On the console the DBT therefore runs at an estimated 85–93% of native,
slightly below the 91–98% measured directly on the host. The pi estimate is
the roughest, since its timer counts whole seconds.

Natively the console runs at 0.72 of the host on 7-Zip (5150 against 7140),
0.76 on nbench's integer index, 0.81 on its FP index and 0.63 on pi. pi is
as slow natively as through the DBT, so its lower ratio comes from the
console's CPU, not from the DBT (its Zen 2 cores are reported to have a
reduced FPU).

The earlier run was on 2026-09-27 (FW 12.02, one run, main at `cd92d61`
with the re-encoder and quantum 1024). 7-Zip reported the CPU as "AMD Eng
Sample 100-000000189-11" at about 3460 MHz.

| Config | Compress MIPS | Decompress MIPS | Total MIPS |
|---|---|---|---|
| PS5, DBT (`wowprospero`) | 1252 | 1514 | 1383 |

For scale, the host's DBT
run of the same build rated 1416 total at 29% of its native 4850, on a
faster core. Getting 1383 on a 3.46 GHz Zen 2 core puts the console within
the same ratio range.

### Unmap notification correctness

The WoW64 CPU backend captures the complete mapped-view or image extent
before an unmap and invalidates that range after success. A failure anywhere
in the region walk falls back to full invalidation; a partial extent could
leave translated code from the remaining regions alive. Failed unmaps do
not invalidate code.

Before/after pairs retain their caller thread, original address and nested
order. A failed query still reserves a pair so its completion cannot consume
an older extent. The bounded 64-record bank falls back to full invalidation
on saturation or ambiguous ordering until outstanding pairs drain. Queries
and invalidation run outside the bank lock.

`tests/test_wowprospero_unmap.py` compiles and executes the actual callback
code with controlled VM replies, including multi-region views, partial query
failure, nested/reentrant callbacks, eight concurrent threads, saturation
and recovery, failed unmaps, and nonprogressing or overflowing regions.
Both `make test` and `make sanitize` run it with their selected compiler and
flags. These tests establish callback behavior; they do not establish a
game performance improvement or console compatibility.

A four-run PC comparison kept the chain-hash Unix adapter identical and
changed only the PE unmap callbacks (scoped/global/global/scoped).
Each completed all 5182 `hl2long` frames without detected DBT diagnostics
or reported clock errors. Scoped unmaps took 85.529/84.582 demo seconds
versus 87.372/89.721 for global unmaps, with inferred pre-demo intervals
of 32.9/30.3 versus 136.2/131.9 seconds. Calibrated translated-invocation
CPU estimates were 176.470/187.806 versus 179.022/182.948 seconds: the
scoped candidate's median estimate was 0.64% worse. Faster loading does
not establish the translated CPU-time target.

These are diagnostic results: Remote Play consumed roughly 1100–1200%
host CPU, local gate revalidation overlapped one baseline run, and a short
FP-transfer microbenchmark overlapped the final candidate. Sampled counters
have incomplete phase brackets and unsampled intervals; phase boundaries
are inferred from the timedemo duration. They cannot establish a controlled
causal gain. Runs used forced own-prefix cleanup after post-demo reports;
clean shutdown remains unverified. Console measurements remain pending.

[b]: https://box86.org/2022/03/box86-box64-vs-qemu-vs-fex-vs-rosetta2/

## Cache lookup diagnostics

`PW_WOW_TIMING` (the console trigger `pw_wow_timing`) also emits an additive
`wowprospero cache` row per owner thread at each timing report and termination.
It reports cumulative hits, misses, probes, publishes and resets, plus the
current capacity, generation, occupancy and arena use. Occupancy is successful
publishes since the last cache reset; rejected publications do not count.
An instance ID separates reused guest thread IDs in the same process.
The report reads existing counters outside signal handlers, without walking
the table or adding per-lookup accounting. The existing timing row is unchanged.

Analyze one process capture with:

```sh
python3 tools/pw_cache_stats.py run.log --output cache-stats.json
```

The tool subtracts adjacent cumulative records and weights average probes by
lookup count. It preserves intervals spanning resets and flags regressing
counters, inconsistent occupancy or clocks, and changed capacity. It does not
combine distinct instances, sum cumulative snapshots or infer missing early
work. `max_probe` is a lifetime maximum, not the interval's maximum. These
counts explain lookup behavior; they do not measure CPU time or establish a
frame-rate gain. `probes` and `max_probe` cover only cache lookup calls: the
publication walk and the engine's compile-time chain-patch walk are uncounted.
They can also traverse long clusters, so these counters do not represent
all probing work per compiled block. Apply identical diagnostics to both comparison builds, then
repeat gameplay with diagnostics off to assess reporting overhead.
## Thread-owned hotspot sampling

On Linux, set `PW_WOW_PROFILE=1` to log each thread's top 20 translated
blocks every five seconds. A filename instead writes `<filename>.<thread-id>`
for each thread, replacing its previous window. `PW_WOW_TIMING=1` supplies
the separate run/Unix/system-call timing split. The default fault-marker
mode supplies the arena block map; sampling with fault markers disabled is
refused explicitly.

`wowprospero profile` reports the window duration, translated-arena samples,
stub samples and histogram overflow. `wowprospero hotspot` names the guest
PC and samples in re-encoded entry, body and exit code, or older emitted
code. The process CPU timer fires every millisecond; these counts are
statistical samples, not instruction counts or exact per-block timings.
Per-thread histograms ignore signals outside translated arenas; cumulative
`profile_process` tick and unattributed counts retain the process denominator.
The per-thread `outside=0` does not imply
that the process spent no time in native code. Compare identical workloads
and use the timing split alongside these records.

Each thread owns a bounded 4096-slot histogram. The signal handler resolves
the interrupted PC immediately, before an arena reset can reuse its address.
It uses no compiler TLS access, allocation, formatting or source-byte reads.
Reporting snapshots and clears that thread's histogram with SIGPROF blocked;
it does not scan another thread's cache. Overflow is reported without evicting
rows, and makes the top-block list incomplete. Logs contain addresses and
counts only; the former binary block dumps are no longer produced.

The sample-aggregation code is portable and host-tested, including collisions,
full capacity, arena boundaries and cache resets. On the console, create
`/data/prospero-win/pw_wow_profile` before launching a fresh game process;
remove it to disable sampling for subsequent processes. Records use Wine's
normal output sink, and a timer installation failure disables sampling with
a diagnostic. The pinned SDK exports `setitimer`. The exact f7ff5244 build produced nonzero
main-thread samples on the PS5, completed route v6 and exited cleanly through
the Kleiner lab with corrected-unmap PE4c8f7118. That validates the observed
sampling and shutdown path; a console report with zero samples still cannot
be used as a performance result. Updated cap builds require their own receipt.

`wowprospero native` supplements translated-block records with a bounded,
atomic process-wide histogram of PCs sampled outside translated arenas.
These counts are cumulative, and `native_summary` reports overflow. Linux
reports the current module/symbol when `dladdr` resolves the address; console
addresses can be resolved against the exact linked ELF. Module attribution
may change after an unload, so retain exact artifacts and loader records.
No raw guest code is dumped. Use these records to distinguish translation
or cache-reset work from execution of translated instructions.

## Reset poisoning extent

The engine now poisons only the published code extent when discarding its
cache, and clears the corresponding block-map prefix. Previously every reset
wrote `0xcc` across the entire reserved arena (128 MiB for the first WoW64
thread), even when a loader flush had discarded only a few blocks. Native-PC
sampling during HL2 with DXVK identified these writes as the dominant startup
cost. Generation changes, cache metadata clearing, indirect-target clearing
and poisoning of discarded instructions are preserved. Tests check that old
code becomes traps, unused arena bytes remain untouched and execution after
reset recompiles correctly. This removes reset overhead; the separate
steady-workload HL2 translated-time target still requires measurement.

## Execution CPU timing

`PW_WOW_EXEC_TIMING=1` enables an optional owner-thread CPU clock around each
sampled generated-code invocation. On PS5, use the separate
`/data/prospero-win/pw_wow_exec_timing` trigger. This also enables periodic
timing reports. `wowprospero execution` records cumulative `sample_cpu_ns`,
invocation `calls`, measured `samples`, `stride` and `clock_errors`; totals
survive cache resets and are reported again
at thread termination. Subtract two records from the same thread and process
to measure a fixed workload. Do not sum cumulative windows. The default
stride is 64: a pseudorandom sequence selects roughly one invocation in 64,
avoiding a fixed periodic sampling pattern. `PW_WOW_EXEC_STRIDE=1` measures
every invocation; it can substantially slow dispatch-heavy workloads. For
sampled mode, `delta(sample_cpu_ns) * delta(calls) / delta(samples)` estimates
the total; it is not an exact time. Repeat workloads and report sample counts
and uncertainty; a short run with few samples cannot certify a speedup.

Each thread separately reports `clock_resolution_ns` from `clock_getres`,
and `clock_batch_read_ns`, the median of eight batch means, each spanning
1024 consecutive clock reads. The batch mean includes loop and validation
work, so it is a diagnostic estimate, not an exact read cost. A valid zero
batch mean does not disable timing. Report raw sampled CPU and sample counts
as the primary comparison; do not subtract a per-sample calibration value.
The old median of adjacent read pairs was resolution-limited on the PS5 and
its 1000 ns result did not establish 1000 ns of read overhead. Earlier
calibration-subtracted estimates cannot certify a speedup.

This clock excludes compilation, cache reset, dispatcher work and time when
the thread is descheduled. It includes generated entry/exit code and the FP
invocation wrapper, plus any signal-handler CPU work interrupting an invocation.
Two thread-clock reads per sample add overhead, including a clock-read cost
in the measured interval; compare identical builds
with timing enabled, and check FPS with timing disabled too. Use a timedemo to
avoid counting frame-pacing spin loops, repeat short demos and exclude loading.
The clock is optional and defaults off; it is not used in the signal handler.
Unavailable clocks disable timing explicitly, and nonzero `clock_errors` make
a measurement invalid. Console clock operation still needs hardware validation.

### HL2 host check (2026-10-02)

A short `d1_trainstation_01` timedemo was run twice per build, interleaved
baseline/candidate/baseline/candidate. Both used the same Wine, game prefix,
DXVK, demo and sampled execution clock (stride 64, with calibration). The
baseline precedes both reset optimizations and has the same timing code
applied privately; the candidate includes bounded poisoning and touched-slot
metadata reset. Each completed timedemo measured only 158 frames after the
engine's warmup, so these results are too short to certify a performance gain.

| Build | First run | Second run | Median frame time |
|---|---|---|---|
| Before reset optimizations | 1.789 s, 88.31 FPS | 1.734 s, 91.12 FPS | 11.15 ms |
| With reset optimizations | 1.869 s, 84.53 FPS | 1.755 s, 90.03 FPS | 11.47 ms |

The candidate's median frame time was 2.9% longer. This does not reproduce
an earlier improvement from a single pair of runs and does not establish a
steady-gameplay speedup. Median launch-to-result time fell from 146.1 s to
49.8 s, consistent with the reset changes targeting loading. Launch time
includes Wine startup, map loading, demo warmup and the measured frames;
it is not translated-code execution time.

All four timedemos completed with no DBT fault diagnostics and no execution
clock errors. The harness stopped its own Wine prefix after obtaining each
result; these runs do not establish clean game shutdown. Execution counters
include loading, and their periodic reports do not isolate the short measured
frame interval. A longer fixed workload with explicit timing boundaries,
console measurements and Counter-Strike/Warcraft III regression runs remain
required before claiming the HL2 translated-time target.
## Empty chain-table targets

An all-zero re-encoder chain table treated guest PC zero as a tag hit and
jumped through its empty host pointer. Null indirect calls, jumps and
returns could therefore fault at host RIP zero outside translated code,
instead of returning to guest exception handling. Clear the table with a
PC-one sentinel in slot zero at allocation and reset. PC one hashes to
slot one, so this empty sentinel cannot match a requested target; a real
translation at PC zero replaces it normally. No instruction is added to
the generated lookup. A missing executable source span is also reported as
a guest access violation at its EIP, rather than an internal DBT error;
previous data-fault metadata is not reused for that instruction-fetch fault.

Regressions execute null register and memory calls/jumps and a null return,
checking dispatcher result, registers, flags and data against the emitter
in ordinary, unbounded, call-stack and superblock modes. A lifecycle test
also executes null lookups before and after reset and publishes real PC-zero
code in each generation. The same reporting helper used by the Unix adapter
is checked with the actual missing-source result and stale fault metadata,
plus data faults, unsupported instructions, x87 traps and internal errors.
An unmodified-engine negative control hits the host-null fault; the regression
uses a fixed source PC so random mapping cannot hide it by filling slot zero.
`make -j2 all audit check-whitespace` and `make -j2 sanitize` pass, including
362 differential forms with zero mismatches (93 unsupported forms skipped).
The PS5 SDK compiles and links the updated Unix adapter and engine. Console
execution and clean-game validation remain pending; this correctness fix is
not a performance result.

PR288 is independent of the touched-slot reset trial (#287); its console
baseline retains the merged occupancy cap, profiler and null-target fix.
Use identical timing code on both sides of each speedup comparison.

## HL2 shutdown padding instruction

A cache-disabled HL2 comparison reached shutdown and reported an
untranslatable instruction at `steam_api.dll + 0x2051`. The exact bytes were
`66 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00`, a 15-byte NOP padding form.
This was discovered after the measured demo completed; the earlier forced
shutdowns did not exercise the same path.

The emitter and reencoder now share a bounded padding recognizer for `90`
and `0f 1f /0` with operand-size, address-size and segment prefixes. It parses
the address encoding for instruction length only: NOP neither evaluates a
memory operand nor changes guest registers or flags. It rejects overlong
instructions, invalid sub-opcodes and incomplete encodings. LOCK/REP and
other instructions retain their existing decoding paths. The instruction
semantics follow Intel's [NOP instruction reference](https://cdrdv2-public.intel.com/835752/253667-sdm-vol-2b.pdf).

Regressions execute the exact form and address-size/segment variations in
all four emitter residency/flag modes, ordinary reencoding and superblocks.
They include native execution of the exact padding form, unchanged flags,
unmapped nominal operands, every truncation boundary, invalid sub-opcodes
and a 16-byte encoding. Full HL2 startup and shutdown validation on PC and
console remains required before declaring this correctness issue resolved.

PC validation subsequently completed three full 5,182-frame demos without
DBT or guest-exception diagnostics. Two close requests timed out; capturing
the owned game window identified a Source assertion at `threadtools.cpp:1667`
about illegal worker-thread termination. On the third run, acknowledging
that single recorded assertion let HL2 exit with status zero and without
forced cleanup or an unsupported-instruction message. The original prefix
DLL/config were restored after each run. This is exit after an assertion
acknowledgement, not unattended clean shutdown; the assertion's cause and
console startup/shutdown validation remain outstanding. Normal/sanitizer
test suites and all three differential modes pass (zero mismatches).

### Dispatcher chain-table residency

`PW_WOW_DISPATCH_PROFILE=1` enables cumulative counts of the re-encoded
chain table's state when a PC arrives at the C dispatcher. On PS5, create
`/data/prospero-win/pw_wow_dispatch_profile` before starting the game.
`wowprospero dispatch` reports `table_matches`, `table_empty` and
`table_collisions`; the last means the indexed slot holds another guest PC.
Counts persist across cache resets, and collection defaults off. The flag
also enables periodic timing reports without requiring an execution CPU clock.

This distinguishes repeated dispatcher arrivals whose target is resident
from arrivals that encounter an absent or colliding target. It does not
instrument generated exits and does not count exact indirect lookup misses:
arrivals also follow native calls, safepoints and emitted blocks. A collision
at arrival is evidence of table aliasing, not proof that a collision caused
that return. Compare these counts with the FP-wrapper/native samples before
changing the chain table hash or size. Tests execute two compiled PCs sharing
a chain slot, verify replacement and cache-hit execution, and check reset
and disabled collection. Console reports remain unvalidated.

### Full-PC chain hash

The chain table used to be indexed by the low 16 bits of the guest PC, so
any two targets 64 KiB apart landed in the same slot and evicted each other.
It is now indexed by `(pc + bswap32(pc)) & 0xffff`, which mixes every byte
of the PC into the slot number. Publication, the dispatcher's residency
profile, generated dynamic exits and superblock side exits all use the same
`pw_x86_chain_slot()` index. The generated lookup computes it with MOV,
BSWAP, LEA and MOVZX, none of which change the guest's arithmetic flags.
The emitted-block indirect table keeps its own hash.

PC one still maps to slot one, so the empty-table sentinel from the null
target fix cannot match a real request. Tests cover slot replacement under
the new hash, cache hits, reset, indirect calls and returns that preserve
carry, zero, parity, sign and overflow, and superblock side-exit linking.

On the PS5, the `hl2long` timedemo (5,182 frames, vsync-bound at 59.8 fps
either way) was run with the execution clock enabled, alternating main and
this change. Each figure below is the estimated CPU time spent in translated
code over the whole session, from `sample_cpu_ns / samples * calls`:

| Build | Main thread | Busiest worker |
| --- | ---: | ---: |
| main `a620f92` (three runs) | 106.5–107.2 s | 92.1–92.9 s |
| full-PC chain hash (two runs) | 102.0, 102.8 s | 73.8, 74.6 s |

These raw whole-session estimates are about 4% lower on the main thread
and 20% lower on the worker. Some threads reported nonzero clock errors,
which invalidate their affected timing comparisons; their effect cannot
be dismissed from the error count alone. The counters also include loading
and post-demo work, without measured-frame phase boundaries. Keep the
figures as a preliminary screen, not proof of those CPU savings or extra
headroom. The demo remains capped at the display's 60 Hz.

The change was also played through the opening of `d1_trainstation_01` on
the console with the route controller, from the train to Barney's monitor,
and then quit from the game. It held 59.9 fps throughout (minimum 59.9),
reported no translator or guest exceptions, and Wine exited normally.

### Shared fault-exit epilogue

Every memory access in a re-encoded block has a cold path that runs if the
access faults. Each of those paths used to end with its own copy of the
register and flag save plus the return to the dispatcher. They now share a
single copy. Each fault site still stores its own guest EIP, restores any
saved flags and records its own fault address, width and write flag, then
takes a flag-free jump to the common epilogue; the last site simply falls
through. Fault-table entries keep pointing at each site's own prefix, so
fault reporting is unchanged. A block with one fault site gains no jump,
and a block with none gains no epilogue.

The saving grows with the number of accesses. In a size probe of identical
blocks of absolute dword loads, with fault markers and the host call stack
enabled, the emitted code shrank as follows:

| Memory accesses | Before (bytes) | After (bytes) | Reduction |
| --- | ---: | ---: | ---: |
| 1 | 361 | 361 | 0% |
| 2 | 482 | 421 | 12.7% |
| 4 | 724 | 541 | 25.3% |
| 8 | 1,208 | 781 | 35.3% |
| 16 | 2,176 | 1,261 | 42.0% |
| 32 | 4,112 | 2,221 | 46.0% |

The regression test faults each of fifteen loads in one block, after ADC
instructions that change registers and consume flags, through both guarded
and native-fault paths, with and without the host call stack. Each path
must report the same EIP, registers, flags and fault metadata as before.

On the PS5, the `hl2long` timedemo (5,182 frames, vsync-bound at 59.8 fps
either way) was run with the execution clock enabled, alternating main and
this change. Each figure is the estimated CPU time spent in translated code
over the whole session, from `sample_cpu_ns / samples * calls`:

| Build | Main thread | Busiest worker |
| --- | ---: | ---: |
| main `a620f92` (three runs) | 106.5–107.2 s | 92.1–92.9 s |
| shared fault exit (two runs) | 102.1, 102.6 s | 90.3, 89.9 s |

The raw whole-session estimates are about 4% lower on the main thread and
2.5% lower on the worker. Smaller generated code is established by the
size probe above; its effect on CPU is a hypothesis. These cumulative
figures lack measured-frame phase boundaries, and any thread with nonzero
clock errors is invalid for a timing comparison. The capped frame rate
does not establish a CPU saving.

These timedemo figures were measured against main before the full-PC chain
hash landed. The two changes were then combined on the console, played
through the opening of `d1_trainstation_01` with the route controller and
quit from the game: 59.9 fps throughout (minimum 59.9), no translator or
guest exceptions, and a normal Wine exit.

### Second chain-table bank

Even with the full-PC hash, two hot targets can still land in the same
chain slot and keep evicting each other. The chain table now has a second
bank of 65,536 slots behind the first. When a newly published PC collides
with a different PC in its primary slot, the old entry moves into the same
slot of the second bank instead of being dropped. Generated dynamic exits
and superblock side exits check the primary slot first and probe the second
bank only on a mismatch; the probe uses LEA, MOV, NOT and JRCXZ, so guest
flags survive, and guest ECX is restored from R9 on a miss. Both banks get
the null-target sentinel, are cleared on reset, and are counted by the
dispatcher residency profile. The table grows from 1 MiB to 2 MiB per
translating thread.

On the PS5, with the full-PC hash and the shared fault exit already in main,
the `hl2long` timedemo was alternated between main and this change (5,182
frames at 59.8 fps either way, execution clock on):

| Build | Main thread | Busiest worker |
| --- | ---: | ---: |
| main `f2b0379` (three runs) | 96.5–97.9 s | 71.3–71.6 s |
| second chain bank (two runs) | 94.0, 95.9 s | 69.3, 69.0 s |

Both candidates' raw estimates are below every control on both threads,
but each candidate reports `clock_errors=1` on both threads. The apparent
2–3% reduction is therefore not an accepted CPU measurement. The captures
also include loading and post-demo work rather than just measured frames.

At the same recorded native tick, process CPU was 252.438, 242.558 and
227.166 s for the controls, versus 251.157 and 253.138 s for the candidates.
The candidate median is 3.95% higher, while the control range spans 11.12%
of its median. This does not establish a process-CPU regression either:
thread `0048` alone varies from 129,033 to 54,443,821 translated invocations
across the controls. The tick is a captured record, not a demo-phase boundary.
Process CPU and the translated-invocation estimates cover different work
and are not interchangeable.

Dispatcher invocation counts are more consistent on the two reported busy
threads and do not depend on the CPU clock:

| Thread | Control calls (three runs) | Second-bank calls (two runs) | Median change |
| --- | --- | --- | ---: |
| `0024` | 32,265,545; 32,268,907; 32,277,310 | 29,524,398; 29,522,607 | -8.51% |
| `00b0` | 23,728,184; 23,726,021; 23,725,602 | 21,568,800; 21,567,610 | -9.09% |

These counts describe entries into generated chains, not retired guest
instructions or a CPU-time saving. Fewer entries also mean fewer timed
samples, so scaled clock-read overhead can contribute to the lower raw
estimates. No per-read cost has been subtracted to manufacture a corrected
gain.

**Decision: retain #326.** The consistent reduction in dispatcher entries
supports its intended collision-handling mechanism, and the host tests
cover displaced targets, reset, null sentinels and register/flag preservation.
The additional bank costs 1 MiB per translating thread and probes only after
a primary mismatch. The PS5 opening of `d1_trainstation_01` was played
through and quit: 59.9 fps on average (minimum 59.2), no reported translator
or guest exceptions, and a normal Wine exit; final-runtime Counter-Strike
and Warcraft III smoke checks were also reported. Those checks support
keeping the implementation without a proven 2–3% CPU claim. Reconsider it
if a controlled comparison finds a regression or the extra memory becomes
a problem.


### Reset protection and lazy arenas

Reset makes only the published code's page extent writable before poisoning
it. A refused reset generation restores executable protection over that same
extent. An empty reset requires no protection call. Protecting the complete
reserved arena would make Wine's lazy backend commit its unused tail, even
though the poison writes already stop at the published cursor.

A host test reserves 32 MiB, translates within the first 1 MiB commit step,
then checks both a refused and a successful reset. Commitment stays at 1 MiB;
published bytes are poisoned and recompiled execution still works. A separate
POSIX test uses real page protection changes, refuses zero and unchanged
generations, and executes the original cached block after each refusal. It also
checks that a successful reset clears the sealed state before recompilation.
These are synthetic allocation/correctness results, not PS5 memory or performance
measurement. Console census and regression checks remain pending.

## Refused bit-count reads

The re-encoder keeps the original EIP, GPRs and arithmetic flags when a
TZCNT/LZCNT memory guard refuses the source read. The instruction has not
completed at that point. Regression checks cover 16- and 32-bit operands,
with and without FS, and require a read fault at the effective address.
This is a deliberate exception to the re-encoder's existing relaxation for
dead flags on other flag-defining memory instructions. It matches the older
emitter for these newly supported forms; it does not establish exact incoming
flags for every faulting memory instruction.

Periodic translated-run percentages describe wall-time attribution. They
do not measure thread CPU utilization or the number of cores freed.

## Fixed-route console results and runtime defaults

The console owner accepted the combined shared-mutex/image-view defaults
and one-file `/dev/null` wrapper after ordinary PE semantics, HL2 with
Wine-exit, loading and 600-second gameplay checks on 2026-10-04. With the
signed-bit-offset/high-byte translator and cached computer-name DLLs, the
GTA IV city run reports 53.1 mean approximate FPS and a 45.4 sampled minimum
over the fixed 200–440-second window. This remains below the 58-mean/50-minimum
target at the accepted goal settings and with profiling off.

These figures describe the combined runtime configuration. They do not
attribute the improvement to one translator change, convert profiling wall
time into CPU time, or validate later event/semaphore or profiler candidates.
The [shared-mutex acceptance receipt](SHARED_MUTEX_BACKEND.md) records the
configuration and regression checks; the
[image-view lifetime contract](IMAGE_VIEW_FD_LIFETIME.md) describes the
descriptor policy and its retained debugger limitation.

## FS memory pushes, byte-table lookup and direction flags

Both translators take 32-bit memory pushes with an FS override, including
absolute, base/displacement and SIB operands. FS applies to the source read;
the destination stays on the ordinary guest stack. The source address uses
the original ESP, and a refused source or stack access leaves ESP unchanged.

XLAT uses the unsigned byte in AL to index the table at EBX (plus the guest
FS base when present), updates only AL, and preserves flags. STD and CLD
update the guest DF bit in state. The host DF stays clear outside the native
string instruction so returns to C obey the host ABI; string operations use
the guest direction and clear host DF again afterwards.

Tests compare FS pushes with the existing native fallback and XLAT with the
native instruction, check register preservation, byte indices, flags, guarded
faults, marked accesses and warm-cache reuse. The older emitter also runs the
cases in all residency/lazy-flag combinations. These checks establish host
correctness and coverage; a console comparison of the same workload is
required to establish a game performance benefit.

Native-fault checks also exercise refused FS source reads, FS-push stack
writes and flat/FS XLAT reads with guest DF set, both with and without the
host call stack. Marker recovery must match the guarded EIP, registers,
arithmetic flags and access metadata, preserve guest DF, and leave host DF
clear when control returns to C.

The console owner reports two alternating 480-second comparisons at the goal
settings on 2026-10-05, with sync disabled and enabled. Coverage wins three
of four matched pairs, averaging about 1.3 approximate FPS above its controls.
These are preliminary comparisons: coverage HL2 and 600-second stability
validation remain pending, and the 58-mean/50-minimum city target remains unmet.
