# Reproducible i386 Wine PE runtime

Wine PE modules are build inputs, not sources of this repository. This
document defines how they are produced, how their identity is recorded and
what a second developer has to reproduce to obtain the same bytes.

The pinned reference is:

```text
Wine 490f6d5dcbb2a5047345b8af88d114bbcaad69a8  (Wine version 11.17)
```

## What is staged

## Current capability (evidence checkpoint)

The measured state of this work, reproducible with the commands below. It is a
bounded host gate, not a PS5 Wine-process launch.

| Area | State |
|---|---|
| Runtime distribution | Three i386 PE modules (`ntdll`, `kernelbase`, `kernel32`) built reproducibly from the pinned revision plus every NLS data file that revision tracks (its whole `nls/*.nls` - the locale tables, the sort keys, the case map, the normalization tables and one file per codepage), each recorded with its own hash and folded into the distribution digest `a70042324ceb268a714936501add18de2cd0a4a6f3fd16b7a758e7893c186bb5` |
| Module graph | `kernelbase`'s 428 imports bind against `ntdll`'s exports with zero failures, by name, ordinal and forwarder |
| PE32 TLS | Parsed and relocated for the generated application graph; one process thread, its TEB and TLS traversal are modelled. Loader-list and callback-order validation remain. |
| ntdll control | Real `LdrInitializeThunk` executes through the IA-32 translator: 33,367 retired instructions, 7,148 dispatches, 962 translated blocks, 19 calls serviced, `host_calls=0`, and complete cleanup. |
| Unix-call boundary | Both Wine dispatcher slots are published. The syscall table has all 256 pinned i386 entries and the Unix-call table has all eight pinned entries; both match Wine source. The complete handler ledger currently contains 32 serviced NT call shapes plus two classified termination stops. |
| Platform services | Files (open/read/query/close, one gate-owned directory object), image sections (`NtCreateSection`/`NtQuerySection`/`NtMapViewOfSection`/`NtProtectVirtualMemory`), NLS data (`NtInitializeNlsFiles`, `NtGetNlsSectionPtr` and the three locale queries), registry (open/create/query/set against a host profile), token (`TokenUser`), threads (`NtGetNextThread`, `NtQueryInformationThread`), object namespace (`\KnownDlls` plus section lookups), system information (the Wine version class) and process information (the process image, from the module's own headers) |
| Application checkpoint | A generated PE32 executable, two DLLs and their dependency diamond load through Wine's own ntdll. All four chaining/residency configurations reach its transfer address, return `1`, and exit through `NtTerminateThread` after the same 598,404 retired instructions and 2,981 blocks. |
| Not claimed | Wine's loader lists and DllMain/TLS ordering are not independently validated; the registry is run-local rather than a persistent prefix; no console or hardware Wine evidence exists. |

Reproduce it:

```sh
tools/build_wine_runtime.sh --source <pinned-wine-checkout>   # stages .deps/wine-runtime
make all                                                     # every suite + publication audit
make sanitize                                                # clang ASan+UBSan
make wine-check                                              # fails instead of skipping the
                                                             # pinned-source and real-gate checks
```

The sections below are the discovery journal: each step, the measurement that
justified it and the honest limits at that point.

`tools/build_wine_runtime.sh` builds the selected i386 PE modules and stages
them, with a manifest, into an ignored directory:

```text
.deps/wine-runtime/
    lib/i386-windows/ntdll.dll
    lib/i386-windows/kernelbase.dll
    lib/i386-windows/kernel32.dll
    nls/locale.nls          and the other data files below
    wine-runtime-manifest.json
```

`ntdll`, `kernelbase` and `kernel32` are the first boot attempt's module set:
`kernel32` forwards into `kernelbase`, `kernelbase` forwards into `ntdll`, and
`ntdll` owns the versioned Unix-call boundary named in
[WINE_INTEGRATION.md](WINE_INTEGRATION.md). Nothing else is selected yet, so
the staged runtime is deliberately not a complete Win32 surface.

The modules are PE32/i386 images. The Unix-side modules (`*.so`) are *not*
staged: they are not loadable objects on this target, and the whole point of
the DBT is that guest PE code runs natively translated instead.

The `nls/` files are the distribution's own NLS data: every `*.nls` the pinned
revision tracks, which is `locale.nls`, `sortdefault.nls`, `l_intl.nls`, the
normalization tables and one file per codepage (`dlls/ntdll/unix/env.c:93`
names them). The pinned revision tracks
them as source files, so staging them is a copy and not a second build, and
the manifest records each one's size and hash and folds them into the
distribution digest: a distribution that carries the data is not the
distribution that does not, and the run needs them because the pinned
runtime's own locale initialisation maps and parses `locale.nls`
(`RtlGetLocaleFileMappingAddress`, `dlls/ntdll/locale.c:613`, and kernelbase's
`init_locale`, `dlls/kernelbase/locale.c:438`). The host file service looks a
runtime-namespace name up in that directory first and in the module directory
second, which is the order Wine itself uses (`open_nls_data_file`,
`dlls/ntdll/unix/env.c:120`).

## Reproducing the build

```sh
# A clean checkout of the pinned revision is required. The script clones one
# when PROSPERO_WINE_SOURCE does not exist.
export PROSPERO_WINE_SOURCE=/private/wine-11.17
export PROSPERO_WINE_ROOT=/private/wine-build
tools/build_wine_runtime.sh --check-reproducible
```

Requirements the script enforces rather than assumes:

- the checkout is clean and its `HEAD` is exactly the pinned commit;
- the build is out-of-tree, in `PROSPERO_WINE_ROOT/build`;
- the environment is normalised: `LC_ALL=C`, `TZ=UTC`,
  `SOURCE_DATE_EPOCH=1000000000`;
- the configure line is fixed:
  `--enable-archs=i386,x86_64 --disable-tests`.

`--check-reproducible` stages the modules, forces a rebuild of every target in
the same configured build directory (`make -B`) and stages them again,
comparing every module byte for byte before writing the manifest. That is two
forced rebuilds in one pinned build tree, not yet two independent clean build
roots: building in two empty roots with different absolute paths is the
stronger check and is not claimed here. On this host the PE linker
is GNU `ld` (binutils) through `i686-w64-mingw32-gcc`; it honours
`SOURCE_DATE_EPOCH` for the PE timestamp, which is the only non-deterministic
field otherwise present. The observed difference between two builds without
that variable is exactly the file-header `TimeDateStamp` and the derived
optional-header `Checksum`; with it, two forced rebuilds in one pinned build tree are byte-identical.
No field is zeroed, masked or excluded from the digest.

Tool versions that can change the bytes are recorded in the manifest, because
an identical source revision plus a different linker is a different runtime.

## Manifest contract

`tools/validate_wine_runtime.py write` hashes the staged modules and writes
the manifest; `check` validates a manifest against a staged tree.

The manifest records, per module, the canonical lowercase name, the relative
path inside the distribution, the byte size, the SHA-256 and the PE machine
and magic; plus the Wine commit, the configure line, the normalised
environment, the tool versions and the aggregate digest. The JSON schema is
[WINE_RUNTIME_MANIFEST.schema.json](WINE_RUNTIME_MANIFEST.schema.json).

The aggregate digest is reproducible from the manifest alone:

```text
sha256( concat( sorted_by_name( "name<TAB>size<TAB>sha256\n" ) ) )
```

`check` fails closed on: an unknown schema, an unknown architecture, a second
entry for the same canonical name, a name that is not a lowercase `.dll`, an
absolute or `..`-traversing path, a missing file, a size drift, a hash drift,
a module whose PE machine is not i386, a module that is not PE32, a path that
is not inside the declared library, and an aggregate digest that disagrees
with the module list it is supposed to summarise. Every one of those cases is
exercised by `tests/test_wine_runtime_manifest.py`.

## Publication rules

- Generated PE modules and the build tree stay in ignored directories
  (`.deps/`, `build/`). `tools/audit_publication.py` refuses a Windows binary
  anywhere in the tree, so a staged runtime can never be committed by
  accident.
- No Wine source is vendored. The manifest and this document are the whole
  public footprint of the runtime, and Wine remains LGPL-2.1-or-later with its
  own authorship and notices.
- The runtime directory is configured explicitly on both the host and the PS5
  provider. There is no fallback that would silently load an application-local
  `ntdll.dll`.

## Bounded ntdll entry gate

`tools/wine_ntdll_entry.c` is the integration milestone: it loads the staged
runtime, binds the real module graph, enters one exported ntdll function
through the IA-32 DBT and stops at Wine's versioned Unix-call boundary.

```sh
make build/host/wine_ntdll_entry
build/host/wine_ntdll_entry --runtime .deps/wine-runtime/lib/i386-windows \
    > /private/wine-ntdll-entry.txt
python3 tools/validate_wine_ntdll_evidence.py /private/wine-ntdll-entry.txt \
    --manifest .deps/wine-runtime/wine-runtime-manifest.json \
    --expect-entry NtClose
```

What the gate proves, and what it does not:

- it maps `ntdll.dll` and `kernelbase.dll` from `PW_FILE_RUNTIME`, and binds
  the 428 imports `kernelbase` declares against `ntdll`'s exports with the
  same resolver every other caller uses;
- it identifies the boundary structurally. Wine's i386 PE syscall stubs end in
  `call <__wine_syscall>`, and `__wine_syscall` is the single
  `jmp dword ptr [__wine_syscall_dispatcher]` thunk that references the
  exported dispatcher slot. The gate locates the slot through the export
  directory and the thunk by scanning the mapped `.text`, so the stop address
  is one the manifest-hashed image really contains;
- it enters the exported `NtClose` stub, retires its real instructions and
  stops *before* the dispatcher jump, reporting the syscall number the stub
  encoded (0x0f) and the number observed in EAX;
- it never calls a host Wine function, and it refuses to accept a stop that is
  not the Unix-call boundary.

It does not implement the Unix call, does not create a PEB/TEB, does not run
ntdll's process initialization and is not wired into title startup. A
classified stop is evidence, not a compatibility claim.

The bridge is assembled from units, each with its own contract:

| Unit | Owns |
|---|---|
| `src/pw_wine_gate.c` | the run: mapping, binding, the DBT loop, the virtual-memory calls and the dispatch registry |
| `src/pw_guest_vm.[ch]` | the low-address policy and the contract with the backend underneath |
| `src/pw_guest_process.[ch]` | the stack, the TEB, the PEB and the process parameters |
| `src/pw_nt_handle.[ch]`, `src/pw_wine_handle.[ch]` | the typed handle table with single-issue values, and what the run does around it |
| `src/pw_wine_path.[ch]` | guest names turned into canonical strings for all three namespaces |
| `src/pw_wine_file.[ch]` | files and the gate-owned directory object |
| `src/pw_wine_registry.[ch]` | registry keys and values |
| `src/pw_wine_object.[ch]` | the object namespace |
| `src/pw_wine_query.[ch]` | the answers that come from host state: Wine version, token, process image |

`pw_wine_gate_run()` orchestrates those units. None of them calls a host Wine
function, the file, registry and object adapters reach the platform only
through the service vtable the caller supplies, and the name translation and
the virtual-memory service are pure enough to be tested without a mapping
(`tests/test_pw_wine_path.c`, `tests/test_pw_guest_vm.c`).

### How far real ntdll execution currently gets

Two entry points are measured, each with the engine's chaining, register
residency and lazy-flag modes toggled (four configurations, identical stop
kind, retired count and stop address):

| Entry | Result |
|---|---|
| `NtClose` | stops on the dispatcher thunk after 3 retired instructions, syscall `0x000f` |
| `LdrInitializeThunk` | retires 266 instructions over 35 dispatches and reaches ntdll's **first Unix call**: syscall `0x0018` (`NtAllocateVirtualMemory`), again stopping before the dispatcher jump |

Reaching that point required four general instruction families and a minimal
guest thread block, all of which are now implemented and host-tested:

- **FS-prefixed absolute operands** (`mov r32, fs:[disp32]` and
  `mov fs:[disp32], r32`). Wine's ntdll reads `fs:[0x18]` (the TEB self
  pointer) on its first initialization instructions. The segment base is the
  guest's own FS base from `PwX86State`, never the host's, and the offset is
  bounded by the declared FS block before any dereference. GS and FS operands
  with a base or index stay refused.
