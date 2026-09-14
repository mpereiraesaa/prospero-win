# Architecture

The target has four layers with a fixed dependency direction. Portable code
does not call PS5 APIs directly; native adapters do not implement Windows API
semantics. Wine owns the Windows subsystem, while prospero-win owns CPU
execution and the platform boundary.

```text
Windows application and application-local PE modules
  -> Wine PE runtime (ntdll, kernelbase, user32, gdi32, ...)
     -> prospero-win DBT / ABI bridge, Unix-call and NT service boundary
        -> native PS5 adapters
           -> PS5 title sandbox and kernel services

DXVK PE modules -> ps5-vulkan -> AGC / VideoOut / gfx1013

include/           portable contracts a host implements
  prospero_win.h      result codes, protection flags, limits
  prospero_win_vm.h   reserve / commit / protect / release, two aliases
  prospero_win_file.h namespace + canonical module -> read-only byte span

src/               the portable loader core
  pe_image      DOS/NT headers, directories, section table   (read only)
  pe_layout     reservation, copy/zero spans, page protection
  pe_reloc      base relocation against the executing address
  pe_import     import descriptors: which modules, which symbols
  pw_module_name canonical names and bootstrap system-module classification
  pw_map        reserve, copy, zero, relocate, verify, protect
  pw_loader     recursive graph, module-origin policy and load order
  pw_gate       the structured report one load produces
  pw_registry   fixed-capacity guest keys, values and opaque handles
  pw_x86_block  bounded x86-to-x86-64 translation
  pw_x86_cache  mapping-generation-scoped translated-block lifecycle
  pw_x86_engine source-span translation, cache dispatch and RW-to-RX publication
  pw_x87        integer-only binary80 execution over isolated guest FP state
  pw_user32     process-owned message/window namespace and common-control state
  pw_gdi        process-owned DC/bitmap/surface namespace and software composition
  pw_crt_format allocation-free guest varargs formatting subset
  pw_guest_fp   isolated x87/SSE control and raw 80-bit stack state
  pw_vm_posix   anonymous-mapping backend (host and console)
  pw_file_posix directory backend (host tools and tests only)

native/            the PS5 adapter
  main.c        ps5log init, pre-flight, gate run, teardown
  pw_file_ps5   sceKernelOpen/Close/Stat plus read/lseek, mmap'd buffers
  pw_audio_ps5  bounded PCM queue and the sole SceAudioOut worker
  ps5log/       vendored `ps5log/1` client, pinned by digest
```

The direct `pw_win32.c` import surface made the first playable target possible
and remains a useful reference harness. It is not the broad-compatibility
architecture: new applications should converge on Wine PE DLLs plus the
defined Unix-call/platform boundary instead of expanding title-specific
wrappers. See [WINE_INTEGRATION.md](WINE_INTEGRATION.md).

The real Wine gate is now an executable checkpoint rather than a loader-only
claim. The syscall and Unix-call boundaries are both published. A generated
PE32 application and two local DLLs load through the pinned i386 Wine runtime;
all four chaining/residency configurations reach its own entry point, return
`1`, and exit through `NtTerminateThread` after exactly 598,404 retired guest
instructions and 2,981 translated blocks. This is host evidence. Wine's loader
lists and attach ordering are not independently validated, and no Wine process
boot on PS5 is claimed.

## Isolation boundaries

The packaged homebrew title supplies the outer process and filesystem
boundary. `/app0` is the immutable application image and title-owned writable
storage is exposed through explicit platform adapters. This is materially
useful isolation, but it should not be described as Linux-style container
creation or nested jails: if one title hosts several Windows applications,
they share that title's outer sandbox.

prospero-win therefore enforces a second, logical guest boundary:

- every guest pointer and range is validated before a native service uses it;
- translated code is writable only while being built and executable only
  after publication;
- typed handles are monotonic and do not expose native descriptors or
  pointers;
- DOS/NT paths are canonicalized into explicit application, runtime and
  title-storage namespaces with traversal rejected;
- quotas and capabilities belong at the Unix-call and platform-service
  boundary; and
- deployment facilities such as debuggers, mount refreshers and telemetry
  control are never guest-visible Windows capabilities.

PE32 execution passes through the DBT and therefore offers a natural point for
memory, instruction and syscall mediation. Future PE64 execution can run host
x86-64 instructions directly and shares the title process address space, so it
must be limited to trusted inputs until a stronger isolation design is proven.
Native speed is not itself an isolation boundary.

## Why the core imports almost nothing

`src/` includes only `<stddef.h>`, `<stdint.h>` and `<string.h>`. It has no
allocator, no formatter and no case-folding helper from the platform.

That is a direct response to the laboratory's porting playbook: on this
firmware a system library exports many symbols that are placeholders or
subtly wrong, and treating "provided by `libSceLibcInternal`" as a green
check has already cost a full session once. The cheapest way to not inherit
that class of bug is to not import the symbol. So:

- module names are compared with an in-tree ASCII fold, not `strcasecmp`,
  which is also the locale-correct choice for names that are ASCII by spec;
- the gate formats its own records with bounded integer and hex helpers
  rather than `snprintf`;
