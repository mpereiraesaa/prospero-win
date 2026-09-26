# Wine integration architecture

Wine is the Windows subsystem for prospero-win. DXVK is the Direct3D
implementation, and `ps5-vulkan` is its future native graphics backend. These
roles are complementary:

```text
Windows game (PE32 first, PE64 later)
  -> Wine PE DLLs: ntdll, kernelbase, kernel32, user32, gdi32, ...
     -> prospero-win Unix-call bridge and Windows object services
        -> PS5 platform adapters: VM, files, threads, audio, input, network

  -> DXVK d3d8/d3d9/d3d10/d3d11 + DXGI DLLs
     -> ps5-vulkan
        -> AGC / VideoOut / gfx1013
```

The current Pinball path binds selected Win32 imports directly to
`pw_win32.c`. That was the shortest route to prove the loader, DBT, GDI,
audio, input and lifecycle on hardware. It is now a bootstrap/reference path,
not the architecture for broad compatibility.

## Why this differs from Winlator and Box86/Box64

Winlator combines Wine with Box64/Box86 because its usual Android host is Arm
and Windows applications are x86 or x86-64. PS5 and Windows games share the
x86 family, but FW 12.02 does not expose a usable 32-bit compatibility-mode
route to this title. Therefore:

- PE32/i386 code uses the prospero-win IA-32 to x86-64 DBT;
- PE64/AMD64 code can execute natively once the Windows/SysV ABI bridge,
  exception boundary and module loader are complete;
- both use the same Wine PE modules and native PS5 service layer;
- CPU translation is never mixed into DXVK or `ps5-vulkan`.

This avoids an unnecessary x86-64-to-x86-64 DBT for modern titles while
retaining one Windows subsystem for both architectures.

## Boundaries beyond the native Wine bootstrap

The pinned Wine build proves that the selected revision, PE modules and runtime
data can be reproduced. It does not produce a desktop-host Wine binary that can
simply be loaded by a PS5 title:

- the i386 Wine PE modules and PE32 applications still execute through the DBT;
- Wine's Unix-facing code expects host facilities that must be implemented by
  PS5-native adapters rather than Linux or desktop FreeBSD libraries;
- shared x86-64 instructions do not imply a shared ABI: PE64 still needs
  Windows/SysV calling-convention, callback, exception, unwind and context
  bridges; and
- the native title must own startup, manifest selection, prefix mounting,
  telemetry and cleanup around Wine's loader lifecycle.

The host gate has joined the real PE32 runtime graph to the DBT and reached an
application entry point. An opt-in `native/wine_main.c` bootstrap wires that
gate to the PS5 file, registry-seed and object services. It has now launched on
the console with the generated profile fixture: the log records three loaded
images (`app.exe`, `ntdll.dll` and `kernelbase.dll`), 598,649 retired guest
instructions, 2,981 translated blocks and an explicit DBT-observed transfer to
the fixture's entrypoint, followed by process termination with guest status
1. The title closed normally and `ps5log/1` reported a clean BYE, no sequence
gaps and zero cleanup failures. The runner reports `status=unsupported` for
the process-terminated gate stop; this validates the generated fixture's path,
not generic app compatibility.

## Wine reuse boundary

The scalable unit of reuse is a Wine module and its tests, not an isolated C
function copied into a title-specific dispatcher. The intended PE32 startup is:

1. map the application and application-local DLLs;
2. map the selected i386 Wine PE runtime from a separate runtime namespace;
3. resolve exports, forwards, TLS and loader callbacks across the complete
   graph;
4. start at Wine `ntdll` initialization under the DBT;
5. intercept only the defined Wine Unix-call/syscall boundary;
6. execute Windows behavior in Wine and platform behavior in native PS5 code.

The loader now has an explicit `PW_MODULE_RUNTIME` origin and
`PW_FILE_RUNTIME` namespace. `pw_loader_wine_policy` selects known Windows
modules from that namespace. Both host and PS5 providers accept an independently
configured runtime directory; an unconfigured runtime fails as unsupported and
never falls back silently to the application directory. The i386 runtime is
now built and staged reproducibly. A bounded host gate maps it, binds the real
module graph, constructs the PE32 TLS and minimal process environment, and
executes real `ntdll` initialization under the DBT. Both Wine dispatcher
boundaries are now published and checked against the pinned source. A generated
PE32 executable, two local DLLs and their dependency diamond load through that
runtime. Every chaining/residency combination reaches the application's own
entry point, returns `1` and exits through `NtTerminateThread` after the same
598,404 retired instructions and 2,981 translated blocks. Loader-list and
attach-order validation remain. The current verified boundary and measurements
are summarized in [technical details](TECHNICAL_DETAILS.md). Runtime staging
into a native package is implemented, and a bounded generated-fixture
bootstrap has been validated on PS5. User-installed applications remain a
separate compatibility and copy-and-run acceptance gate.

