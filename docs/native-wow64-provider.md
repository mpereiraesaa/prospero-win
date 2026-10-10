# Native WoW64 signal provider

Patch 0883 installs PS5 signal entries that recover the host FS base from
an aligned alternate signal stack before entering Wine C handlers. They are
registered with the kernel's own `sigaction`, not libkernel's wrapper (which
enters handlers through code that needs the host TLS). libkernel exports no
raw `sigaction`, so the patch runs system call 416 through the `syscall`
instruction of an exported plain stub: libkernel's stubs are
`mov $N,%rax; mov %rcx,%r10; syscall; jb error; ret`, so `getppid + 10` is a
syscall instruction inside libkernel, the only place the kernel accepts one.
Nothing depends on the firmware's libkernel build. Before using it the
patch proves the instruction runs whatever `%rax` names (getpid and getppid
through the same instruction must return the process's two IDs) and that a
raw query reports a handler inside libkernel's executable segment, as the
kernel sees the wrapper's registrations.

The signal frame layout (FXSAVE at `ucontext+320`, FS base at `+1152`) was
measured on one firmware, so the patch then takes one SIGUSR2 through a raw
registration with a marked MXCSR and checks both values in the frame. When
any check fails, the imported signal path is kept, native execution is not
advertised and the game runs on the translator; a frame mismatch logs
`PW_NATIVE_FRAME unverified`.

The optional `__wine_prospero_native_wow64_caps(1)` export reports zero until
signal registration and the standard x87/SSE/AVX layout checks pass. The
native backend requires all three capability bits. Existing translated-code
SIGSEGV hooks remain available for wowprospero.

Run `python3 tests/test_native_wow64_provider.py` for the host resolver
fixture. Against a model of the kernel it accepts the stub on any segment
layout or module fingerprint and rejects malformed records, other modules
(such as the ELF loader's libkernel), wrong stub offsets and registrations
outside libkernel. It does not prove kernel signal delivery or guest
execution.

On the owner's console (2026-10-10, its libkernel matches the layout the
first version of this patch was pinned to), getpid, getppid and getuid ran
through each other's stubs' syscall instructions; sigaction queries through
`getppid + 10` matched those of the formerly pinned raw entry, a
handler registered through it received a signal with the frame values
above, and Pinball ran on the native CPU with capabilities 7 for the whole
run, as with the pinned build.

The full patched Wine pipeline builds all PRXs without unresolved imports.
On a physical console, the pinned version's freshly built ntdll passed simultaneous native
Win32 worker TLS/TEB/thread-ID checks, native Minesweeper with scripted input,
access-violation/UD2 resumption with x87/XMM0/YMM0, and a DBT Minesweeper
fallback control using that same provider. Each run exited normally with
capabilities 7 and restored and verified the original console files.

An earlier private provider also passed native and DBT Space Cadet gameplay
and audio. Those results do not establish Space Cadet correctness for the
fresh full-build provider. HL2, GTA SA, GTA IV, DXVK/d3dx9 performance and DBT
retirement remain open. The backend stays opt-in through the Wow64 registry
selection; wowprospero remains available as the fallback.
