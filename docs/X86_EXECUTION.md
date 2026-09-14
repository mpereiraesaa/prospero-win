# x86 execution prototype

The first prototype in src/pw_x86_block.c translates a bounded sequence of
32-bit guest instructions into native x86-64 code. Its initial purpose is
to establish 4-byte guest stack semantics and dispatcher transitions needed
by Pinball's entry. It is not a complete decoder or CPU implementation.

Supported encodings: push imm8/imm32/r32 and r32/memory (FF /6), pop r32, mov r32/imm32,
mov r32/r32 and r32/memory (89/8B ModRM/SIB), LEA,
MOV immediate/r32 or memory (C7 /0), register/memory ADD (01/03), SUB (29/2B), XOR (31/33),
NOT/NEG r32/memory (F7 /2,/3), LEAVE (C9),
byte MOV (88/8A, C6 /0, B0-B7), byte CMP (38/3A, 80 /7, 3C),
byte TEST (84, F6 /0, A8), register/memory INC/DEC (40-4F, FF /0,/1),
Immediate ADD/OR/ADC/SBB/AND/SUB/XOR/CMP 16/32-bit (81/83 and accumulator
forms, optional 66 prefix), CMP r32/memory
(39/3B), TEST 32-bit register/memory or immediate (85, A9, F7 /0),
MOVZX/MOVSX byte/word to 32-bit (0F B6/B7/BE/BF), all short/near Jcc and register-byte SETcc,
32-bit SHL/SHR/SAR with immediate, implicit-one or CL counts (C1/D1/D3),
Absolute and FS-prefixed A1/A3 moffs32 loads/stores
through EAX, nop, direct call rel32, indirect near call/jump (FF /2,/4),
jmp rel8/rel32 and ret/ret imm16 (C3/C2). Calls push a 32-bit guest return PC and yield the
target EIP to the caller. Ret reads that PC and yields again. Guest ESP
lives in the state structure, independently from native RSP. No guest stack
opcode is copied as a 64-bit push/pop/call/ret.

The caller supplies a live RW low-address guest stack and keeps it mapped
for the full execution lifetime. Generated bounds checks verify each 1-, 2- or 4-byte
access and preserve the faulting guest PC on failure. Prior completed guest
instructions remain committed. The translator emits into writable scratch;
only successful translations may be published RX. Unsupported or truncated
input and insufficient output capacity report failure.

FS accesses use `fs_base` and `fs_bytes` in guest state, never the native
thread's segment register. The caller owns the live RW low-address mapping.
Generated checks reject offsets outside that mapping, base/offset overflow,
and a dword crossing the 32-bit address limit before any memory access.
This is an addressing primitive, not a complete TEB or exception subsystem.

ModRM/SIB uses 32-bit effective-address arithmetic, including wraparound,
signed disp8 and absolute disp32 (never host RIP-relative). LEA only computes
an address. MOV accepts the live stack or up to eight additional identity-
mapped regions with independent read/write permissions. The caller owns
their lifetime and must register only real, accessible mappings. A SysV
helper validates each dword before access; generated blocks contain its
process-local address and are not serializable. Native RSP is aligned and
the context pointer preserved across that call. This registry is not a
virtual-memory allocator or a security boundary against a hostile host.
All three memory ModRM modes and every SIB byte are covered by host LEA
tests, including truncated encodings. These tests are not a full decoder
conformance suite; operand/address-size overrides are not supported.

SUB snapshots the six arithmetic flags (OF/SF/ZF/AF/PF/CF) after the native
32-bit subtraction, retaining all other guest EFLAGS bits in the state.
Guest control flags are never installed in native RFLAGS. Tests cover edge
values for borrow, signed overflow, auxiliary carry, parity, sign and zero;
MOV and failed memory accesses preserve the recorded guest flags.
XOR updates its five defined arithmetic flags, clears OF/CF, and preserves
the previous guest AF as a deterministic choice for that undefined flag.

## Host evidence

