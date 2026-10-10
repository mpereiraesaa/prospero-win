# Native system-service diagnostic

Set `PW_NATIVE_SYSCALL_PROFILE=1` for a diagnostic run. The default is off.
The 64-bit WoW64 dispatcher records guest 32-bit system-service entry IDs,
not raw kernel calls. This covers both ntdll and win32u service tables;
ntdll names are emitted directly, other IDs require the matching generated
service table. The syscall dispatch and result remain unchanged.

Each thread emits cumulative totals and every nonzero interval counter after
16,384 entries. A partial last interval is not reported. The bounded table
has 64 thread-state slots; reused TEB addresses share that slot, so thread
identity must be verified from the same process log. Allocation failures and
threads beyond the bound are unprofiled. Profiling storage lives until process
exit and must be disabled during final performance measurements.

For NtProtectVirtualMemory, the caller return address is read from the
compatibility-mode stack passed by the CPU backend. Up to 64 addresses are
recorded per thread; additional addresses contribute to `other`. Resolve
addresses using that process's module-load evidence. A caller address names
a return location, not a complete stack trace.

Counts include entries satisfied in the 64-bit PE layer without a host call.
They must not be labelled as FS transitions or total native crossings. The
existing native transition profiler measures those separately. Diagnostic
logging and allocation add overhead; no FPS gain is inferred from this run.

## Slow system-service events

`PW_NATIVE_SLOW_SYSCALL_US=<n>` (patch 0613; off when unset, `0` or not a
decimal number) is for the other question: not how often a service is
entered but which one a thread sat in. With it on, the dispatcher reads the
TSC (`lfence; rdtsc`) before and after every guest 32-bit system service and
writes one line, after the call returns, for each that outlasted n
microseconds:

    PW_NATIVE_SLOW_SYSCALL version=1 tid=004c tsc=… ticks=… code=0004 name=NtWaitForSingleObject arg0=00000120 arg1=00000000 arg2=00000000 status=00000000 stack=ntdll.dll+1234,d3d9.dll+1a2b3c,d3d9.dll+2000,gtaiv.exe+3000,…

`tsc` is the start, `ticks` the duration; the rate is `PW_QPC_TSC_HZ`
(decimal Hz) or 1.6 GHz when it is unset or outside 100 MHz..10 GHz.
`code` and `name` are as above (`win32-or-other` for a service outside the
ntdll table). `arg0..2` are the first three guest arguments as 32-bit values
(a service with fewer shows what lies above them on the stack) and `status`
the NTSTATUS it returned. The dispatch and its result are unchanged.

`stack` lists, first, the syscall thunk's continuation and its caller (the
two return addresses just above the guest ESP), then up to 14 more dwords
read upward from the ESP, over at most 512 dwords and never past the 32-bit
thread's stack (TEB32 `StackLimit..StackBase`), that fall inside a loaded
32-bit module's image. Each prints as `basedllname+offset` (lowercase
ASCII, the offset from the module's load address) from the 32-bit PEB's
in-load-order list, walked once per event (256 entries at most). The first
two print as a raw address when no module holds them. This is a stack scan,
not a frame walk: saved return addresses of frames already left, function
pointers and data that happen to point into a module show up too, in stack
order, so read it as the call chain the wait probably sits under, innermost
first, and let the function names decide. Entries that do not fit in 800
characters are cut with `...`.

To name them: `i686-w64-mingw32-addr2line -f -C -e <unstripped dll>
<ImageBase+offset>`, with the DLL's preferred ImageBase from its optional
header (`i686-w64-mingw32-objdump -p d3d9.dll | grep ImageBase`); or
`tools/symbolize_guest_stack.py LOG --module d3d9.dll=<unstripped d3d9.dll>`,
which reads the ImageBase itself and rewrites every entry of the modules
given to `dll!function`. `tools/native_profile_frames.py` lists the events
with the driver's, each under the frame whose present followed its return,
and its CSV carries the CS thread's slow-service time per frame
(`slow_syscall_ms`).

Cost: off, one static check per dispatch; on, two TSC reads per service
(tens of nanoseconds) and, per event only, the loader-list walk, the scan
and the formatting, all in stack buffers under 1000 bytes with no
allocation and no call that could enter a guest service. The loader list is
read without the 32-bit loader lock, so a module being loaded at that moment
may be missing from one event. A threshold under the frame time floods the
log with waits that are normal (the present, the fence), so start at the
length of the stalls being chased, say 50000.