The native bootstrap can be cross-built with the generated PE32 application
fixture and a validated runtime:

```sh
PW_NATIVE_MODE=wine PW_SAMPLE=1 \
  PW_WINE_RUNTIME_DIR=/path/to/wine-runtime \
  PW_OUTPUT_SUFFIX=-wine-bootstrap \
  bash tools/build_native.sh
```

The isolated output suffix keeps the ordinary `build/native` and
`dist/PPSA99995` artifacts untouched. The package places the fixture under
`/app0/win/app`, the Wine DLLs under `/app0/win/runtime/lib/i386-windows`, and
NLS data under `/app0/win/runtime/nls`. The entry starts at
`ntdll!LdrInitializeThunk` through the IA-32 DBT and records module identity,
dispatch counts and cleanup through `ps5log/1`. Its seed registry/object
services are process-local bootstrap defaults, not persistent Wine prefix
hives. Wine mode requires `PW_APP_PROFILE` for user applications; with
`PW_SAMPLE=1`, the builder selects the checked-in fixture profile automatically.
It copies the profile into `/app0/win/app/app.profile`; startup validates it,
selects the executable by staged basename and supplies its image path, current
directory, command line and application-first DLL search path to ntdll. The
current Wine bootstrap accepts PE32 + GDI only. The profile's prefix and runtime
identifiers are still metadata: they do not yet select persistent prefix hives
or multiple Wine builds. The generated-profile host gate reaches normal process
termination across all four DBT configurations (598,430 retired instructions).
The console smoke is separate hardware evidence for the staged profile,
Wine initialization and generated application's entrypoint. Persistent prefix
state, loader-list and TLS attach-order validation, and a user-supplied
copy-and-run workflow remain open gates.

The manifest-driven staging route was also exercised with `PW_STAGE_INPUT`
pointing to an external temporary directory containing a generated PE32
`app.exe` and two DLLs. The builder copied and preflighted that tree with the
profile and pinned Wine runtime; the host gate then consumed the staged package
and reached the app entrypoint before its expected fixture exit. The app image
hash matches the PE32 image recorded by the PS5 smoke. This validates the
fixture's copy-and-run path, not a user-installed application, installer-created
registry state or persistent-prefix behavior.

DXVK DLLs use the same runtime-distribution mechanism. Per-application DLL
overrides will be an explicit policy entry, not an accidental filename search
order.

## Wine WoW64 with the DBT as its i386 CPU

Wine already separates "which CPU runs i386 code" from everything else: its
`wow64.dll` loads an i386 CPU backend named by
`HKLM\Software\Microsoft\Wow64\x86` through the `BTCpu*` contract that
`wow64cpu.dll`, `xtajit.dll` and third-party emulators implement. The
`wine/wowprospero` module is that backend for prospero-win:

```text
PE32 application + Wine i386 PE modules          (guest, IA-32)
  -> prospero-win DBT, host-exec fallback         (wowprospero.so)
     -> syscall / Unix-call BOP
        -> wow64.dll + wow64win.dll thunks        (Wine, x86_64 PE)
           -> ntdll.so, win32u.so, wineserver     (Wine, native)
```

The PE side keeps the canonical `I386_CONTEXT` where Wine expects it
(`TlsSlots[WOW64_TLS_CPURESERVED]`) and services the two BOP addresses exactly
as `wow64cpu`'s thunks do. The Unix side owns one engine per host thread.
Every Windows service - NT calls, USER/GDI (`win32u` with its DIB engine),
the object server - is Wine's own, so a compatibility gap is an
instruction-coverage gap, not a missing `NtUser*`/`NtGdi*` reimplementation.

Instruction coverage is handled as a finite, measurable problem rather than a
per-application frontier:

- `pw_x86_hostexec` executes a non-control, non-stack instruction the
  translator does not cover by rewriting it for the x86-64 host (0x67 prefix,
  effective address in ESI/EDI including the FS base, no ESP register
  operand) inside a stub that loads and stores the guest GPRs, flags, x87,
  SSE and MXCSR state.