test_pw_x86_block.c executes generated code after RW-to-RX protection and
checks calls, ret, stack boundaries, sign extension and rejection paths.
Register stack tests cover all eight registers, old-ESP semantics for push,
loaded-ESP semantics for pop, and partial progress before a stack fault.
Register MOV tests cover both encodings and every source/destination pair.
FS tests cover a synthetic exception-chain sentinel read, a write, the last
valid dword, out-of-bounds offsets, undersized regions and address overflow.
Those are host contract tests, not differential i386 or PS5 FS evidence.
The generated-function invocation alone disables Clang's UBSan function-type
check: JIT code has no compiler metadata preceding its entry. AddressSanitizer
and the other undefined-behavior checks remain enabled for the host code;
they do not instrument generated machine instructions.
test_x86_differential.py assembles a separate i386 ELF from project-authored
assembly, runs it directly using the host Linux i386 execution support, and
compares its three stack words and a wrapping SIB address with the translated
program. The return PC, both pushed values and computed address match byte
for byte. GNU as/ld and host i386 execution
support are required; this check does not substitute an emulator silently.

The differential test covers only that instruction sequence, not the whole
supported subset or arbitrary guest blocks. Generated blocks now have
integration-level PS5 evidence from the original game runtime, but not a
separate instruction-by-instruction hardware differential suite.

## Pinball dependency

The private executable's entry begins with two immediate pushes and a
direct call, which this subset represents. Its helper then accesses FS:0
and constructs an x86 exception-registration frame. We must implement a
guest TEB and exception chain; using the native thread's FS state is wrong.
The bounded host tracer now executes instructions directly from the private
file through this engine. Build `make build/host/trace_x86_entry`, then run
`build/host/trace_x86_entry /private/path/Pinball.exe`. Exit 2 is a classified
stop, not successful application startup; exit 1 is setup/cleanup failure.
The tracer owns a synthetic stack and FS region and initializes only the
exception-chain sentinel, not a complete Windows TEB. It fetches from
executable PE sections through the existing mapper and registers headers
and sections with their logical access permissions. A typed catalog binds
function tokens and CRT data, with unsupported APIs stopped by identity.
The current tracer requires mapping at the preferred base; it rejects an
alternate base instead of executing with inconsistent guest addresses.
It stops at 256 dispatch/instruction events by default, unsupported decoding/API behavior
or memory-bound failure. The printed steps count excludes API dispatch events.

On 2026-09-08, input SHA-256
`2bbc8234685fe2f6324040af6ea20123cf00c4a56882ce0d9074f0beefac67bc`
initially completed 22 translated instructions through the startup helper.
With import binding and indirect-call dispatch, the same input now completes
495 instructions and 24 completed API calls (eighteen distinct APIs),
using the optional bounded event limit (`trace_x86_entry private.exe 4096`, up
to 10,000,000 events for long startup discovery), after
adding memory arithmetic and initializer epilogue support, clock services, logical TEST, guest arguments,
operand PUSH, initializer dispatch, guest FP control, absolute MOV, immediate ALU
operations, conditional execution and initial CRT state services:

```
kind=host-import-bind total=207 functions=205 data=2
kind=host-api dll=kernel32.dll name=GetModuleHandleA result=0x01000000
kind=host-api dll=msvcrt.dll name=__set_app_type result=0x00000000
kind=host-api dll=msvcrt.dll name=__p__fmode result=0x03300008
kind=host-api dll=msvcrt.dll name=__p__commode result=0x0330000c
kind=host-api dll=msvcrt.dll name=_controlfp result=0x0009001f
kind=host-api dll=msvcrt.dll name=_initterm result=0x0009001f
kind=host-api dll=msvcrt.dll name=__getmainargs result=0x00000000
kind=host-callback-enter dll=msvcrt.dll name=_initterm target=0x0101cd2b depth=1
kind=host-api dll=kernel32.dll name=GetSystemTimeAsFileTime result=0x030fff34
kind=host-api dll=kernel32.dll name=GetCurrentProcessId result=0x00000001
kind=host-api dll=kernel32.dll name=GetCurrentThreadId result=0x00000002
kind=host-api dll=kernel32.dll name=GetTickCount result=0x28d8442d
kind=host-api dll=kernel32.dll name=QueryPerformanceCounter result=0x00000001
kind=host-api dll=msvcrt.dll name=_initterm result=0x00000000
kind=host-api dll=kernel32.dll name=GetStartupInfoA result=0x030fff78
kind=host-api dll=kernel32.dll name=GetModuleHandleA result=0x01000000
kind=host-api dll=user32.dll name=LoadStringA result=0x00000020
kind=host-api dll=kernel32.dll name=lstrlenA result=0x00000020
kind=host-api dll=msvcrt.dll name=malloc result=0x03400000
kind=host-api dll=kernel32.dll name=lstrcpyA result=0x03400004
kind=host-api dll=user32.dll name=LoadStringA result=0x0000000a
kind=host-api dll=msvcrt.dll name=malloc result=0x03400030
kind=host-api dll=kernel32.dll name=lstrcpyA result=0x03400034
kind=host-api dll=kernel32.dll name=lstrcatA result=0x03400034
kind=host-api dll=kernel32.dll name=lstrcatA result=0x03400034
kind=host-api-stop dll=advapi32.dll name=RegCreateKeyExA status=-5
kind=host-entry-trace steps=495 stop=unimplemented-api eip=0xe0000010 esp=0x030ffdc4 ebp=0x030ffdf8 fs0=0x030fffe8 flags=0x00000202
kind=host-heap-summary blocks=3 live=2 requested=2041 arena=8388608 valid=1
```

