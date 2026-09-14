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
attach-order validation remain. See [WINE_RUNTIME.md](WINE_RUNTIME.md).
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

The next performance work is driven by Wine plus application traces, not only
Pinball:

1. indirect-branch inline caches and return prediction;
2. profile-guided contracts, direct resident emission and then an intermediate
   representation with wider register allocation;
3. SSE/SSE2 and remaining integer instruction families required by Wine PE32;
4. trace/superblock formation with bounded safepoints;
5. shared immutable translations and per-thread execution state;
6. deterministic PS5-side counters for dispatch, spills, exits, code size and
   frame pacing.

No native-performance percentage is claimed without an exact eager/control
comparison on PS5.

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

The machine-readable foundation ledger is
[WINE_FOUNDATION.json](WINE_FOUNDATION.json). Its status vocabulary is
conservative: `validated`, `implemented`, `partial`, `planned` and
`external-wip` describe evidence, not aspiration.
