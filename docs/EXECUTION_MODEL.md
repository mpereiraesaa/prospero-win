# Execution model

PE32/i386 and PE32+/AMD64 applications are in scope. A complete PE32 game runs
through the IA-32 DBT and the bootstrap Win32 surface. Separately, a generated
PE32 application now reaches its own entry point and exits through Wine's
loader in a bounded host gate; this is a Wine bring-up result, not broad
compatibility or a PS5 Wine boot. PE64 mapping and a synthetic integer bridge
exist, but a complete PE64 Windows application does not yet run.

## Execution and containment

The PS5 launches prospero-win as one native title process. The title sandbox is
the outer security and filesystem boundary; prospero-win does not assume
`proot`, arbitrary child containers, unrestricted `execve`, host `dlopen`, or
Linux namespaces. Wine PE modules are mapped into this process and Windows
threads are intended to use native threads with separate guest CPU, TEB/FS and
object state.

PE32 adds an inner mediation layer because every guest instruction and native
service transition passes through the DBT. Guest spans, handles, paths and
executable publication can therefore be checked centrally. PE64 can eventually
execute directly on the x86-64 CPU, but then the PS5 title sandbox is the only
hard process boundary and the Windows/SysV bridge is not a sandbox. Native PE64
inputs need an explicit trust policy or a separately proven containment model.

## Native AMD64 and ABI bridges

AMD64 guest instructions can execute on the PS5 CPU. Crossings into native
code still need an ABI bridge: Windows x64 uses RCX/RDX/R8/R9, 32 bytes of
caller-provided shadow space and different preserved registers from the
native System V convention. Floating arguments, aggregate returns, stack
alignment and callbacks need explicit tests.

Gate 0.2b starts with a mapped constant-return function, then validates
these crossings using explicit ms_abi functions or equivalent thunks.
Compiler acceptance is not hardware proof. No-argument leaf execution alone
does not establish a working Win32 import boundary.

### Host execution tests and PS5 compiler result (2026-09-08)

tests/test_pw_win64.c maps synthetic PE64 code, verifies its bytes, protects
it RX and executes constant-return, entry stack alignment, six weighted
integer arguments (including high 32 bits and all four writable home slots),
four double arguments, guest-to-host and guest-to-host-to-guest callbacks.
The host reference uses ms_abi; bridge functions are noinline so their ABI
crossings cannot disappear through inlining.

The pinned Prospero Clang 18 target rejects ms_abi as unsupported. Native
code therefore uses the explicit SysV-to-Win64 assembly bridge
src/pw_win64_call.S. Its six integer/pointer argument path matches the host
compiler reference, including nested callbacks, and compiles for the PS5
target. Its integer call path is now linked and hardware-tested as recorded
below. The full conformance test's ms_abi functions remain host-only reference code.

Remaining 0.2b work: native host-import entry stubs, native callbacks,
preserved GPR/XMM canaries, mixed arguments, aggregate returns, variadics
where needed, hardware telemetry and an independent execution validator.
PE32 cdecl/stdcall and x87 semantics remain separate required work.

### PS5 integer call result (FW 12.02)

Run `20260908T150617934Z_PPSA99995_prospero-win_0x102dcb26b5fc6`, source
`6e661a9`, passed the independent mapping validator with --expect-call6,
--root pinball.exe, --expect-modules 9, --expect-local 0, --expect-host 8,
--allow-i386, --allow-wx and --expect-compat32 refused.

- ELF SHA-256: `dc5415023418fc7e4bceaa64a787bbda4fed0f67c30a13a998bafb2930805ebf`.
- fSELF SHA-256: `bd5bcd6ae13c17c710d20fa2202f3e36261f318eae9aaafaf26b7ba9b0823557`.
- Transcript SHA-256: `ffe23998d460cca5a75701dcd01f6ac3a8662ce9ec4ca3dfd6dd83d611316f88`.
- PW_CALL6: constant=42, alignment=8, weighted=278, high=4294967574,
  sealed=1, released=1, status=ok, kind=synthetic-code.
