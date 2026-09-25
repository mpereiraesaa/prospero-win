# Technical details

This document is the concise public description of the runtime as implemented.
It records verified architecture and current limitations, not an internal task
queue or delivery schedule.

## Runtime layers

prospero-win separates Windows behavior from PS5 platform behavior:

1. The PE loader maps application, Wine and future DXVK modules.
2. PE32 instructions execute through the IA-32 DBT. Future PE64 instructions
   can execute natively after their ABI boundary is complete.
3. Wine supplies Windows subsystem behavior through PE modules.
4. prospero-win implements the defined Wine syscall/Unix-call and platform
   service boundary.
5. Native adapters own PS5 VM, files, threads, telemetry, Pad, AudioOut,
   VideoOut and AGC resources.

The current playable Pinball path predates the full Wine integration and binds
a reviewed Win32 subset directly. It remains a hardware regression target, but
new application support is intended to pass through Wine.

## PE loading

The loader supports PE32 and PE32+ parsing, section layout, base relocations,
normal imports, exports by name and ordinal, bounded forwarded exports, module
cycles and PE32 TLS metadata. Application modules and runtime modules live in
separate namespaces; a missing runtime file cannot silently fall back to an
application-local file.

Mappings are created writable, populated and relocated, verified, and only
then assigned their final protections. Executable DBT output follows the same
W^X rule: emission and publication are separate states. Every section, module,
depth and allocation capacity is bounded, and partial loads release acquired
resources on failure.

Delay imports, broader API-set policy and complete Wine loader-list and
`DllMain`/TLS ordering remain outside the validated profile.

## PE32 execution

Firmware 12.02 does not expose a usable 32-bit compatibility-mode path to the
title, so PE32 code uses an IA-32-to-x86-64 dynamic binary translator. The
engine provides:

- a mapping-generation-scoped translated-block cache;
- hashed block lookup and direct block chaining;
- cross-block guest-register residency with explicit reconciliation;
- lazy arithmetic flags with precise helper and safepoint materialization;
- isolated integer, x87, SSE-control and FS/TEB state; and
- bounded RW-to-RX code publication.

Translation remains single-owner per guest thread. Native audio and platform
workers may run concurrently, but they do not mutate guest CPU state. Shared
immutable translated code and general multi-threaded Wine execution are not yet
claimed.

## PE64 execution

The PS5 and 64-bit Windows applications share the x86-64 instruction set, but
not the complete runtime ABI. Windows x64 calls use different argument,
callback and unwind conventions from the native title. A synthetic integer
call bridge exists; native PE64 applications still require complete import
thunks, callbacks, FP/vector arguments, exception/context delivery and loader
lifecycle support.

Consequently, PE64 is expected to avoid CPU translation, but is not currently
an executable compatibility target.

## Wine integration

The pinned Wine reference produces a manifest-validated i386 runtime containing
`ntdll`, `kernelbase`, `kernel32` and the revision's 76 tracked NLS files. The
host integration gate:

- maps application and runtime namespaces independently;
- constructs the initial PE32 PEB, TEB, process parameters, stack and context;
- publishes Wine's syscall and Unix-call dispatcher slots;
- validates the 256-entry i386 syscall table and eight-entry Unix-call table
  against the pinned Wine source; and
- services 32 NT call shapes across VM, files, sections, registry, NLS,
  process/thread queries and the object namespace.

A generated PE32 application and two DLLs load through Wine's own `ntdll`,
reach the application entry point, return `1` and exit through
`NtTerminateThread`. The four chaining/residency combinations retire the same
598,404 guest instructions over 2,981 translated blocks and return the same
status.

That result is host integration evidence. Separately, the opt-in PS5 bootstrap
has launched the profile-selected generated fixture through the platform file
adapter and process-local registry/object seed services. The hardware log
records `app.exe`, `ntdll.dll` and `kernelbase.dll`, 598,649 retired guest
instructions, 2,981 blocks, and an explicit DBT-observed transfer to the
fixture's entrypoint before process termination with guest status 1. Its
`ps5log/1` manifest is clean (eight records, no gaps, clean BYE); the transcript
SHA-256 is
`5af8bdd68f3afe7461742d6cf26ef741c95fb4c9cb7bcfed9ba9929a009dafd5`. The gate
reports `status=unsupported` because process termination is not currently an
accepted stop. This validates the generated profile through its entrypoint and
exit, not compatibility with arbitrary Windows programs. Persistent prefix
hives, broader objects and waits, thread creation and loader attach-order
evidence remain incomplete.

The bootstrap build uses `PW_NATIVE_MODE=wine PW_SAMPLE=1` with a validated
`PW_WINE_RUNTIME_DIR`. `PW_OUTPUT_SUFFIX` isolates its build and package from
the normal Pinball outputs. Its generated PE32 fixture is staged in
`/app0/win/app`; the pinned Wine modules and NLS data remain in separate runtime
directories. Startup and teardown have now been observed on hardware through
`ps5log/1`; the run does not validate persistent prefix behavior or a copied,
user-installed application.

## Application and prefix model

The intended generic workflow separates three things:

- a shared, versioned Wine runtime;
- a user-supplied installed application directory plus a manifest naming its
  EXE, working directory, arguments, architecture and DLL policy; and
- an isolated persistent prefix containing `drive_c`, Windows directories,
  Wine's `system.reg`, `user.reg` and `userdef.reg` stores, environment and
  per-application state.

Copying an installed folder does not recreate registry values written by an
installer. Portable applications can run directly; applications with setup
state need an explicit compatibility recipe or, later, direct installer
execution. Explorer is optional UI rather than a runtime dependency.

## Graphics, audio and input

The current Win32 bootstrap composes GDI surfaces in software and presents the
result through native AGC and VideoOut. Direct3D is not implemented by these
wrappers: the selected path is DXVK over the companion `ps5-vulkan` project,
which owns Vulkan-to-AGC translation and gfx1013 GPU behavior.

WinMM/WaveMix buffers are copied into a bounded native queue. One AudioOut
worker owns `sceAudioOutOutput`; completion returns to the guest thread before
it updates Windows-visible buffer state. DualSense samples are translated into
the Win32 message path by the native Pad adapter. These ownership rules are
reusable platform contracts, even though their current hardware acceptance
comes from the first playable target.

## Evidence boundary

The public repository contains source, generated fixtures and aggregate
evidence only. A hardware claim requires an exact artifact identity, matching
deployment, structured `ps5log/1` records, subsystem ownership evidence and a
classified close. Screenshots and video supplement those records but do not
replace them.

The repository does not contain Windows executables, vendor DLLs, proprietary
shaders, SDK material, raw captures or decompiler output. See
[HARDWARE_VALIDATION.md](HARDWARE_VALIDATION.md) and
[DEVELOPMENT.md](DEVELOPMENT.md) for the reproducible gates. The
[support matrix](SUPPORT_MATRIX.json) is the machine-readable summary of the
current component-level claims.