- memory comes from the injected `PwVmBackend`, never from `malloc`.

`tests/test_native_contract.py` enforces this: it fails if a core source
includes an unexpected header or references a forbidden symbol, and it fails
if `tools/build_native.sh` stops compiling a core source, so a new module
cannot be silently left out of the title.

## The two memory aliases

`PwVmRegion` carries a `write_base` and an `exec_base`. On a POSIX host they
are the same pointer. On the console, publishing executable pages goes
through a second mapping of the same memory, so they differ.

The rule that follows is the important one: **a relocation delta is computed
against `exec_base`, and the patched bytes are written through
`write_base`.` `pw_map_image()` does this in one place, and
`pw_map_verify()` compares the two aliases over every executable section so
a mismatch is caught as itself rather than as a wild jump later.

## Order of operations, and why it is fixed

```text
reserve(image_bytes, section_alignment)
  -> commit everything read-write
  -> copy headers, copy each section's raw bytes
  -> zero the uninitialised remainder explicitly
  -> apply base relocations
  -> verify against the file
  -> install final page protections
```

Protections come last because the read-write window is where relocation and,
later, import binding happen: installing a read-only `.text` first would
fault on the first thunk write. Verification comes before protections
because afterwards some pages are deliberately unreadable;
`pw_map_verify()` returns `PW_ERR_STATE` if called too late, so the
ordering is a contract rather than a convention.

The explicit zero fill is not redundant with a fresh anonymous mapping. A
recycled reservation would otherwise leak previous contents into a `.bss`
that the program is entitled to see as zero.

## Application, runtime and host module origins

Every import is classified by canonical name and explicit policy:

- **application** modules are the executable and application-local DLLs;
- **runtime** modules are selected Wine or DXVK PE files from a separate,
  pinned runtime distribution;
- **host** modules are native interfaces, registered but never opened or
  mapped.

The default policy preserves the direct-wrapper bootstrap: known system names
are host modules and other DLLs are local. `pw_loader_wine_policy` instead
maps known Windows modules from `PW_FILE_RUNTIME`. The provider must implement
namespace-aware lookup; runtime lookup fails as unsupported if it cannot, and
never falls back silently to the application's directory. Application-local
overrides and API-set resolution will be explicit policy rules with fixtures,
not ambient filename search order.

## Bounds, cycles and failure

Every capacity is compiled in — 96 sections, 64 import descriptors, 32
modules, 16 levels of depth — and every overflow fails closed. A corrupt or
hostile import table cannot make the loader allocate or recurse without
bound.

Import cycles are normal in PE (`kernel32` and `ntdll` import each other), so
they are not errors: a module is registered once under its canonical name,
a repeat edge only adds a link, and the depth-first ordering counts back
edges into `cycle_edges` and keeps going. What *is* an error: a dependency
that cannot be found, a graph mixing instruction sets, a rebase with no
relocation table, an unknown relocation type, and a verification mismatch.

A failed load releases every reservation it had already made and closes every
span it had opened, so `PW_EXIT` on a failing run is as trustworthy as on a
passing one.

## Translated block ownership

`PwX86Engine` owns one executable arena and borrows both immutable executable
source spans and fixed-capacity cache metadata. A mapping generation identifies
the lifetime of source bytes. Compilation takes the maximal valid prefix up to
the explicit block limits, emits while the arena is writable, publishes the
entry only after copying, and seals the complete arena read/execute before
dispatch. A protection failure poisons the engine; reset invalidates every
entry and changes the generation before translation resumes.

Blocks retain every guest instruction end offset. If a translated memory guard
fails midway, the dispatcher reports only the exact retired prefix. Cache and
retirement metrics are part of the runner's structured host evidence; they are
not a throughput claim and do not imply PS5 execution.

Hot block lookup uses an open-addressed table keyed by guest PC. A normal
dispatch therefore probes one or a small number of entries instead of scanning
all 8,192 slots. Publishing a new translation changes W^X protection only for
the 16 KiB page or pages touched by that block; it no longer toggles the entire
4 MiB arena on every cold edge. The guest dispatcher remains single-owner.
Audio, presentation and future I/O workers may run concurrently, but two host
threads must never execute or mutate the same `PwX86State`.

## Asynchronous audio ownership

WinMM and SceAudioOut are separated by a bounded producer/consumer contract.
`waveOutWrite` copies and converts the guest buffer into an owned queue, marks
its `WAVEHDR` `WHDR_INQUEUE` and returns without waiting for playback. A
dedicated worker is the only thread that calls `sceAudioOutOutput`. When the
final 256-frame block has actually been consumed, it posts a completion token;
the guest thread drains those tokens, changes the header to `WHDR_DONE` and
posts `WOM_DONE`.

This ownership rule applies to every WinMM consumer, not to a title name.
Pause, restart, reset, queue exhaustion, worker failure and teardown are
explicit states. Guest memory and User32 remain guest-thread-owned; the audio
thread sees only copied signed-16-bit stereo blocks. The fixed 1,024-block
queue holds about 5.46 seconds at 48 kHz and fails closed rather than blocking
the DBT when full.