- `tools/dbt_differential` compares each encoding as a translated block with
  host execution from randomized state. Over the 203,843 distinct encodings in
  the Wine i386 modules Pinball loads, the 92,536 both paths accept match; the
  run found and fixed a 16-bit `ALU r16, m16` result drop, an `FST ST(i)`
  false stack fault and missing x87 NaN propagation.
  `tests/fixtures/dbt_differential_forms.txt` keeps those as a `make test`
  gate. The [Box86 opcode catalog](BOX86_OPCODE_CATALOG.md) is the
  checklist for forms these images do not yet exercise.

Host reproduction, from a pinned WoW64 Wine tree
(`tools/build_wine_runtime.sh` configures one under `.deps/wine`) and the
staged application tree described by its profile:

```sh
tools/build_wowprospero.sh
tools/run_wine_dbt_host.sh --profile examples/profiles/pinball.profile \
    --stage /path/to/staged/pinball --screenshot pinball.png
tools/run_wine_dbt_host.sh ... --cpu native   # identical control run
```

On a Linux host the unmodified Pinball executable initialises, creates its
window, renders the table and starts a game with all of its i386 code and
Wine's executed by prospero-win; the window matches the `wow64cpu` control
run except for animated lights. This is host evidence only.

The platform facts this port depends on have been measured on the console
(firmware 12.02) by the gate-mode probe `native/pw_wine_platform_ps5.c`, whose
`PW_WINE_PLATFORM` lines are recorded through `ps5log/1`:

| Requirement | Measured |
| --- | --- |
| Per-thread GS base for the x86_64 TEB (`sysarch(AMD64_SET_GSBASE)`) | allowed; `%gs:0x30` reads the installed TEB and a second thread keeps its own base |
| Recoverable SIGSEGV on an alternate stack | delivered on the `sigaltstack`; recoverable |
| Editing the saved RIP in the signal context | works at `ucontext` offset 224; the SDK header's `mc_rip` (offset 176) is not the live slot, so Wine's signal-context accessors need a PS5 layout |
| Signal `ucontext` layout (`native/pw_ucontext_map_ps5.c`, every GPR located by sentinel) | the FreeBSD amd64 `mcontext_t`, unchanged, at `ucontext` offset 64 instead of the header's 16; a non-canonical load arrives as SIGBUS |
| Host page size and low address space (`native/pw_vmspace_ps5.c`) | 16 KiB pages; 64 KiB-aligned fixed reservations are honoured, 4 KiB-aligned ones are not; every 16 MiB slot from 16 MiB to 4 GiB is free at title start, but the whole range cannot be reserved in one mapping |
| User-mode FSGSBASE (`rdfsbase`/`rdgsbase`/`wrgsbase`) | disabled (SIGILL): segment bases change only through `sysarch`, so the port keeps GS = TEB per thread and never switches FS on a syscall transition |
| `socketpair` with `SCM_RIGHTS`, `kqueue`/`kevent` | work (in-process wineserver transport) |
| Title libc `malloc` | about 13 MiB; Wine's native side needs its own `mmap`-backed allocator |

A console run is not yet meaningful for this path. Its runtime contract still
needs, in this order: native execution of Wine's x86_64 PE modules (`ntdll`,
`wow64`, `wow64win`, `wowprospero`), which is the PE64 ABI work; Wine's Unix
side (`ntdll.so`, `win32u.so`) and an in-process `wineserver` on the PS5
platform adapters, since titles cannot fork or exec; a user driver that hands
`win32u` window surfaces to `pw_present`. (The backend's page-readability
check already uses Wine's own `NtQueryVirtualMemory`, with a per-thread cache
of the last readable region, so it needs no host-specific kernel call.)
Until then the direct and bootstrap paths below remain the console evidence.

## Native boundary

Wine's Unix side assumes facilities that must be deliberately adapted to PS5:

- address-space reservation, commit, protection and instruction-cache
  publication;
- native threads, condition variables, monotonic clocks and thread teardown;
- an object/handle service for events, mutexes, semaphores, files, processes
  and waits;
- DOS/NT path translation confined to title-owned storage;
- registry persistence, Unicode/NLS data and environment construction;
- exception/context delivery and PE32 SEH;
- sockets, audio, input and controller integration;
- dynamic-module and Unix-call lookup without depending on an unverified host
  `dlopen` contract.

The first implementation may embed the object service in the title process.
Its API must preserve Wine/NT object semantics so a separate wineserver-style
transport can be introduced later without changing PE modules or guest ABI.
Multi-process compatibility is not claimed by the embedded service.

## Copy-and-run application model

The first distributable workflow does not require a Windows desktop. A shared
Wine runtime is packaged with prospero-win, while each user-supplied application
has two isolated inputs:

