# Hardware WoW64 backend bring-up

This independent `wow64native.dll` backend derives from Wine's `wow64cpu`
revision `490f6d5dcbb2a5047345b8af88d114bbcaad69a8`. It retains the hardware
32-bit BOP and context paths and uses the console's GDT selectors.
`wowprospero.dll` remains unchanged and remains the fallback.

`tools/build_wow64native.sh` accepts `--source`, `--build`, and `--output` for
the pinned host Wine source/build and an independent output directory. Add all
five of `--ps5-sdk`, `--ps5-wine`, `--ps5-ntdll`, `--ps5-foundation`, and
`--ps5-stubs` to compile, link and sign the Unix PRX too. The PS5 Wine directory
must contain its prepared `source` and `build` directories; the ntdll input is
its linked shared ELF. The foundation supplies the PRX tool and linker script.
Use title import stubs without WebKit libraries. This command deploys nothing.

The backend requires ntdll's optional Unix export
`__wine_prospero_native_wow64_caps(unsigned version)`, queried with version 1.
Its required bits are `1` for installed title-safe raw signal handlers, `2` for
host-FS restoration before signal C, and `4` for corrected signal FP/xstate
handling. The query must return zero for an unsupported protocol. Missing or
partial capabilities leave initialization at `STATUS_NOT_SUPPORTED`; ordinary
host builds always refuse native console execution. The ntdll integration is
still pending, so this build does not establish a native Wine application or
FPS result.

`BTCpuThreadInit` captures host FS and associates the low guest TEB with
Unix-owned thread-local state. `BTCpuThreadTerm` clears the current thread's
state. Thread readiness follows the process signal gate; neither function
changes FS. `BTCpuSimulate` checks process and
thread initialization before native execution: the pinned Wine caller ignores
the status returned by `BTCpuProcessInit`, so that status alone is insufficient
to disable a partially implemented backend.

Both BOP entry paths now restore host FS before calling Wine, and both fast
returns restore guest FS before changing stacks and far-jumping. The reset-state
path restores guest FS after loading its segment selector and before `iretq`.
These calls use an assembly wrapper for libkernel's Unix ABI, preserving the
guest registers, flags and x87/SSE state. The new R15 state register has an
unwind save in the matching simulation/BOP frames.

The optional `--wine-build` argument to `tests/test_wow64native_scaffold.py`
executes the actual PE wrappers using a diagnostic export table and a syscall
callback that clobbers registers and FP state. It also verifies that a failed
switch attempts host-FS restoration and terminates instead of returning to the
guest. This fixture changes no real FS base and executes no 32-bit code.
The wrappers and simulation preparation also preserve all sixteen upper YMM
halves when the OS enables AVX. Initialization accepts only the x87/SSE/AVX
xstate components; an enabled component we do not preserve makes initialization
fail. The Wine fixture clobbers all YMM registers and compares their complete
contents for both wrappers. Signal/exception xstate and platform fault recovery
remain unfinished.

With `--wine-build`, the test also compiles the actual console initialization
branch against host mocks for only the external readiness query and FS syscall.
It checks missing/partial capabilities, FS-capture failure, per-thread state and
readiness revocation without setting FS or entering compatibility mode. Use
`--wine-source` if Wine's headers are not in a sibling `source` directory.

P1 requires per-thread host FS capture, a 32-bit TEB base, restoring host FS
before both service calls, guest FS restoration immediately before re-entry,
valid low stacks/thunk addresses, and fault/suspend/unwind coverage. A mode-switch
microbenchmark excludes the TLS switching cost of these service transitions.
The launcher selects it per game through `WINE_PS5_WOW64_CPU` (Wine patch 0611)
rather than the prefix's `HKLM\Software\Microsoft\Wow64\x86`, which keeps
naming the translator and is used whenever `wow64native.dll` cannot be loaded.
32-bit games run on it by default with the Vulkan batching; OpenGL games and
profiles with `[runtime] cpu = translator` keep the translator
(docs/WINE_PS5_BUILD.md, game profiles).

The signal entry must restore host FS before any Wine C or Unix TLS access.
An ELF probe that reaches a handler with guest FS does not establish that the
same registration is permitted in a title, or that signal return restores the
guest correctly. Acceptance must cover repeated delivery and return with guest
FS, the compatibility-mode CS and a valid low stack, plus Wine's own exception
and thread-suspension paths. Do not store host TLS pointers in the guest TEB's
fiber or arbitrary-user-pointer fields: ordinary DLL loading and fibers use them.

The first application checks must observe CS `0x33` in the guest, read the TEB
directly before and after service calls, and verify the selected native backend.
A host Wine pass only validates the fixture. Follow with finite worker threads
and actual access-violation/illegal-instruction recovery through Win32 handlers
before Minesweeper and the game sequence.


Opt-in transition diagnostics use ABI version 3 (32-byte thread state).
Set `PW_NATIVE_PROFILE=1` in the title environment to enable per-thread
host/guest FS-switch counts and serialized TSC ticks around the `sysarch`
call, plus aggregate Unix-call and syscall entry counts. Other values leave
the profile pointer NULL. Rebuild the PE and Unix backend together; version 2
modules cannot participate in the new contract.

`PW_NATIVE_PROFILE version=1` reports cumulative counters and the raw TSC
at the first host-FS switch, every 262144 host switches thereafter, and on
thread cleanup. The report runs only with host FS restored, inside the
existing full register/flags/x87/SSE/YMM save. Guest-FS restoration never
calls the reporter. The tick measurement excludes FP saving/restoring and
logging; it includes timestamp/branch overhead and interruptions. Counter
intervals must be aligned to actual gameplay and a console TSC calibration.
Compare profile-off/on runs before attributing FPS changes. Counters are not
per-opcode attribution or proof that FS alone explains a regression. No
WRFSBASE fast path is enabled by these diagnostics.