That historical trace stopped at `RegCreateKeyExA`. The current registry
package completes create/query/close and create/set/close using real guest
arguments. The current exact-binary trace reaches the verified `WinMain`
address and retires 37,925 translated instructions before stopping at
`waveOutGetNumDevs`; two live allocations request 473 bytes at that point. It
transactionally creates the splash HWND, executes the original WndProc's
creation and paint callbacks, stores the splash state in four owned
window-extra bytes, constructs/composes the splash bitmap and returns from
`UpdateWindow` before entering keyboard scan-code discovery.
Before it, malloc, string and resource calls construct the startup state. The CRT reads
the GUI startup profile, walks the command line and
the second `_initterm` returns. Its original-game callback has completed through the translator and
guest ABI bridge. Clock values (and derived flags) vary across live runs;
the transcript above is one observed run, not a fixed-value invariant.
Synthetic tests separately exercise nested callbacks. `_controlfp` and the
startup x87 arithmetic families update isolated guest FP state without modifying
host FP controls. Absolute MOV tests exercise independent read/write permissions,
last-valid and crossing-boundary addresses, unchanged flags and fault atomicity.
Operand PUSH tests cover old-ESP addressing, source and destination faults,
flag preservation and continuation into the next instruction. PUSH checks
the source before committing the destination or changing guest ESP.
TEST regressions cover all five supported operand forms, sign/zero/parity
flags, cleared carry/overflow, preserved operands, and memory-boundary faults.
Undefined AF is retained deterministically, as with the other logical operations.
Memory-arithmetic tests cover operand order, aliasing of address/destination
registers, results/flags and bounds. NOT/NEG tests cover register/memory forms
and signed overflow; LEAVE/RET tests cover full frame teardown and invalid EBP.
Byte tests cover all low/high register MOV combinations, partial-register
preservation, comparison/test flags, last-byte memory access and crossing
faults. INC/DEC preserve guest CF while updating the other arithmetic flags.
The tracer accepts an optional maximum of 1..10000000 events and an optional
generic hexadecimal PC milestone, both validated before opening the executable.
It reports DBT dispatch, cache hit/miss/publication, retired-instruction,
emitted-byte and generation metrics. Default-budget, explicit-budget, cache-hit
and milestone regressions are synthetic and require no proprietary input.
RET imm16 tests cover zero, odd and boundary cleanup sizes, oversized cleanup
rejection, unchanged flags, and an actual translated stdcall callback with
two arguments. Return-PC reads and final ESP validation precede state publication.
FF /0,/1 tests verify register/memory INC/DEC, carry preservation, overflow
and continuation into the next instruction.
MOVZX/MOVSX tests cover every source/destination register combination, including
high bytes, aliasing and negative byte/word values, with unchanged guest flags.
Memory forms check the exact source width and reject crossing-boundary reads
without publishing a destination register value.
Shift regressions compare SHL/SHR/SAR results and defined flags against native
x86 for all 256 count bytes, all three count forms and register/memory operands.
Counts are masked to five bits; zero preserves every guest flag, AF is retained,
and OF is updated only for count one. Tests cover ECX/CL aliasing, continuation
and rejected crossing-boundary accesses, including a zero-count memory operand.
Rotates and 8/16-bit shifts are not yet implemented.
The tracer and its synthetic contracts reproduce under ASan/UBSan. The same
translator is integrated into the PS5 runtime and executes the original guest
continuously; this does not turn its supported subset into complete IA-32.
Immediate-ALU tests cover all eight operations, all three encoding forms,
16/32-bit operands, carry inputs, boundary values and memory/register results.
Read-modify-write requires both read and write permissions; rejected accesses
preserve memory and guest flags. ADC/SBB import only guest CF into the host
arithmetic operation, never guest control flags.
The tracer's `result` field reports EAX; `__set_app_type` returns void, so
that field is not a CRT return value (the same applies to `_initterm` and
GetSystemTimeAsFileTime and GetStartupInfoA). Regression tests cover all 16
conditions across 32 arithmetic-flag combinations, register-byte writes,
word-access boundaries, compare operand order and immediate sign extension.
The counts in this historical section are host-tracer evidence. Current PS5
evidence supersedes its old integration boundary: the native runner reaches
the message loop, changing frames and original PCM. See
HARDWARE_VALIDATION.md. Synthetic PE tests independently cover normal
instruction progress, unsupported stops, memory faults and a looping budget
stop. Executable bytes remain private; no extracted routine is embedded here.