```text
application/<id>/
  manifest                 exact EXE, cwd, arguments, architecture and prefix
  files/                   copied installed application directory

prefix/<id>/
  drive_c/                 Windows-visible files and user directories
  system.reg               machine registry state
  user.reg                 per-user registry state
  userdef.reg              default user registry state
```

The paths are a logical contract; the final title-storage layout remains a
platform-adapter decision. The manifest must never silently search arbitrary
executables or inherit DLL overrides from another application.

`PwPrefixService` now defines the persistent path layout below a configured
title-owned storage root. It creates the prefix, `drive_c`, Windows system
directories, Program Files, the default user tree and temp directory through
an injected recursive directory adapter. It also resolves Wine 11.17's three
registry text-file paths: `system.reg`, `user.reg` and `userdef.reg` (there is
no separate `classes.reg` in this pinned prefix contract). The operation is
idempotent and may be retried after partial
directory creation. It does not use host filesystem calls or claim that Wine's
registry hives are already loaded or persisted; those file operations remain
the responsibility of the PS5 storage adapter and Wine registry integration.
The native `pw_prefix_ps5` adapter implements recursive, idempotent directory
creation with `sceKernelStat` and `sceKernelMkdir`; it exposes
`/download0/prospero-win` as `PW_PREFIX_PS5_DEFAULT_ROOT`, which yields
`/download0/prospero-win/prefixes/<id>`. The adapter is host-tested through a
POSIX test shim. Profile-mode `runtime_main` creates that tree and stores its
current emulator registry snapshot as `registry.pwrg` under the prefix. This
is not a Wine registry hive and is not read by the native Wine bootstrap.

`PwRuntimeSupervisor` joins a parsed profile to its prefix and defines the
single-guest states `IDLE → PREPARING → RUNNING → STOPPING → CLEANUP → IDLE`.
Guest exit and launch failure both enter cleanup; a failed cleanup retains the
active ownership data for retry, and a successful cleanup preserves the final
result for the shell. Profile-mode `runtime_main` now drives this lifecycle
around its single direct PE32/GDI guest and records the result in teardown
telemetry. This is not yet generic Wine process startup or multi-process
supervision; the legacy no-profile runner path remains unchanged.

The initial host-side `PwAppProfile` contract uses a bounded INI manifest:

```ini
[application]
id = space-cadet-pinball
name = Space Cadet Pinball
executable = C:\Games\Pinball\PINBALL.EXE
working_directory = C:\Games\Pinball
arguments =
startup_command_id = 101
prefix = pinball
runtime = prospero-win-direct
architecture = pe32
graphics = gdi
```

The parser rejects unknown or duplicate fields, missing required fields,
relative or traversal paths, invalid identifiers and unsupported enum values.
`arguments` is optional. `pe64` and `dxvk` can be represented in a profile
before their runtime paths are available; the native profile boot path rejects
them until those backends exist. Parsing is allocation-free and is covered by
host tests. The `startup_command_id` field is an optional decimal `WM_COMMAND`
ID queued once when the first window enters the runtime message wait; it
defaults to zero (no command).
The Pinball example requests ID 101 to start a game, while Paint does not.
Without profile mode the legacy Pinball runner retains its ID 101 behavior.

Ready-to-edit examples live in `examples/profiles/pinball.profile` and
`examples/profiles/paint.profile`; `test_pw_app_profile` parses those exact
files through the production parser. They describe user-supplied executable
paths only. The direct runtime identifier is metadata, not Wine startup, and
the Paint profile does not assert that the current Win32 surface can run every
Paint build. Prefix contents come from user-managed files, and Paint requires
a legally obtained compatible `mspaint.exe`.

The native runner can now be built with `PW_APP_PROFILE=/path/to/app.profile`.
The manifest is copied to `/app0/win/app.profile`, parsed with the bounded
profile contract at startup, and selects its executable by staged basename.
This boot path currently accepts only PE32 + GDI profiles; the runtime and
prefix identifiers are reported as metadata but do not yet select Wine builds
or separate Wine builds. `working_directory` is exposed through
`GetCurrentDirectoryA`. In profile mode, read-only file callbacks accept a
relative path or a full DOS path within that working directory and resolve it
under the recursive, case-folded `/app0/win/app/` package tree. Absolute paths
outside the profile directory, traversal components, invalid Windows names,
symlinks and case-fold collisions are rejected. The executable must currently
also be present at the top level of `PW_STAGE_INPUT`; it remains staged at the
package root for PE loading. Writes to app files, `SetCurrentDirectoryA` and a
general writable prefix filesystem remain future work. With `PW_APP_PROFILE`
unset, the existing `PW_ROOT_MODULE` and legacy path handling remain unchanged.

