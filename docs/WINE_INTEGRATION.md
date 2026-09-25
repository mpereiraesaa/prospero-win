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

## Why a successful Wine build is not yet a PS5 boot

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

The host gate has already joined the real PE32 runtime graph to the DBT and
reached an application's entry point. The next boundary is therefore not a new
Wine build; it is the first equivalent boot through the PS5-native runner and
platform adapters.

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
are summarized in [technical details](TECHNICAL_DETAILS.md).
Staging the runtime inside the title and booting a Wine process remain separate
hardware acceptance gates.

DXVK DLLs use the same runtime-distribution mechanism. Per-application DLL
overrides will be an explicit policy entry, not an accidental filename search
order.

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
  classes.reg              COM/class registrations when required
```

The paths are a logical contract; the final title-storage layout remains a
platform-adapter decision. The manifest must never silently search arbitrary
executables or inherit DLL overrides from another application.

`PwPrefixService` now defines the persistent path layout below a configured
title-owned storage root. It creates the prefix, `drive_c`, Windows system
directories, Program Files, the default user tree and temp directory through
an injected recursive directory adapter. It also resolves the four registry
hive paths. The operation is idempotent and may be retried after partial
directory creation. It does not use host filesystem calls or claim that Wine's
registry hives are already loaded or persisted; those file operations remain
the responsibility of the PS5 storage adapter and Wine registry integration.
The native `pw_prefix_ps5` adapter implements recursive, idempotent directory
creation with `sceKernelStat` and `sceKernelMkdir`; it exposes
`/download0/prospero-win` as `PW_PREFIX_PS5_DEFAULT_ROOT`, which yields
`/download0/prospero-win/prefixes/<id>`. The adapter is host-tested through a
POSIX test shim. Profile-mode `runtime_main` creates that tree and stores its
current emulator registry snapshot as `registry.pwrg` under the prefix. This
is not Wine's four-hive format: the hive paths remain contracts, not
persistent Wine registry stores.

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
the distribution's `lib/i386-windows/` and `nls/` layout. This is package
staging only: `runtime_main` still launches the direct PE32/GDI runner and
does not select or execute those Wine modules. Native Wine startup and
manifest-driven launch through `LdrInitializeThunk` remain separate acceptance
gates.

The intended launcher selects the manifest's EXE directly for portable
applications.
Applications that depend on installer-created registry state can use reviewed
compatibility recipes first. Direct `setup.exe` execution follows once process,
filesystem and persistence behavior is sufficient; MSI and complex child
process trees follow after that. Explorer may later provide a familiar shell,
but is not required to launch an EXE. Store clients are a substantially later
multi-process integration target and are not a prerequisite for game
compatibility.

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
