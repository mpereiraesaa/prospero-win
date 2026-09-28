# Wine integration architecture

Wine is the Windows subsystem for prospero-win: its own PE modules, its Unix
side and its object server run inside the PS5 title. DXVK is the Direct3D
implementation, and `ps5-vulkan` is its future native graphics backend:

```text
Windows game (PE32 or PE64)
  -> Wine PE DLLs: ntdll, kernelbase, kernel32, user32, gdi32, ...
     -> Wine's Unix side on PS5: ntdll.prx, win32u.prx, wineserver.prx
        -> the title: VideoOut, AudioOut, DualSense, files

  -> DXVK d3d8/d3d9/d3d10/d3d11 + DXGI DLLs
     -> ps5-vulkan
        -> AGC / VideoOut / gfx1013
```

How the Unix side is built and started on the console is in
[WINE_PS5_BUILD.md](WINE_PS5_BUILD.md).

## Why this differs from Winlator and Box86/Box64

Winlator combines Wine with Box64/Box86 because its usual Android host is Arm
and Windows applications are x86 or x86-64. PS5 and Windows games share the
x86 family, but FW 12.02 does not expose a usable 32-bit compatibility-mode
route to this title. Therefore:

- PE32/i386 code uses the prospero-win IA-32 to x86-64 DBT, as Wine's WoW64
  CPU backend;
- PE64/AMD64 code runs natively, through Wine's own x86_64 PE modules and
  their Windows/SysV transitions;
- both use the same Wine Unix side and native PS5 service layer;
- CPU translation is never mixed into DXVK or `ps5-vulkan`.

This avoids an unnecessary x86-64-to-x86-64 DBT for modern titles while
retaining one Windows subsystem for both architectures.

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
Between runs of guest code the guest's x87 and SSE state is the thread's
hardware state, as with `wow64cpu`: `cpu.c` saves it into the context's
FXSAVE image before each run and restores it after, so what `NtContinue`,
`SetThreadContext` and exception dispatch write reaches the guest and
`GetThreadContext` reads the guest's.
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
tools/run_wine_dbt_host.sh --profile <prospero-win-profiles>/profiles/pinball.profile \
    --stage /path/to/staged/pinball --screenshot pinball.png
tools/run_wine_dbt_host.sh ... --cpu native   # identical control run
```

On a Linux host the unmodified Pinball executable initialises, creates its
window, renders the table and starts a game with all of its i386 code and
Wine's executed by prospero-win; the window matches the `wow64cpu` control
run except for animated lights. This is host evidence only.

The platform facts this port depends on were measured on the console
(firmware 12.02) by a probe title, since removed, whose `PW_WINE_PLATFORM`
lines were recorded through `ps5log/1`:

| Requirement | Measured |
| --- | --- |
| Per-thread GS base for the x86_64 TEB (`sysarch(AMD64_SET_GSBASE)`) | allowed; `%gs:0x30` reads the installed TEB and a second thread keeps its own base |
| Recoverable SIGSEGV on an alternate stack | delivered on the `sigaltstack`; recoverable |
| Editing the saved RIP in the signal context | works at `ucontext` offset 224; the SDK header's `mc_rip` (offset 176) is not the live slot, so Wine's signal-context accessors need a PS5 layout |
| Signal `ucontext` layout (every GPR located by sentinel) | the FreeBSD amd64 `mcontext_t`, unchanged, at `ucontext` offset 64 instead of the header's 16; a non-canonical load arrives as SIGBUS |
| Host page size and low address space | 16 KiB pages; 64 KiB-aligned fixed reservations are honoured, 4 KiB-aligned ones are not; every 16 MiB slot from 16 MiB to 4 GiB is free at title start, but the whole range cannot be reserved in one mapping |
| User-mode FSGSBASE (`rdfsbase`/`rdgsbase`/`wrgsbase`) | disabled (SIGILL): segment bases change only through `sysarch`, so the port keeps GS = TEB per thread and never switches FS on a syscall transition |
| `socketpair` with `SCM_RIGHTS`, `kqueue`/`kevent` | work (in-process wineserver transport) |
| Title libc `malloc` | about 13 MiB; Wine's native side needs its own `mmap`-backed allocator |

These facts shaped the execution-core patches (0500–0899): the PS5 signal
context, per-thread GS, the 16 KiB page size and the reserved low range. The
whole path now runs on the console; see
[WINE_PS5_BUILD.md](WINE_PS5_BUILD.md#console-bring-up) and
[hardware validation](HARDWARE_VALIDATION.md).

## Profiles and prefixes

Each game is a profile under `/data/prospero-win/profiles`: its executable,
working directory, prefix, display and input. A prefix is an ordinary Wine
prefix (`drive_c`, `system.reg`, `user.reg`, `userdef.reg`) under
`/data/prospero-win/prefix` or `prefixes/<name>`, so copying an installed
game into its `drive_c` is enough for a portable application. Copying a
folder does not recreate registry values an installer would have written;
those need the installer itself or a reviewed recipe. The format and the
examples are described in
[WINE_PS5_BUILD.md](WINE_PS5_BUILD.md#starting-wine-in-the-title).

## Title sandbox and guest boundary

PS5 packages already execute in a title-scoped process and filesystem context.
prospero-win benefits from that outer isolation without reproducing the Android
`proot` layer used by projects such as Winlator. It does not, however, gain a
general facility for creating one kernel jail per Windows program. A single
prospero-win title hosting several programs would place them in the same outer
sandbox.

Wine's own handle, path and process model applies inside the title. PE32
code passes through the DBT, which writes the code it translates into memory
of its own above the guest's 4 GiB, readable, writable and executable, where
guest code cannot reach it; native PE64
execution shares the host address space and is limited to trusted inputs.
Native PS5 debugging or deployment facilities stay outside the guest's
reach.

## Performance architecture

The DBT stays single-owner per guest thread: `wowprospero` gives every host
thread its own engine, `PwX86State` and cache. Translated code is immutable
after publication, and a memory change discards only the translations made
from the pages it touches. Under `wowprospero` the arena, the fallback stubs
and the cache entries are Wine's own memory above 4 GiB
(`wine/wowprospero/host_memory.h`), committed read-write-execute, so
publishing a block needs no protection change (two per block before, about
26 µs each on the console); on the console that memory is direct memory
(patch 0600). The first guest thread gets a 128 MiB arena and 65536 entries,
later ones a quarter of that (`thread_budget.h`).

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
