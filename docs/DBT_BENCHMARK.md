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
digits are chaining, register residency, lazy flags and the indirect target
table.

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
| **prospero-win DBT** | i7-12700H (x86-64) | **12%** | this page |

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

1. **The memory guard on every access.** `memory_address_width` runs about 9
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
