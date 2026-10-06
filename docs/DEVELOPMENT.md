# Development workflow

## Host setup

Host checks target x86_64 Linux. Ubuntu 24.04 is a suitable starting point;
a Linux VM or WSL2 can run host checks. Playing installer windows in WSL2
also requires working WSLg, and has not been validated as a player setup.

For portable code and documentation:

```sh
sudo apt-get update
sudo apt-get install build-essential clang git python3 python3-yaml
python3 tools/check_setup.py
make -j2 all
make -j2 sanitize
```

The checks build their own synthetic inputs. You do not need Wine, the PS5
SDK, a game or a console. Some optional Wine cross-checks skip when their
inputs are absent; `make wine-check` is the strict release check.
CI performs the host and sanitizer checks on every PR.

For Wine/console development, add:

```sh
sudo apt-get install bison flex pkg-config gcc-mingw-w64 g++-mingw-w64 \
    libx11-dev libxext-dev libfreetype-dev libfontconfig-dev libvulkan-dev \
    libgnutls28-dev libasound2-dev libpulse-dev zlib1g-dev zip unzip
python3 tools/check_setup.py --wine
tools/build_wine_runtime.sh --jobs 4
```

The runtime script obtains the pinned upstream Wine checkout. The native
builder obtains its pinned payload foundation. The Wine PRX converter
requires a separate foundation with module exports; use the documented
`PS5_PRX_FOUNDATION` revision in [Wine build notes](WINE_PS5_BUILD.md#prx-modules).
Graphics backends also need their own SDK/build inputs. Inspect every
configure warning: a build without X11/FreeType is not an installer kit.

## A focused review

Use your fork for public contributions. Make a branch for the change and
open a PR against `main`; [CONTRIBUTING.md](../CONTRIBUTING.md) lists the steps
and evidence. `main` is protected by a repository ruleset on GitHub, kept in
`.github/rulesets/main.json`: every change goes through a pull request, the
`host-contracts` CI check must pass, and the branch can't be deleted or
force-pushed. Nobody can bypass it, the maintainer included. It needs no
approving review, so the maintainer can merge their own pull requests. Checking
in a change to that file doesn't change the live rules: apply it through
GitHub's settings or the rulesets API, and read the active result back.

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

The native-title build downloads the release of
`mpereiraesaa/PS5-Lapy-JB-Daemon` pinned in `tools/build_native.sh`
(`lapy_release`) on each run and requires its `lapy.elf` and
`lapy-manifest.json` assets. It checks the ELF against the pinned SHA-256
(`lapy_elf_sha256`) and verifies the ELF and protocol digests before
staging the helper in `dist/PPSA99995`; the manifest must also declare the
`root_layout_probe_retry` feature. A release without it is rejected, with no
local stale-helper fallback.

`tools/build_native.sh` accepts `PW_OUTPUT_SUFFIX` for an isolated
build/dist pair, and `PW_WINE64_SCRIPT=1` with `PW_WINE64_SECONDS=<s>` for an
unattended launcher run that opens and closes the first game.
`PW_FOUNDATION_READY=1` verifies an already prepared payload SDK instead of
rebuilding it. `tools/build_wine_ps5.sh` needs the host WoW64 Wine build from
`tools/build_wine_runtime.sh`; its environment is described in
[WINE_PS5_BUILD.md](WINE_PS5_BUILD.md#build).

The runtime title uses the pinned foundation's high-address native layout:
its executable segments begin at 4 GiB, leaving `0x00400000` available for
Windows games that cannot be relocated. The native converter is built under
a filename tied to its pinned source commit, so an existing foundation
checkout cannot silently reuse a converter with the old segment addresses.

The Wine runtime is staged beside the title under `win/wine` and the library
(profiles, input presets, prefixes) under `/data/prospero-win`; see
[WINE_PS5_BUILD.md](WINE_PS5_BUILD.md#starting-wine-in-the-title). A
hardware claim needs the exact artifacts and the title's `ps5log/1` records
([telemetry](TELEMETRY.md), [hardware validation](HARDWARE_VALIDATION.md)).

## Packaging the app

The folder a player uploads to `/data/homebrew` is the title with its Wine
runtime beside it. `tools/package_release.sh` puts it together from four
builds:

```sh
tools/build_native.sh                  # the title: dist/PPSA99995
tools/build_wine_ps5.sh --radv <PS5_Vulkan checkout>   # Wine's PS5 modules, RADV
tools/build_host_wine.sh               # the same Wine for the PC: its PE modules
tools/build_wowprospero.sh             # the 32-bit CPU: wowprospero.dll
tools/package_release.sh --title dist/PPSA99995 --wine-ps5 .deps/wine-ps5 \
    --host-wine <host Wine install>/usr \
    --cpu-dll <Wine build>/dlls/wowprospero/x86_64-windows/wowprospero.dll \
    --lapy-release build/native/lapy-helper-release.json \
    --out release --zip
```

It writes `release/PPSA99995/` (about 350 MB) and, with `--zip`,
`release/PPSA99995.zip`. The title's `dev.conf`, which names the builder's
PC, is left out; a player adds their own to get a log. Wine's PE modules are
the host build's, less import libraries and the drivers that need the PC's
own libraries (X11, GStreamer, pcap, scanners).
`eboot.bin` and every `.prx` are marked executable: the console refuses to
exec an eboot, or to load a module, without that permission, and the zip
keeps the modes.

The folder also carries the licences of everything in it: `LICENSE`,
[`THIRD_PARTY.md`](../THIRD_PARTY.md), which says what each part is and
under which licence, and `LICENSES/`, which adds the texts Wine and FreeType
ship to the ones in this repository. `SOURCES.txt` records the source
revision of each part, read from the builds themselves (the Wine build's
`report.json` and the helper's `release.json`). Packaging stops if one of
those is missing, or if `libvulkan.prx` is not the RADV build the notices
describe. For an OpenGL package, pass `--ps5-opengl-sdk` the SDK that
ps5-opengl's `make sdk` installed inside its own checkout
(`build/sdk/ps5-opengl-gl46`), so the report can name the commit it was
built from.

## Making a release

Players download the app folder from the repository's Releases page. A
release is a version tag plus the zip from `tools/package_release.sh`:

```sh
git tag v0.1.0 && git push origin v0.1.0
# The Release workflow runs the host checks and opens a draft release.
tools/publish_release.sh --tag v0.1.0 --zip release/PPSA99995.zip
# Check the draft on GitHub, then:
tools/publish_release.sh --tag v0.1.0 --zip release/PPSA99995.zip --publish
```

The zip is built on your machine because the PS5 builds need the payload SDK
and take hours; GitHub only drafts the release and hosts the file.
`publish_release.sh` uploads it as `prospero-win-<tag>.zip` with a
`SHA256SUMS` file, and refuses a zip that holds a `dev.conf` or any `.exe`
other than Wine's own programs, which sit beside its DLLs.
The draft's text comes from `.github/release-notes.md`, followed by the
merged pull requests since the previous tag. Before you publish, install
the zip on a console and run a game from a clean `/data/homebrew`, and
check that `SOURCES.txt` names published revisions only: every repository it
lists must hold that commit publicly, because it is how players get the
source the licences promise them.

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