- **LOCK-prefixed read-modify-write** for the group-1 memory forms
  (`lock add [mem], 1`, which Wine's critical sections use). The emitted host
  instruction carries the same prefix, so the host provides the atomicity; a
  register destination or a pure compare is not a legal LOCK form and stays
  refused.
- **XCHG with a memory operand** (`xchg [mem], reg`, with or without the
  redundant LOCK prefix), which is what ntdll's heap takes an entry off a free
  list with. i386 performs that form atomically whether LOCK is written or not,
  so the emitted code calls a helper that performs exactly one host exchange,
  and the register and the operand swap values while no flag is written at all.
  The register form has no memory operand to exchange and the byte and 16-bit
  forms are not implemented, so all three stay refused.
- A **minimal guest TEB and PEB** owned by the gate: the TEB is what FS points
  at, with the documented NT offsets for `StackBase` (0x04), `StackLimit`
  (0x08), `Self` (0x18) and `ProcessEnvironmentBlock` (0x30), and the PEB is
  what the process parameters hang off.
  The initialization entry's first argument is not the PEB but the register
  *context* the kernel builds at the top of a thread's initial stack
  (`winnt.h`'s `I386_CONTEXT`, 0x2cc bytes, `Eax` at 0xb0): the gate fills one
  in there with `ContextFlags = CONTEXT_FULL`, the user CS/SS and an `Eflags`,
  leaves `Eax` null so `loader_init` publishes the process image's own entry
  point through it (`dlls/ntdll/loader.c:4527`) and `Ebx` null as the argument
  of a first thread, and passes its address. That is what makes
  `signal_start_thread` (`dlls/ntdll/signal_i386.c:514`) clear the 0xf000
  bytes of stack *below* itself instead of below the PEB.
  `ThreadLocalStoragePointer` (0x2C) stays zero because no module of this
  distribution declares a TLS directory.
- **BT/BTS/BTR/BTC**, both the register-index encodings (`0f a3/ab/b3/bb`) and
  the immediate form (`0f ba /4../7`), for register and memory destinations
  and for the 16-bit forms. The host instruction is re-emitted on guest
  values, so bit-string addressing of a memory operand and the 4/5-bit index
  masking of a register operand come from the CPU. Only CF is written; the
  flags the ISA leaves undefined stay unchanged, which is deterministic
  rather than arbitrary. A register-destination BTS/BTR/BTC reads, modifies
  and writes back; a memory one needs write permission, while a memory BT
  only reads.
- **CMOVcc** (`0f 40..4f`, 32-bit forms) with the condition materialised from
  guest flags and the move skipped when it does not hold, so flags are never
  written and the destination keeps its value otherwise.

Every family has native i386 differential coverage: `make test` runs a
32-bit reference program that installs a real FS base with `set_thread_area`
and executes the same `mov edi, fs:[0x18]`, `lock addl $1, mem`,
`btsl %ecx, %eax` and `cmovel %esi, %edx` instructions on the host CPU, and
the translated engine's output must match it byte for byte. Each family is
also executed through residency and lazy-flag modes in the unit suite.

The syscall number itself is verified twice: from the entry stub when the
entry is a stub, and always from the **issuing stub inside the mapped image**
— the return address on the guest stack points into the stub that called the
dispatcher, and the `mov eax, id` immediately before its
`mov edx, <dispatcher thunk>` must name the number observed in EAX. A number
that cannot be re-read from the image is rejected by the evidence validator.

Mode parity is asserted on this real code: all eight combinations of
chaining, register residency and lazy flags retire 266 instructions and stop
on the same thunk with the same syscall. That check earned its place — it
exposed a real defect in the first CMOV emission, where the skip branch
covered three bytes and jumped into the middle of a two-byte `mov` whenever
the condition did not hold; register residency changed the following bytes
enough to turn the fault into a silent no-op, so only the mode matrix found
it. `tests/test_pw_x86_block.c` now runs the new families through all four
residency/lazy combinations, and the gate runs the real ntdll initialization
through all eight.

The next unimplemented behaviours on that path are the rest of Wine's loader
initialization — which needs a real `PEB_LDR_DATA` and process parameters
rather than the gate's zeroed page — and the Unix call itself. Neither is
claimed as working.

### Servicing the first Unix calls

`--bridge 1` turns the gate's boundary from a stopping point into a bridge:
the call is identified, its arguments are read out of validated guest memory,
a handler services it, and the guest continues exactly where the stub's own
`ret imm16` would have left it.

The call number is a position in a table that belongs to one Wine revision,
so `src/pw_unix_call.c` carries the first 64 entries of the pinned i386 table
with their exported names and stdcall argument widths, and
`tests/test_unix_call_table.py` re-derives that table from
`dlls/ntdll/ntsyscalls.h` at the revision named in the source and fails on any
difference. Numbers outside the table are reported as unknown, never guessed.

The frame has two levels, because a Wine stub reaches the dispatcher with a
call rather than a jump:

```text
[esp]      return address into the issuing stub   (provenance, and where the
                                                   syscall number is re-read)
[esp+4]    the caller's return address            (where the guest resumes)
[esp+8..]  the stdcall arguments
```

Servicing a call therefore means: `EAX = NTSTATUS`, `EIP = [esp+4]` and
`ESP += 8 + arg_bytes` — the state the stub's own `ret imm16` would have
produced.

The first handler is `NtAllocateVirtualMemory` for the profile a boot attempt
reaches: the current process handle, `ZeroBits == 0`, `MEM_RESERVE`,
`MEM_COMMIT` or both, `PAGE_READWRITE` (or the reserve-only value), a bounded
total per run, and guest pointers that must pass the dispatcher's own region
check before anything is read or written. Value errors are answered with an
NTSTATUS, because that is the guest's contract; a guest pointer the bridge
cannot safely touch is a refusal that names the argument index instead. Every
block it maps joins the dispatcher's declared regions, so the guest can
immediately use what it was given, and every block is released at cleanup.

A `MEM_COMMIT` that lands inside a block this run already mapped is treated as
the second half of "reserve then commit" rather than a new reservation, which
is what Wine's loader does.

Measured on the pinned runtime:

```text
LdrInitializeThunk, --bridge 1
  retires 315 instructions over 44 dispatches
  services  2 NtAllocateVirtualMemory calls, both STATUS_SUCCESS
  maps      1 guest region of 65536 bytes, addressable to the guest
  stops at  ntdll RVA 0x1cdd5 on "movd xmm0, eax" (SSE), the next family
            the translator does not cover
```

Honest limits at that point: the reserve/commit distinction is not modelled
(the dispatcher knows one kind of guest region) and only a handful of calls had
handlers. A platform call that writes guest outputs now preflights **every**
output span before it changes anything, so a call either commits completely or
not at all: an allocation cannot be published and accounted for when the guest
could never be told the base and size it received, and a release cannot return
a mapping the guest cannot be told it gave up. `tests/test_pw_wine_gate_bridge.c`
injects each of those failures - an unwritable `*RegionSize`, an unwritable
`*BaseAddress` on the allocation and on the release - and asserts that region
ownership, live region count, declared bytes, cleanup count and the guest's own
outputs are exactly what they were.

`tests/test_pw_wine_vm_transactions.c` goes under the calls themselves: it
wraps the real backend in a double that fails one step on demand and counts
only the calls that belong to the guest's heap window, and it injects a refused
`reserve_at`, a refused `commit`, a refused `release` and an exhausted
declared-region table (with a control run per mode, so the counters can be
compared instead of guessed). The commit case is the one that proves the
rollback: the reservation the backend made is given straight back, so the
number of live heap mappings the backend holds at the end is zero whether the
call succeeded or failed, and the guest is never told about memory it cannot
use. Two contracts this work made visible are now fixed rather than documented
around. The gate's low-address wrapper used to *replace* the vtable's `context`
field with a pointer to its own struct, so any backend that keeps state there
was corrupted - invisible with today's stateless posix backend, fatal with the
next one. And a backend that refused every candidate made one reservation walk
the whole `0x10000000..0x40000000` window (196 608 candidates at 4 KiB) before
falling back. Both live in `src/pw_guest_vm.[ch]` now: the unit keeps the
caller's vtable *and its context* exactly as it received them, forwards every
call with the backend's own context, wraps only the callbacks the backend
actually has, and bounds the candidate scan to `PW_GUEST_VM_MAX_CANDIDATES`,
reporting exhaustion in the evidence (`kind=host-wine-low exhausted=`) so a
PE32 mapping that lands on the fallback has a stated reason.
`tests/test_pw_guest_vm.c` covers the policy, the bound and the context
contract with a backend double that asserts on its own context.

### What ntdll initialization reaches now

Servicing the first calls pushed real ntdll code into the DBT that nothing had
executed before, and each stop then named the next missing family. Current
state of that path with `--bridge 1`:

```text
retired 8582 instructions over 1440 dispatches and 253 translated blocks
3 NtAllocateVirtualMemory calls serviced, 2 guest regions mapped (88 KiB)
stops at ntdll RVA 0x28ef7 with a classified memory bound: the guest read
2 bytes at 0x7ca, i.e. CurrentDirectory.Buffer was null
```

The families added for that path, each with architectural tests:

- **SSE data movement and lane shuffles**: `movd` both directions, `movdqa`/
  `movdqu` (`66`/`F3 0F 6F`/`7F`), `movups`/`movaps` (`0F 10/11/28/29`),
  `movq` store and load (`66 0F D6`, `F3 0F 7E`), `movss`/`movsd` memory
  forms, the `punpck`/`packss` lane family (`60`-`6D`), `pand`/`paddq`/
  `psubq`/`por`/`pxor`, `pshufd`/`pshufhw`/`pshuflw` (`66`/`F3`/`F2 0F 70`),
  `pextrw` and `pinsrw`. The i386 XMM register file lives in
  `PwGuestFp.xmm`, host XMM registers are scratch, and the host executes the
  same operation, so upper-bit zeroing (`movd`, `movss`, `movsd`, `movq`),
  lane order and the 16-byte granularity come from the CPU. MMX encodings of
  the same opcodes, the merging scalar register forms and anything outside
  the list stay refused.

  Two boundaries are deliberate. The packed moves that *require* 16-byte
  alignment - `movaps`/`movapd` (`0F 28/29`) and `movdqa` (`66 0F 6F/7F`) -
  are accepted **between registers only**: their memory forms stay refused
  until the dispatcher has a classified guest alignment fault, because the
  host executes the emitted instruction and a misaligned guest address would
  otherwise become an uncontrolled host fault rather than the guest's own
  exception. `tests/test_classify_x86.py` pins both halves of that rule.
  And no accepted SSE form is verified only by the instruction the emitter
  wrote: `tests/test_pw_x86_reference.S` executes a lane sequence
  (`movdqu`, `movd`, `paddw`, `pshufd`, `punpckldq`, `movdqa`, `pand`,
  `movups`, `pmovmskb`, `pextrw`) as real native i386 and prints the stored
  lanes, the lane mask and the extracted word, and the translated engine must
  produce the same 24 bytes - so the semantic oracle for that slice is the
  CPU, not a second copy of our own emitter. Each of those forms is also run
  through all four residency/lazy-flag combinations in the unit suite, and the
  `movd`-to-memory defect that started this is now one of them.
- **BSF/BSR** (`0F BC/BD`, 32- and 16-bit, register and memory sources): the
  index comes from the host instruction, a zero source leaves the destination
  unchanged deterministically (the ISA leaves it undefined) while ZF still
  reports the zero, and only ZF is written.
- **The 16-bit shift group** (`66 D1`/`D3`/`C1` with the `shl`/`shr`/`sar`
  registers): the destination's upper 16 bits are untouched, the count is
  masked to four bits rather than five - which also changes which counts
  preserve the flags - and the flag policy matches the 32-bit path
  (`count == 0` preserves everything, `count == 1` sets OF, wider counts keep
  the deterministic subset).
- The memory guard accepts 16-byte accesses now, which is the width of the SSE
  loads and stores, and records the address, width and direction of a refused
  access, so a `memory-bounds` stop names the fault instead of only its kind.
- A REP prefix (`F3`) that does not introduce a string instruction falls
  through to the SSE slice instead of being refused as a malformed `REP`.

Two further gaps surfaced by the same path were closed in the gate: ntdll
reads `PEB->ProcessParameters` (PEB+0x10) during heap and loader
initialization, so the gate owns a zeroed process-parameters page and links
it, and the FS-segment work from the previous step is what makes the TEB
reads valid. The gate's PEB is still a zeroed page with two fields filled: it
is not a Windows process environment.

Testing note: the SSE slice is verified by explicit architectural
expectations (upper-bit zeroing, lane order, 16-byte guard behaviour, and the
same instructions through all four residency/lazy-flag combinations) rather
than by the native oracle, because the emitted host instruction *is* the
instruction the oracle would execute.

### Where the run now stops, and why that is not an opcode

The last stop is not another instruction family. `_RtlGetCurrentDirectory_U`
reads `PEB->ProcessParameters->CurrentDirectory` and dereferences `Buffer`;
the gate's process-parameters page is zeroed, so `Buffer` is null and the
dispatcher classifies the read as `memory-bounds` at address `0x7ca` with
width 2. In other words: real ntdll initialization now runs until it needs a
**populated process environment** - the system root, the current directory,
the environment block, the command line - which is the
`wine-process-startup` exit criterion in the foundation ledger, not a gap in
instruction coverage.

The evidence contract reflects that honestly. A `memory-bounds` stop is
accepted by the validator only when the calls record is present, every call
was handled, the `fault` record names a non-zero access of an allowed width,
the fault address lies outside every mapped module (so it is the guest
dereferencing something it was never given, not the guard refusing mapped
memory) and the stop address is inside ntdll. Nothing about it is reported as
acceptance: the verdict still says the run did not reach the Unix-call
boundary.

The stack is the size the process's own image asks for. Windows and Wine size
a process's stack from the PE header's `SizeOfStackReserve`, and a linker that
emits no value leaves 1 MiB; this unit publishes exactly that, rounded up to
the backend's page size and bounded by what the run will honour. Measured: with
the 64 KiB page it used to map, the pinned runtime's own registry startup died
pushing a frame at the stack's lower bound, and with the real size it walks on
through hundreds of thousands of instructions.

### A populated process environment, and the first I/O call

The null `CurrentDirectory.Buffer` was the binding constraint, so the gate now
builds a small but self-consistent `RTL_USER_PROCESS_PARAMETERS` in its page:
`MaximumLength`/`Length`, `CurrentDirectory.DosPath`, `DllPath`,
`ImagePathName`, `CommandLine` and an `Environment` block, all as UTF-16LE
strings (`C:\windows`, `C:\windows\system32`, the root module's path and
`SystemRoot=C:\windows`). `PEB->ProcessParameters` points at the page and
`PEB->ImageBaseAddress` at the root module. It is still not a Windows process
environment - no registry, no NLS data, no drive-letter table - but the fields
the loader asks for are present and self-consistent.

Every one of those strings is published with room for its terminator
(`MaximumLength = Length + 2`, as `RtlInitUnicodeString` defines it), because
ntdll's own `init_user_process_params` copies `MaximumLength` bytes and
terminates what it copied: publishing the two as the same number lets that
terminator land in the string that follows in the same allocation.

The names themselves belong to the root the process's own image came from. A
root opened from `PW_FILE_APPLICATION` publishes `C:\<module>` as its image
path, `C:\` as its current directory and `C:\;` first in its search path; a
root opened from `PW_FILE_RUNTIME` publishes `C:\windows\system32\<module>`
and starts its search path in that directory. The rest of the list is the one
Wine builds for itself (`dlls/ntdll/loader.c:2574`, `get_dll_load_path`) -
`C:\windows\system32;C:\windows\system;C:\windows` - so the loader searches the
directories a Windows loader searches, and this gate answers the two it owns
and refuses the third by the same path rule it applies to every other name.

That closed the architectural gap: the run jumped from 8582 to 11 707 retired
instructions, and three more forms the compiler emits as padding or prefixing
were needed along the way:

- `66 90` (the 16-bit NOP) and `0F 1F /0` (the multi-byte NOP), which compilers
  emit for alignment;
- a **segment override on LEA** (`2E 8D B4 26 ...`), which is exact to ignore
  because LEA never accesses memory.

The gate's translated-code arena was also raised to 4 MiB: ntdll's loader path
translates far more code than a title's startup, and running out of the arena
now has its own classified stop (`cache-limit`) instead of surfacing as a
guest fault. The evidence validator accepts that stop under the same rule as
the others: the calls record must be present and nothing may have been
refused.

The result is the first **Unix call that has no handler**:

```text
retired 11707 instructions over 2252 dispatches and 404 translated blocks
3 NtAllocateVirtualMemory calls serviced, 2 guest regions mapped (88 KiB)
4th call: syscall 0x0033 = NtOpenFile, 24 argument bytes -> unimplemented
stop: unix-call-unimplemented, verdict not accepted
```

So the next work is concrete and architectural rather than an instruction
family: implement the file/open path (`NtOpenFile` and the calls that follow
it) behind the same table, with guest-pointer validation and a documented
mapping onto the gate's read-only runtime directory.

### The first platform service: files

`NtOpenFile` was the first Unix call with no handler, so the bridge now has a
file service below it. The gate owns a bounded handle table and translates the
guest path; the runner owns the host side through a small interface
(`PwWineFileService`: open, read, close), so the portable core still contains
no file-system call.

Handlers and their shape:

```text
NtAllocateVirtualMemory       (0x0018) the first-boot profile's reserve and
                                       commit, bounded per run, with both
                                       output spans preflighted
NtFreeVirtualMemory           (0x001e) a whole block this run mapped, with the
                                       same preflight and the same rollback
NtOpenFile                    (0x0033) OBJECT_ATTRIBUTES + UNICODE_STRING
                                       name, handle and IO_STATUS_BLOCK
                                       written back; a directory of the
                                       runtime namespace becomes a directory
                                       object instead of a platform open
NtReadFile                    (0x0006) handle, IO status, buffer, length,
                                       offset; a directory handle is refused
NtQueryInformationFile        (0x0011) FileStandardInformation only (sizes,
                                       and Directory for a directory object)
NtInitializeNlsFiles          (0x00a4) the runtime namespace's own
                                       locale.nls, asked for the way Wine asks:
                                       the file's bytes are mapped into the
                                       guest below 4 GiB, page-rounded and
                                       declared readable and nothing else, and
                                       the address, the mapping size and the
                                       system language id are written back -
                                       which is what Wine's own map_section
                                       does for this call. A distribution that
                                       does not carry the file answers with the
                                       failure of the open Wine itself falls
                                       back to; the language id is written
                                       either way, as Wine writes it
NtProtectVirtualMemory        (0x0050) a range this run mapped - an NT
                                       allocation or a section view - protected
                                       to PAGE_NOACCESS, READONLY, READWRITE,
                                       EXECUTE, EXECUTE_READ, EXECUTE_READWRITE
                                       or their write-copy forms; the host
                                       mapping and the dispatcher's view change
                                       together, the declared regions are split
                                       around the range so the guard can never
                                       allow a write the host would fault on,
                                       and a range this run did not map is
                                       STATUS_INVALID_PARAMETER. The protection
                                       it reports as the old one is the *union*
                                       of the regions covering the range's first
                                       page, which is what the guard actually
                                       allowed there. With a view declared page
                                       by page that union is exactly the page's
                                       own protection, while a coarse view
                                       declaration made it overlap two regions
                                       and answer "read-only" for a page the
                                       guard allowed writes on - and the loader
                                       relocates an image by protecting pages and
                                       putting that value back, so it then
                                       restored read-only over a writable page
NtMapViewOfSection            (0x0028) the one view a loader maps: the
                                       section placed at its preferred base
                                       when this run can put it there and
                                       elsewhere when it cannot, the file's
                                       headers and each section's raw bytes
                                       where the image's own section table says
                                       they belong, and the protections the
                                       section table implies, declared *page by
                                       page* and applied to the host mapping
                                       from the same map: every page of the
                                       view is readable, plus writable and/or
                                       executable when a section overlapping it
                                       says so (the guard's union rule for the
                                       page two sections share), and a page no
                                       section claims is readable and nothing
                                       else. One map, so the host mapping and
                                       the dispatcher's view of it cannot
                                       disagree about what a page allows: the
                                       guest jumps into a view it mapped itself
                                       (kernelbase's entry points are the first
                                       such jump) and writes into that image's
                                       data (the TLS slot index), and a page the
                                       map calls writable is one the host really
                                       did protect that way. Pages and not
                                       sections, because a section boundary is
                                       where the linker put it and the host
                                       protects whole pages. Nothing is
                                       relocated here, because a SEC_IMAGE view
                                       is the image as the file holds it
NtCreateSection               (0x004a) an unnamed SEC_IMAGE section over an
                                       open file handle, described from the
                                       file's own headers; the section keeps
                                       the canonical name it can re-open the
                                       file by, because the loader closes that
                                       handle as soon as the section exists
NtQuerySection                (0x0051) SectionBasicInformation and
                                       SectionImageInformation, answered from
                                       the section's description - the
                                       transfer address is the image's own base
                                       plus its entry point, as Wine fills it
                                       for a section that is not mapped yet
NtFsControlFile               (0x0039) FSCTL_GET_OBJECT_ID only, answered
                                       with the identity a handle carries: the
                                       SHA-256 of the canonical name the file
                                       service resolved, truncated to the 16
                                       bytes ObjectId has room for, with the
                                       three birth fields zeroed as Wine leaves
                                       them - so the same file answers the same
                                       id, which is what the loader's
                                       deduplication compares
NtQueryVolumeInformationFile  (0x0049) FileFsDeviceInformation only
NtClose                       (0x000f) releases a gate-owned handle
NtFreeVirtualMemory           (0x001e) returns a whole guest block that this
                                       run mapped to the backend; a partial
                                       release answers STATUS_UNABLE_TO_FREE_VM
                                       and MEM_DECOMMIT answers
                                       STATUS_NOT_SUPPORTED, because the
                                       reserve/commit distinction is not
                                       modelled
NtOpenKey                     (0x0012) OBJECT_ATTRIBUTES + UNICODE_STRING key
                                       path, absolute inside the registry
                                       namespace or relative to a key handle
NtOpenKeyEx                   (0x00b6) the same open as NtOpenKey with the
                                       extended form's open options, which
                                       Wine only warns about: REG_OPTION_OPEN_LINK
                                       asks for a link object rather than the
                                       key, and this profile carries none
NtSetValueKey                 (0x0060) one value on a key the run has open:
                                       the name goes through the same
                                       translation a query's does, the data is
                                       read through the validated accessor into
                                       the gate's own buffer before the service
                                       is asked to store it, and a service
                                       without a writable store answers
                                       STATUS_NOT_SUPPORTED rather than
                                       pretending the value is there
NtQueryValueKey               (0x0017) KeyValuePartialInformation only
NtQuerySystemInformation      (0x0036) SystemWineVersionInformation (1000) only
NtCreateKey                   (0x001d) create-or-open against the run's own
                                       registry store: a path the store already
                                       holds answers REG_OPENED_EXISTING_KEY and
                                       one it does not is added and answers
                                       REG_CREATED_NEW_KEY, because a runtime
                                       that creates its user hive and is then
                                       refused the keys it created never stops
                                       asking for them
NtQueryInformationToken       (0x0021) TokenUser for the current-token
                                       pseudo-handles only, answering with the
                                       SID the host declares
NtOpenDirectoryObject         (0x0058) a directory in the object namespace
NtOpenSection                 (0x0037) a section by absolute name or relative
                                       to a directory object the gate owns
NtQueryInformationProcess     (0x0019) ProcessImageInformation only, answered
                                       from the process module's own PE headers
NtQueryVirtualMemory          (0x0023) MemoryBasicInformation only, answered
                                       from this run's own mappings: a module
                                       image reports its base, extent and the
                                       protection of the section the page falls
                                       in with MEM_IMAGE, every other declared
                                       region reports MEM_PRIVATE, and a page
                                       this run never mapped is refused instead
                                       of being described as MEM_FREE
NtAreMappedFilesTheSame       (0x0072) two guest addresses, answered from the
                                       views this run owns: the images the
                                       guest mapped itself and the images this
                                       gate mapped before the run. Two
                                       addresses in one view, or in two views
                                       of the same canonical file in the same
                                       root, are the same file; a page this
                                       run mapped that is not a file view is
                                       STATUS_CONFLICTING_ADDRESSES, and an
                                       address this run never mapped is
                                       refused rather than guessed at, because
                                       the rest of the address space is not
                                       modelled here
NtGetNextThread               (0x00a0) the next thread of this run's own list,
                                       which holds the one thread the run
                                       started: a handle to it, and
                                       STATUS_NO_MORE_ENTRIES with a null
                                       handle at the end. A handle this run did
                                       not hand out, another process, an
                                       attribute or a flag it cannot honour,
                                       and an access it cannot grant are each
                                       refused with their own status
NtQueryInformationThread      (0x0025) ThreadBasicInformation only:
                                       TebBaseAddress is the TEB this run
                                       published (the one the guest runs on
                                       through FS), ClientId is the pair this
                                       run names for itself, and the exit
                                       status of a thread that has not exited
                                       is STATUS_PENDING. Another class is
                                       refused, and a buffer shorter than the
                                       structure is reported before anything is
                                       written
NtContinue                    (0x0043) the state a thread resumes at, read out
                                       of the guest's own I386_CONTEXT (0x2cc
                                       bytes) and installed whole: Eax..Edi,
                                       Eip, Esp and Eflags, plus the x87
                                       control word and MXCSR. The call does
                                       not return to its stub - the
                                       instruction pointer and the stack come
                                       from the context - and the run carries
                                       on wherever it said. A context that does
                                       not ask for the integer and control
                                       groups, or that names a code or stack
                                       segment other than the user ones this
                                       run publishes (0x1b/0x23), is refused
                                       with STATUS_INVALID_PARAMETER rather
                                       than run in a segment it cannot
                                       describe; the x87 register file and the
                                       XMM file are the run's own FP state and
                                       are not rebuilt from the context
NtTerminateThread             (0x0053) a stop, like NtTerminateProcess and
                                       next to it in the dispatcher: a thread
                                       that has ended does not keep executing,
                                       and this run models one thread, so
                                       terminating it ends the process. That is
                                       how a program's own clean exit arrives -
                                       its entry point returns, kernel32's
                                       BaseThreadInitThunk passes the value to
                                       RtlExitUserThread, and that calls
                                       NtTerminateThread( GetCurrentThread(),
                                       status ) - and the status travels in the
                                       evidence as the exit code. Any other
                                       handle is STATUS_INVALID_HANDLE
NtQueryDefaultLocale          (0x0015) the user's locale or the system's, both
                                       of them the one locale this run models
                                       (MAKELANGID( LANG_ENGLISH,
                                       SUBLANG_DEFAULT )), written through the
                                       caller's pointer
NtQueryDefaultUILanguage      (0x0044) the same language id, for the user
                                       interface
NtGetNlsSectionPtr            (0x00a1) the rest of the NLS tables by type and
                                       id, the way Wine's own locale code asks
                                       for them (dlls/ntdll/unix/env.c:93):
                                       sort keys (9), the case map (10), the
                                       codepage tables (11) and the
                                       normalization forms (12), each mapped
                                       out of the file the distribution
                                       carries, with the open's own failure
                                       for a file it does not
NtQueryInstallUILanguage      (0x00c7) the installation's language, again the
                                       one locale this run models
NtTerminateProcess            (0x002c) the current process only; the run stops
                                       with a classified stop instead of
                                       pretending a terminated process runs on
```

Rules the gate enforces before the platform is ever asked:

- a name belongs to exactly one of two roots, and the path decides which:
  `C:\windows\system32\<name>` and `C:\windows\<name>` are the **runtime**
  distribution's, and `<one component>` directly under `C:\` (with the bare
  root naming that directory itself) is the **application's** own. Both accept
  an optional `\??\` prefix, and the prefix only counts when a separator or the
  end of the string follows it, so `C:\windowsfoo` is a component of the root
  and not the Windows directory, exactly as Windows reads it. Neither root
  falls back into the other: the name decides the root, and the service is
  handed the pair, so an application module can never be answered with a
  runtime file that happens to share its name or the other way round;
- the remainder must be a single path component, lower-cased, so `..`, a
  separator, a drive letter or an absolute host path cannot reach the service;
- every guest pointer (the OBJECT_ATTRIBUTES, the UNICODE_STRING, its buffer,
  the handle slot, the IO status block, the read buffer) is read or written
  through the dispatcher's validated accessor;
- reads are bounded per call, handles are gate-owned indices (`0x100 + slot`),
  and any handle the gate does not own answers `STATUS_INVALID_HANDLE`.

Everything else is answered with a real NTSTATUS - a name outside the runtime
namespace gives `STATUS_OBJECT_NAME_NOT_FOUND`, a value error gives
`STATUS_INVALID_PARAMETER`, an unsupported information class gives
`STATUS_INVALID_INFO_CLASS` - because that is the contract the guest expects.
An open file handle keeps both halves of its identity, the canonical name and
the root it was opened from, because the loader closes its own handle before
the image view is mapped and the section re-opens the file by that pair.
Handles are released at cleanup, and the counters (`opens`, `reads`, `bytes`,
`closes`, `directories`, `refusals`, `last`) are part of the evidence.
`opens` counts every `NtOpenFile` the gate answered with a handle - file or
directory - and `directories` how many of those were the gate-owned Windows
directory, so a directory open can never be mistaken for a platform file.

`tests/test_pw_wine_file_service.c` proves the whole path without a real file:
a synthetic module builds a UNICODE_STRING and an OBJECT_ATTRIBUTES for
`C:\windows\system32\test.dll` in its own data section (with the base
relocations a real image has), calls NtOpenFile, reads the file into a guest
buffer, closes the handle and then attempts
`C:\windows\system32\..\..\etc`. The first three calls succeed, the guest
observes the handle and the IO status blocks, and the escaping path is refused
*by the gate* - the platform service is never asked to open it.

On the real runtime the loader's first open is the Windows **directory**
itself:

```text
4th call: NtOpenFile "\??\C:\windows" -> STATUS_OBJECT_NAME_NOT_FOUND
retired 14918 instructions, 3041 dispatches, 580 translated blocks
stop: returned-to-caller at EIP 0 (the loader's failure path jumps to null)
```

### The directory object, and the question the loader asks about it

That open is a directory, not a file, so the gate now owns directory objects
too. `C:\windows` and `C:\windows\system32` (with or without the `\??\`
prefix) are accepted by `NtOpenFile` and answered with a gate-owned handle;
there is no platform token behind it, because the loader only needs the object
to exist. It queries `FileStandardInformation` - the gate reports
`Directory = 1` with zero sizes, and `NtReadFile` on that handle is refused
with `STATUS_INVALID_DEVICE_REQUEST` - and then
`NtQueryVolumeInformationFile` with `FileFsDeviceInformation`, which is what
`RtlSetCurrentDirectory_U` uses to decide whether the current directory lives
on removable media: a `FILE_REMOVABLE_MEDIA` characteristic would make it
close the handle again. The runtime distribution is a directory tree on a
fixed disk, so the answer is `FILE_DEVICE_DISK_FILE_SYSTEM` with no
characteristics. The device type is a property of this profile, not a host
probe, and every other volume information class is answered with
`STATUS_INVALID_INFO_CLASS`; a buffer shorter than the 8-byte structure gets
`STATUS_BUFFER_TOO_SMALL` rather than a silently truncated success.

The call table itself was extended at the same time. It is still generated
from the pinned `ntsyscalls.h`, and it now covers all 256 i386 numbers, so a
number the loader reaches is recognized and named in the evidence instead of
being reported as unknown; `tests/test_unix_call_table.py` re-derives every
entry (number, name and stdcall argument width) from the pinned Wine revision
and fails on any difference.

Measured on the same pinned runtime with `--bridge 1`:

```text
retired 18386 instructions over 3821 dispatches and 721 translated blocks
6 calls handled: 3 NtAllocateVirtualMemory, NtOpenFile "\??\C:\windows",
  NtQueryVolumeInformationFile FileFsDeviceInformation on that directory,
  and NtFreeVirtualMemory MEM_RELEASE of the process-parameters block
2 guest regions mapped (88 KiB) and 1 released again
7th call: syscall 0x0012 = NtOpenKey, 12 argument bytes
stop: unix-call-unimplemented, verdict not accepted
```

That release is worth naming precisely, because it is the loader's own
initialization doing it. `init_user_process_params` builds its own copy of the
process environment, points `PEB->ProcessParameters` at the copy, and releases
the block it was handed. In a real process that block was allocated by the
parent through `NtAllocateVirtualMemory`, so the gate registers its
process-parameters page as one of this run's guest regions: the release finds
it, the page leaves the dispatcher's declared ranges, and the mapping goes
back to the backend. It is *not* counted against the guest's live bytes,
because the guest never allocated it - the gate is playing the parent.

`NtFreeVirtualMemory` itself is deliberately narrow. It can honour the release
of a whole region this run mapped, which is the form both a loader and a heap
use; a size that does not cover the whole region is answered with
`STATUS_UNABLE_TO_FREE_VM` rather than a partial unmap the dispatcher cannot
represent, and `MEM_DECOMMIT` with `STATUS_NOT_SUPPORTED` because the one kind
of guest region this bridge knows has no committed/decommitted distinction.
`tests/test_pw_wine_gate_bridge.c` proves the successful path from the guest:
it allocates, sets the size to zero, releases with `MEM_RELEASE`, and then
hands both the base the allocation wrote back and the size the release wrote
back to a call with no handler, so the recorded arguments show the guest read
both write-backs out of its own memory. It also checks that the block stops
being one of the run's live regions and is not released a second time at
cleanup.

A directory object is not a file-system implementation: there is no
enumeration and no `NtQueryDirectoryFile`, and the individual DLLs the loader
opens later still go through the same single-component translation onto the
read-only runtime directory.

### The token, the user key, and two instruction families

`version_init` opens HKCU through `RtlOpenCurrentUser`, which first asks for
the process token's user SID and then formats `\Registry\User\<SID>` and opens
that path with `NtCreateKey`. Both halves are now serviced:

- `NtQueryInformationToken` answers `TokenUser` for the current-token
  pseudo-handles (`~(ULONG_PTR)3`, `4` and `5` - Wine's
  `GetCurrentProcessToken`, `GetCurrentThreadToken` and
  `GetCurrentThreadEffectiveToken`), and for nothing else. The answer is laid
  out the way NT lays it out: a `TOKEN_USER` descriptor whose `Sid` names the
  guest address immediately after it, followed by the SID itself, with the
  required length reported through `ReturnLength` and
  `STATUS_BUFFER_TOO_SMALL` for a buffer that cannot hold it. The SID comes
  from the host service - `S-1-5-21-0-0-0-1000`, the local user SID Wine's own
  server uses (`server/token.c`, `local_user_sid`, at the pinned revision) -
  so the identity the guest formats and reports is the one this distribution
  declares, not a compiled-in guess.
- `NtCreateKey` resolves the path exactly as `NtOpenKey` does and asks the
  profile. This distribution has no writable hive yet, so it opens the user
  hive root it declares and reports `REG_OPENED_EXISTING_KEY`; a key it does
  not declare still answers `STATUS_OBJECT_NAME_NOT_FOUND`. The user root was
  added to the profile for exactly this path.

Getting there needed two instruction families, and the first of them found a
real defect in the translator:

- **BSWAP r32** (`0f c8+rd`): the register is named by the opcode byte, there
  is no ModRM operand and no flag is written. The 16-bit encoding is
  architecturally undefined and stays refused, as do the 0x66-prefixed forms
  generally.
- **the packed-integer arithmetic, comparison and shift slice** in its
  `xmm <- <op> xmm/m128` form (`66 0f 60..6d`, `74..76`, `d1..d5`, `d8..df`,
  `e0..e6`, `e8..ef`, `f1..f6`, `f8..fe`) plus **pmovmskb** (`66 0f d7`). The
  host executes the same instruction on host XMM scratch, so lane widths,
  saturation and comparison semantics come from the CPU. The opcodes that
  *store* rather than read-modify-write - `0x7e`/`0x7f` as their own kinds,
  and `0xd6`, `0xe7` (movntdq) and `0xf7` (maskmovdqu, which writes through a
  mask) - are outside those ranges and stay refused rather than being misread.

The defect is worth naming precisely. The memory form of `movd` **to** memory
(`66 0f 7e /r` with a memory destination) emitted "compute the effective
address, then move the value to `eax`, then store `eax`" - but computing the
effective address already leaves it in `eax`, so the store wrote the *address*
instead of the value. Nothing had exercised that form before: the existing
tests used the register-to-register encoding. Wine's
`RtlFormatCurrentUserKeyPath` uses exactly `movd %xmm0,(%ebx)` to widen both
16-bit halves of a `UNICODE_STRING`'s length dword, so the guest was handed a
path whose length field was a pointer, and the SID path could not be opened.
The store now goes straight from host `xmm0` to guest memory, and the unit
test exercises the exact idiom (`movd` from memory, `paddw` with a constant
loaded from guest memory, `movd` back to memory) as well as through all four
residency/lazy-flag combinations.

Measured on the same pinned runtime with `--bridge 1`:

```text
retired 29336 instructions over 5987 dispatches and 890 translated blocks
17 calls handled: the allocations, the directory open and its device query,
  the release of the parameters block, the Wine version, the registry open
  and its two option queries, NtClose of that key, the token query, the
  user-key create, and the version-init registry reads
cleanup modules=2 pending_modules=0 pending_pages=0 pending_regions=0 mappings=7 translations=1 pending_translations=0 failures=0 status=ok
stop: unix-call-unimplemented, syscall 0x0058 = NtOpenDirectoryObject
```

The next gap is `\KnownDlls`: the loader maps the system DLLs from the object
directory `\KnownDlls` before it loads anything by name.

### The object namespace, and Wine's second stub shape

`LdrInitializeThunk` opens `\KnownDlls` once (`open_known_dll_ntdir`) and keeps
the handle, then looks for a section in it for every DLL it loads
(`open_known_dll`: a relative name against that handle). Both are serviced
now, behind a fourth host service:

```text
NtOpenDirectoryObject  (0x0058) a directory object
NtOpenSection          (0x0037) a section object
```

The rules are the same shape as the registry's: an absolute path starts with
`\` and every component is validated (printable ASCII, no separator, no
colon, no `.` or `..`), a relative name is resolved against the canonical path
stored with the directory handle and then revalidated as a whole, and only a
directory handle can be a parent. The distribution's profile declares
`\KnownDlls` and nothing else - no section objects - which is the honest
answer for a distribution that carries PE *files*: every section lookup is
`STATUS_OBJECT_NAME_NOT_FOUND`, and the loader falls back to loading the
module from the file system, exactly as it does on a prefix without known
DLLs.

The interesting part was the next call. The run stopped with
`unsupported-instruction` *inside* a stub, on `64 ff 15 c0 00 00 00`:
`call dword ptr fs:[0xc0]`. Wine's `ntdll.spec` declares a handful of
functions with `-syscall=<id>` - `NtQueryInformationProcess` among them - and
the build emits a different stub for those:

```text
most syscalls:  mov eax, <id>; mov edx, <dispatcher thunk>; call edx; ret n
-syscall ones:  mov eax, <id>; call dword ptr fs:[0xc0];        ret n
```

`fs:[0xc0]` is `TEB.WOW32Reserved`, and it is where Wine's *unix* side
installs the dispatcher (`teb->WOW32Reserved = __wine_syscall_dispatcher` in
`signal_i386.c`). The gate plays that role now: it writes the very thunk it
located structurally into the TEB it builds, and it translates the
FS-relative indirect call - target read from the guest's own FS block, guest
return address pushed exactly as a register-indirect call pushes it, block
ended at the target. Both stub shapes therefore reach the same boundary and
are bridged identically, and the evidence shows which one a call came from
because the issuing stub is re-read inside ntdll as always.

Translating that call also needed one fix in the emitter: the fall-through
store of the next EIP ran for every opcode except the handful of terminal
ones, and it overwrote the target the call had just loaded. A call through
memory is terminal like any other control transfer, so `fs_call` is excluded
from that store - and the unit test now executes the exact stub body, checks
that the target is in EIP and that the guest return address is on the guest
stack.

Measured on the same pinned runtime with `--bridge 1`:

```text
retired 29359 instructions over 5991 dispatches and 894 translated blocks
18 calls handled, including NtOpenDirectoryObject("\KnownDlls") -> SUCCESS
cleanup modules=2 pending_modules=0 pending_pages=0 pending_regions=0 mappings=7 translations=1 pending_translations=0 failures=0 status=ok
stop: unix-call-unimplemented, syscall 0x0019 = NtQueryInformationProcess
```

That last call is the next gap, and it is a real one: it is the loader asking
about the process it is initializing (`ProcessBasicInformation` and friends),
not an instruction the translator lacks.

### The registry, and the version ntdll asks about

The next stop was the registry: `load_global_options` opens
`\Registry\Machine\System\CurrentControlSet\Control\Session Manager` and reads
two option values from it, and `version_init` reads the Windows version keys.
A registry is host state in this architecture, so the bridge gets a third
platform service next to the file and device ones, with the same shape: the
gate translates and validates, the host service decides what exists.

```text
NtOpenKey       (0x0012) OBJECT_ATTRIBUTES + UNICODE_STRING name, absolute
                         under \Registry\Machine or \Registry\User, or
                         relative to a key handle the gate already owns
NtQueryValueKey (0x0017) KeyValuePartialInformation only, with the fixed
                         part, the data that fits and ResultLength written
                         the way Wine writes them
```

Rules the gate enforces before the service is asked anything:

- the accepted namespace is exactly `\Registry\Machine` and `\Registry\User`;
  anything else is `STATUS_OBJECT_NAME_INVALID`;
- every component is a name a canonical string can carry - no control
  character, no separator, no colon and no `.` or `..` component, so a
  traversal cannot name a different key. Units outside ASCII are carried as
  their UTF-8 encoding, with surrogate pairs combined, because a registry name
  in Wine is whatever the guest wrote: kernelbase keeps its locale cache in a
  subkey literally named 🌎🌏🌍 (`dlls/kernelbase/locale.c:54`, `world_subkey`),
  and a name this gate could not represent was a key the runtime could not
  create. A lone surrogate, an unpaired unit or a name too long for the
  destination is `STATUS_OBJECT_NAME_INVALID`;
- a relative name is resolved against the canonical path stored with the key
  handle and then revalidated as a whole, which is how `RtlOpenCurrentUser`'s
  handle plus `Software\Wine` becomes one canonical path;
- a key the profile does not declare is `STATUS_OBJECT_NAME_NOT_FOUND`, so
  ntdll keeps its own defaults instead of being handed invented content;
- a value query answers `STATUS_BUFFER_TOO_SMALL` when the buffer cannot hold
  the fixed part, `STATUS_BUFFER_OVERFLOW` when it cannot hold the whole value
  (the informational status Wine itself returns), and only ever copies bytes
  the host service owns.

The distribution's own profile is one key - the Session Manager key ntdll
opens unconditionally - with the values Wine's initial registry
(`loader/wine.inf` at the pinned revision) gives it. The two search-mode
options the loader asks for are deliberately *absent*: Wine keeps its own
compiled-in defaults then, exactly as it does on a prefix without them.

Two things had to be answered before the registry call arrived. The first is
numeric: `SystemWineVersionInformation` (1000), the Wine extension
`version_init` stores so that `wine_get_version`, `wine_get_build_id` and
`wine_get_host_version` can read it. The gate answers it with the four
NUL-terminated strings Wine packs together, and the host derives them from the
staged distribution's own manifest, so the guest is told the version of the
modules it is actually executing rather than a compiled-in constant. A short
buffer is `STATUS_INFO_LENGTH_MISMATCH` and any other class is
`STATUS_INVALID_CLASS`, as in Wine. The second is a detail of the evidence:
registry key names contain spaces ("Session Manager", "Windows NT"), and the
transcript is a space-separated field list, so a recorded path is printed with
spaces escaped as `%20`.

Measured on the same pinned runtime with `--bridge 1`:

```text
retired 18808 instructions over 3908 dispatches and 746 translated blocks
11 calls handled: the 3 allocations, the directory open, its
  FileFsDeviceInformation, the release of the parameters block, the
  registry open, two value queries that correctly answer
  STATUS_OBJECT_NAME_NOT_FOUND, NtClose of the key, and the Wine version
stop: unix-call-unimplemented, syscall 0x0021 = NtQueryInformationToken
```

The next gap is the process token: `RtlOpenCurrentUser` formats
`\Registry\User\<SID>` from the token's user SID, so ntdll asks
`NtQueryInformationToken` for `TokenUser` - and on this run that call is the
one with no handler. `tests/test_pw_wine_registry.c` proves both new services
without a Wine artifact: a synthetic caller asks for the Wine version, opens
the Session Manager key, queries a value that exists, one that does not, one
that does not fit twice (too small for the header, too small for the value),
an information class with no meaning here and a value name that is not a
name; then opens the registry root and a key relative to it; closes every
handle; and hands what it read back to a call with no handler, so the
recorded arguments are the guest's own memory.

### Historical checkpoint: the process image and second dispatcher

The ntdll initialization path then asks a different question, and the answer is
honest rather than convenient. `build_main_module` calls
`NtQueryInformationProcess( ProcessImageInformation )` to decide whether the
module the process was handed is an executable; the gate answers it out of
**that module's own PE headers** - the transfer address is its entry point, and
the stack sizes, subsystem and versions, characteristics, DLL characteristics,
machine, checksum, file size and contains-code flag come from its file and
optional headers.

The module the gate presents as the process image is `kernelbase.dll`, whose
characteristics (`0x2106`) include `IMAGE_FILE_DLL`. So ntdll prints
`wine: ... is a dll, not an executable` and calls `NtTerminateProcess`. The
bridge services that for the current process: `STATUS_SUCCESS`, and the run
stops with a classified `process-terminated` stop, because a process that has
terminated does not keep executing and the evidence should say so rather than
invent a stop.

Before the termination is reached, the run hits the next real frontier, and it
is architectural. That `MESSAGE` goes through `__wine_dbg_output`, which calls
the **unix-call dispatcher**: `__wine_unix_call_dispatcher`, a data export at
RVA `0x0af034`, immediately next to the `__wine_syscall_dispatcher` slot at
`0x0af030` that the gate already locates structurally. The unix side installs
both; the gate only ever published the first, so the guest's call lands on
address zero and the run stops with `returned-to-caller` at 0.

That is the *unix-call* half of the platform layer this architecture names -
`__wine_unix_call( unixlib_handle, code, args )`, with `__wine_unixlib_handle`
alongside - and it is the honest next step rather than one more syscall:
Wine's PE modules reach the host through it for debug output, server calls and
the rest of the unixlib surface.

At that earlier checkpoint, measured on the same pinned runtime with
`--bridge 1`:

```text
retired 32544 instructions over 6869 dispatches and 968 translated blocks
19 calls handled; the last is NtQueryInformationProcess ProcessImageInformation
  -> SUCCESS with image_characteristics=0x2106 (kernelbase's own)
cleanup modules=2 pending_modules=0 pending_pages=0 pending_regions=0 mappings=7 translations=1 pending_translations=0 failures=0 status=ok
stop: returned-to-caller at 0 - the unix-call dispatcher was not yet published
```

### The loader's own start-up, and the argument it takes

The application-root scenario - a generated PE32 executable with two DLLs and
their dependency diamond, loaded by the pinned runtime's own ntdll - now runs
the loader's start-up to its end. Four things had to be right for that, and
each one is named by the stop that preceded it.

**Registry names outside ASCII.** The stop was a *rejected* `NtCreateKey`:
kernelbase keeps its locale cache in the subkey named 🌎🌏🌍, the gate's
registry names were ASCII, and a name it could not represent was a key the
runtime could not create. Non-ASCII units are carried as UTF-8 now, with
surrogate pairs combined, so that call is served - and the name rules above
are stated for what a name may be rather than for one character set.

**One permission map, per page.** A section view was declared to the guest as
three regions - the image readable, the union of its executable sections
executable and the union of its writable sections writable - while the host
mapping was protected section by section. The two disagreed, and the
disagreement was a host crash rather than a classified stop: with a writable
section above it, a read-only page was declared writable, and the guard let a
store through that the host mapping refused. The view is declared *page by
page* from the image's own section table now - read, plus write and/or execute
for a section overlapping the page, which is the guard's own union rule for a
shared page - and the same map is what the backend is asked to protect, so the
two cannot disagree. Pages and not sections, because a section boundary is
where the linker put it (the fixtures' text section ends at 0x1600) while the
host protects whole pages. The declared region table grew with it: 256 entries
rather than 64, and the generated guard compares the count against that bound
with a 32-bit immediate, because the signed imm8 it used to emit would have
made a bound above 127 negative and every access out of bounds.

**The context the initialization entry takes.** `LdrInitializeThunk` receives
a `CONTEXT*` - the register state the kernel builds at the top of the thread's
initial stack - not a PEB. It hands it to `loader_init`, which uses a *null*
start routine to publish the process image's own entry point into it
(`dlls/ntdll/loader.c:4527`, which is how Wine starts its first thread), and
then to `signal_start_thread`, which clears the 0xf000 bytes of stack below it
and enters the thread through `NtContinue`
(`dlls/ntdll/signal_i386.c:514`). Passing the PEB made the loader write the
entry point into the middle of the PEB and made that clear run down from
0x0d000000 into memory nothing had mapped - a bounds fault at the last
instructions of ntdll's own start-up, with nothing left to blame it on. The
gate builds the context now, one page below the stack's top so the entry's own
argument frame keeps the bytes it uses.

**The fixture's TLS callback entries.** A PE image's callback array holds
virtual addresses the loader *calls*, so a real linker relocates each entry.
The generator relocated the directory's `AddressOfIndex` and
`AddressOfCallBacks` fields but not the entries themselves, so a run placed
away from the preferred base took a.dll's own preferred-base address
(0x10101010) as a function pointer and stopped as `non-code` at an address no
mapping covered.

Measured on the pinned runtime with the generated diamond:

| configuration | stop | retired | blocks |
| --- | --- | --- | --- |
| control | returned-to-caller at 0 | 33 367 | 962 |
| residency off | `process-terminated`, `NtTerminateThread (0x0053)`, status `1` | 598 404 | 2 981 |
| residency on | `process-terminated`, `NtTerminateThread (0x0053)`, status `1` | 598 404 | 2 981 |

`NtContinue(context, TRUE)` installs the guest's integer/control state without
applying the dispatcher's synthesized return. Wine's `RtlUserThreadStart` then
calls the generated application's transfer address. The entry returns `1`,
kernel32 passes that value into `RtlExitUserThread`, and the process ends through
`NtTerminateThread` with the same status. `tests/test_pw_wine_continue.c` pins
context validation, state installation and the termination path; the host gate
pins the complete application result above.

This does not close the Wine-process milestone. Before hardware staging, the
Wine-owned loader graph must be read back and validated and DllMain/TLS callback
ordering must become evidence. General thread/object/wait semantics and
persistent prefix storage remain later roadmap items.
