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