The native package builder can also receive
`PW_WINE_RUNTIME_DIR=/path/to/wine-runtime`. It checks the runtime manifest
against the pinned Wine commit and module/data hashes, then copies only the
manifest-listed PE modules and NLS files into `/app0/win/runtime/`, preserving
the distribution's `lib/i386-windows/` and `nls/` layout. The `runtime`
package mode still launches the direct PE32/GDI runner through `runtime_main`
and does not select these Wine modules. The separate `wine` package mode
selects its profile and enters `LdrInitializeThunk`; its profile selection and
process-parameter path has host evidence, while PS5 execution has only been
validated for the generated profile fixture, not user-installed applications.

The intended launcher selects the manifest's EXE directly for portable
applications.
Applications that depend on installer-created registry state can use reviewed
compatibility recipes first. Direct `setup.exe` execution follows once process,
filesystem and persistence behavior is sufficient; MSI and complex child
process trees follow after that. Explorer may later provide a familiar shell,
but is not required to launch an EXE. Store clients are a substantially later
multi-process integration target and are not a prerequisite for game
compatibility.

### Planning the Wine runtime for a PE32 application

`tools/wine_runtime_modules.py <application.exe> --wine-source <pinned-checkout>`
uses the canonical PE parser to inventory the application's static imports,
then follows Wine's `dlls/<module>/Makefile.in` `IMPORTS` fields at the pinned
Wine revision to produce a reproducible PE-DLL dependency closure. Wine import
libraries are reported separately from runtime DLLs; unknown dependencies fail
closed. The report records the application hash and Wine commit, and explicitly
does not claim coverage of delay-loaded or dynamic imports. This is a planning
artifact: a module being present in the closure does not prove its exports are
implemented or that the application can run through Wine.

This is the same practical separation used by other constrained Wine ports:
application files may be copied from an existing installation while registry
and Windows environment state live in a persistent prefix. The design does not
assume that copying a directory also copies installer registry state.

## Title sandbox and guest boundary

PS5 packages already execute in a title-scoped process and filesystem context.
prospero-win benefits from that outer isolation without reproducing the Android
`proot` layer used by projects such as Winlator. It does not, however, gain a
general facility for creating one kernel jail per Windows program. A single
prospero-win title hosting several programs would place them in the same outer
sandbox.

The reusable runtime must consequently enforce its own guest boundary at the
interfaces it controls: validated guest spans, W^X translated-code
publication, typed non-reissued handles, canonical path namespaces, bounded
resource ownership, and capability-limited Unix-call services. Native PS5
debugging or deployment facilities stay outside that surface. PE32 gets this
mediation naturally through the DBT; future native PE64 execution shares the
host address space and requires a separate trust or containment decision.

## Performance architecture

The DBT stays single-owner per guest thread. Multiple Windows threads receive
independent `PwX86State`, TEB/FS state and dispatch ownership and may run on
native threads. Shared translated code is immutable after W^X publication;
cache metadata and invalidation need explicit synchronization before it can be
shared across engines.

Performance changes are driven by Wine and independent application traces,
not by title-specific shortcuts. The runtime keeps translated code immutable
after publication, maintains per-thread execution state, and records dispatch,
cache, publication and frame-pacing counters. Instruction-family expansion,
branch prediction and wider register allocation must preserve precise
safepoints and the existing differential CPU-state tests. No native-performance
percentage is claimed without an exact control comparison on PS5.

## Graphics boundary

prospero-win will not grow a second native D3D9 renderer. The graphics path is
DXVK over `ps5-vulkan`. Until that backend satisfies the pinned DXVK consumer
profile, work here is limited to the Windows/DXGI-facing loader, object,
threading and presentation contracts that do not fabricate Vulkan support.

The current `ps5-vulkan` implementation already demonstrates native gfx1013
graphics and compute, but its public documentation still identifies missing
DXVK requirements including Vulkan version/features, broad descriptors and
formats, WSI/swapchain behavior and other graphics state. An unmodified DXVK
start is therefore not yet a current compatibility claim.

## Provenance and licensing

The current Wine reference is commit
`490f6d5dcbb2a5047345b8af88d114bbcaad69a8`. Wine-derived code remains
LGPL-2.1-or-later with original notices and per-file provenance. Generated Wine
PE binaries, DXVK binaries and private Windows applications are build inputs or
artifacts; they are not committed here.

The machine-readable [support matrix](SUPPORT_MATRIX.json) reports the current
state of each runtime layer as `validated`, `implemented`, `partial`,
`unsupported` or `external`. It deliberately contains no internal exit
criteria or delivery schedule.
