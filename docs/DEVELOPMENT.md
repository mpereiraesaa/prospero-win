# Development workflow

## Stable tree

`main` is reviewable, not a scratch area. Portable, host-tested code lives in
`src/`; the PS5 title lives in `native/`; Wine's PS5 shims, drivers and
patches live in `wine/`. Every change keeps `make all` green.

## Isolated experiments

Each risky change gets a sibling worktree, so it has its own build directory
while sharing history:

```sh
git switch main
git worktree add ../prospero-win-exp-<topic> -b exp/<topic>
cd ../prospero-win-exp-<topic>
make -j2 all
```

Commit small steps on `exp/<topic>`, validate them independently, and merge
only reviewed commits.

## Required gates

```sh
make -j2 test        # C unit tests and Python contract suites
make -j2 sanitize    # separate incremental Clang ASan/UBSan tree with leak detection
make audit           # fail-closed publication audit
make check-whitespace
```

Build objects and binaries stay under the ignored `build/host/` and
`build/sanitize/` trees. They are retained between test runs for incremental
rebuilds; use `make clean` when you explicitly want to remove generated files.
`make wine-check` additionally runs the suites that need the pinned Wine
checkout and the staged host runtime, failing instead of skipping when they
are absent.

## Building for the console

```sh
tools/build_native.sh                     # the title: dist/PPSA99995/eboot.bin
tools/build_wine_ps5.sh --check-patches   # validate the Wine patch series
tools/build_wine_ps5.sh                   # Wine's PRXs, fonts and report.json
```

`tools/build_native.sh` accepts `PW_OUTPUT_SUFFIX` for an isolated
build/dist pair, and `PW_WINE64_SCRIPT=1` with `PW_WINE64_SECONDS=<s>` for an
unattended launcher run that opens and closes the first game.
`PW_FOUNDATION_READY=1` verifies an already prepared payload SDK instead of
rebuilding it. `tools/build_wine_ps5.sh` needs the host WoW64 Wine build from
`tools/build_wine_runtime.sh`; its environment is described in
[WINE_PS5_BUILD.md](WINE_PS5_BUILD.md#build).

The Wine runtime is staged beside the title under `win/wine` and the library
(profiles, input presets, prefixes) under `/data/prospero-win`; see
[WINE_PS5_BUILD.md](WINE_PS5_BUILD.md#starting-wine-in-the-title). A
hardware claim needs the exact artifacts and the title's `ps5log/1` records
([telemetry](TELEMETRY.md), [hardware validation](HARDWARE_VALIDATION.md)).

## DBT work on the host

`tools/build_wowprospero.sh` builds the WoW64 CPU backend for a host Wine
tree, and `tools/run_wine_dbt_host.sh` runs a profile's PE32 application with
it (or, with `--cpu native`, with Wine's own `wow64cpu` as a control).
`build/host/bench_dynarec` and `build/host/dbt_differential` (built by
`make test` from `tools/`) are the controlled
performance and correctness comparisons; Pinball's gameplay is variable, so
its counts are functional evidence, not a benchmark.

## Working with real binaries

Windows binaries are private inputs. `.exe` and `.dll` are ignored
repository-wide, and the publication audit refuses any tracked file that
starts with a DOS header. Binary-specific analysis, decompiler workspaces and
chronological coverage notes stay outside the public repository. Checked-in
synthetic fixtures and aggregate regression inputs must be reproducible by a
documented tool, carry no proprietary bytes, and remain covered by the
publication audit.

## Adding portable code

- Keep `src/` free of platform imports. The allowed headers are
  `<stddef.h>`, `<stdint.h>`, `<string.h>` and `<limits.h>`;
  `tests/test_native_contract.py` enforces it.
- Take memory only from an injected backend or from the caller.
- Give every new capacity a compiled-in bound and a fail-closed overflow.
- Add the unit test in the same commit, and prefer asserting exact values
  over presence.