The IAT slot at RVA 0x10f0 resolves by its preserved import lookup table to
`KERNEL32!GetModuleHandleA`; the caller pushes a null argument. The bound
address in the original file belongs to the old Windows image, not this
host. The mapper's writable copy is now rebound; the original file remains
unchanged. The GetModuleHandleA(NULL) response uses the mapped main-module
base and the shared stdcall return service. That statement describes the
historical first-IAT checkpoint; the production runtime now binds all 207
static imports required by the Pinball target.

The historical splash-era exact-binary trace retired 37,925 instructions and stopped at
`waveOutGetNumDevs` in source-confirmed WaveMix initialization. Its cache totals
are 3,807 dispatches, 3,469 hits and 338 publications (107,008 bytes). The GDI
live-state and post-reset cleanup records are described in [GDI.md](GDI.md).
Keyboard mapping, main-window creation, the nested audio helper window, REP
string operations, 16-bit tests and integer MUL/DIV were already covered at
that checkpoint. The current engine adds the remaining observed WaveMix,
message-loop and wndproc forms and runs continuously on PS5. Remaining broad
coverage includes TEB/exception delivery, SSE, cache eviction and a fuller
memory/scheduling model.
The diagnostic tracer uses the tested generation-scoped cache and reports its
dispatch/publication metrics. Integer-only binary80 execution covers every
startup x87 form without touching host FP state. Masked stack overflow and
underflow now publish AMD's indefinite response, TOP/tag changes, `IE|SF` and
directional `C1`; unmasked x87 exceptions produce a distinct pending guest trap
with precise PC/retirement and no destination publication. Guest
exception-handler delivery, SSE, cache eviction,
a full memory model and scheduling remain.

Current cache lookup is open-addressed by guest PC, so hot dispatch is O(1) on
average and exposes total/max probe counts. Cold publication preserves W^X but
transitions only the generated block's host pages, rather than all 4 MiB of the
arena. The engine remains deliberately single-dispatcher: native audio and I/O
workers increase runtime concurrency without racing guest registers, flags,
memory ownership or translated-code publication.

## Measured optimizations and synthetic benchmarks

Cold block construction originally evaluated source lengths incrementally from
1 to `available` bytes in an O(N^2) byte-by-byte probing loop. This is replaced
with one bounded, linear-time translation invocation: `pw_x86_translate`
ingests the available source directly. When encountering an
unsupported opcode, truncated instruction or terminal jump/call/ret, any
previously decoded instructions in the basic block are cleanly closed and emitted,
reducing cold translation from O(N^2) to O(N).

Warm execution hot paths are accelerated via inline fast paths:
- Memory access bounds checking: Inlines guest bounds checks for both the active
  stack region and primary memory region (`memory[0]`), validating 32-bit pointer
  limits and permissions inline before directly accessing host pointers. Only
  unregistered regions or out-of-bounds addresses fall back to the C helper
  `memory_pointer`.
- Branch condition evaluation: Replaces indirect function calls for simple
  conditional branches (OF, CF, ZF, SF, PF and composite unsigned comparisons)
  with direct bit tests against `state->eflags` in the emitted block code, falling
  back to C helpers only for complex signed condition codes.