- The original PE32 mapping then passed again; clean BYE, 54 records,
  no active BigApp after exit and all four services healthy.

This executes project-authored Win64 instructions from anonymous RX memory,
not instructions from the original Pinball. It establishes the six-integer
assembly call path, including stack arguments and home slots. It does not
close the full 0.2b ABI gate: preserved-register canaries, imports, callbacks
and floating/aggregate conventions still need native coverage.

Reference: [Microsoft x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention).

## PE32 execution

The tested sysarch(I386_SET_LDT, ...) route returns EINVAL on FW 12.02.
Controls and repeated measurements remain in COMPAT32_PHASE0A.md. The tested
compatibility-mode route is closed, so PE32 executes through prospero-win's
IA-32-to-x86-64 dynamic binary translator.

Dynamic binary translation is a form of CPU emulation. The current engine
translates and caches blocks, uses hashed lookup and direct chaining, keeps a
bounded guest-register set resident across blocks, eliminates dead flags and
materializes arithmetic flags lazily. Exact host differential tests and the
first playable hardware title establish the implemented subset; they do not
establish complete IA-32/SSE2 coverage or a native-performance percentage.

Low addresses do not make arbitrary 32-bit instructions safe to copy into
long mode. Address size changes, absolute disp32 can become RIP-relative,
stack width changes, and guest ESP and FS/TLS need treatment. Prefixes,
flags, x87/SSE state, indirect control flow, exceptions and self-modifying
code also need coverage.

Every added instruction family must retain differential tests against native
32-bit host execution. Wine startup and multiple applications now drive the
next coverage set; Pinball is no longer the sole oracle. Per-thread TEB/FS and
CPU state, SSE/SSE2, indirect-branch prediction, exception precision and
thread-safe immutable translated-code reuse remain required.

The pinned real-Wine control retires 33,367 guest instructions in 7,148
dispatches and 962 translated blocks while servicing 19 NT calls. The generated
application gate exercises 32 serviced NT call shapes and reaches its own entry
point in all four chaining/residency configurations, exiting cleanly after the
same 598,404 retired instructions and 2,981 blocks. These bounded host gates
expand the oracle beyond Pinball; they do not establish a PS5 Wine runtime boot.

## Measured memory facts

The 2026-09-08 low-memory probe recorded:

| Observation | Meaning and limit |
| --- | --- |
| 256 MiB at 0x10000000 in a title | One low mapping; not proof the full low 4 GiB is allocatable |
| 1 GiB and larger title requests failed | Large single mappings unavailable in that context |
| mmap hint honoured; MAP_32BIT returned high memory | Check returned address and release unacceptable mappings; never overwrite existing mappings with MAP_FIXED |
| mprotect to RX and RWX succeeded | Single-mapping executable protection available on tested firmware |
| Title anonymous budget previously around 432 MiB | Sparse mappings do not remove working-set constraints |
| 16 KiB pages | Adjacent 4 KiB PE sections can share protection |

Payload results are not title guarantees. Reserve/commit/decommit, multiple
low mappings and alternative backing memory need separate tests before
promising a larger guest address space.

## Executable publication and ownership

The backend uses one mapping and mprotect; write_base equals exec_base.
Prefer RW during construction and RX before execution. Never modify or
release a block while another thread may execute it.

The contract retains optional write/execute aliases. Relocations use
exec_base and writes use write_base. Double mapping is not a requirement
established by FW 12.02 results; adopting it needs implementation and
concurrency validation.

The mapper unions protections on shared pages and reports merged/WX
counts. The validator requires --allow-wx to accept such pages.

pe_image_machine_is_native() identifies CPU architecture, not ABI
compatibility, resolved imports or readiness to execute. The validator
accepts mapping evidence only; PE32 requires --allow-i386 and still needs
an execution engine.