The reusable synthetic IA-32 benchmark suite (`tools/bench_dynarec.c`) profiles
five representative workloads:
1. `reg_alu`: Dense 32-bit register arithmetic, logic, shifts, and multiplications.
2. `cond_branch`: Collatz branch sequence exercising flag branches and convergence loops.
3. `call_ret`: Function call nesting, stack frame management, and return transitions.
4. `mem_load_store`: ModRM/SIB array indexing, structured traversal, and memory writes.
5. `x87_fp`: Binary80-backed floating-point load, add, multiply and store operations.

Each workload reports deterministic register and full-state checksums used as
regression fingerprints. `tests/test_dynarec_bench.py` also executes the
`reg_alu` workload independently as a native Linux i386 binary and compares its
four live output registers. The other four workloads are deterministic DBT
regressions, not native-oracle comparisons. Each timing sample contains eight
complete deterministic runs, keeping setup outside the timed intervals;
`batch_runs`, retired instructions and dispatches make that denominator
explicit in every record.

### Local dead-flag elimination

Translation uses bounded decode, backward-liveness and emission passes over at
most 32 guest instructions. Arithmetic flags remain fully materialized in
`PwX86State`; there is no hidden flag state crossing a block boundary. The
liveness pass only omits a flag snapshot when a later instruction in the same
block provably overwrites every affected flag before any observation.

Two cases are deliberately conservative:

- A guest memory guard can fail before its instruction changes flags, so every
  incoming arithmetic flag is live at a potentially faulting instruction.
- A shift by zero preserves flags. Immediate counts are classified after x86's
  five-bit mask; a `CL` count is treated as consuming the prior deterministic
  flag subset because its zero/nonzero value is only known at runtime. For a
  multi-bit shift, OF remains the runtime's preserved undefined value.

These rules are covered by executable regressions in `test_pw_x86_block`.

### Linked-block execution

Direct and conditional exits use RW link slots rather than rewriting published
RX code. A bounded block quantum returns to the dispatcher for services and
safepoints. Generation reset invalidates every link. Compatible blocks carry up
to three deterministic guest GPRs in `r8d`-`r10d`; mismatched contracts spill
through a reconciliation stub before entering the target's canonical entry.
C helpers and fault boundaries preserve or spill resident values explicitly.
Common 32-bit register ALU, compare and INC/DEC forms operate directly on those
host registers. Blocks shorter than four instructions and blocks containing a
helper boundary remain canonical because their measured spill/load overhead
cannot amortize residency.

The pinned generated-Wine application is also a differential performance
fixture. Every chaining/residency mode in its four-case matrix retires 598,404
instructions over 2,981 translated blocks with the same exit status. Chaining
reduces dispatcher returns from 120,931 to 15,787. In the chained resident run,
profitability filtering reduces resident canonical stores to 12,656 and
reconciliation spills to 61,805 while emitted code is 1,759,312 bytes versus
1,678,688 bytes without residency (4.8% larger). These counters are pinned
regression data; host timing and PS5 timing are measured separately.

Live arithmetic flags can cross a linked boundary as an eight-byte RAW
descriptor (`raw_flags`, `known_mask`). Consumers merge only the required guest
bits inline; no helper call or host control flag installation is involved. The
engine commits the complete pending subset at every dispatcher safepoint.
Variable-count shifts and string helpers are explicit ownership boundaries and
commit before they replace canonical EFLAGS. The standalone `pw_x86_translate`
API keeps eager EFLAGS semantics because it has no engine safepoint.

The host tracer exposes controlled parity switches:

```sh
PW_TRACE_CHAINING=1 PW_TRACE_RESIDENCY=1 PW_TRACE_LAZY_FLAGS=1 \
PW_TRACE_QUANTUM=64 build/host/trace_x86_entry /private/path/app.exe 100000
```

For the private first target, the four chaining/lazy combinations reached
identical bounded endpoints: 461,087 retired instructions without chaining and
1,609,088 with a 64-block quantum, with exact EIP, GPR, FS and EFLAGS parity.
Lazy mode increased emitted code by less than one percent in that trace
(935,856 versus 929,488 bytes). Seven paired host runs rounded to 0.11-0.12 s
in both modes, so this is a correctness and architecture result, not yet a
claimed speedup. `PW_BENCH_CHAINING`, `PW_BENCH_RESIDENCY` and
`PW_BENCH_LAZY_FLAGS` provide the same A/B controls for the synthetic suite.
