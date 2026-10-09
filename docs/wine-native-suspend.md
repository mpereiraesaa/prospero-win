# Preserve native service contexts during suspension

The PE64 service and its children run in a process whose main image is PE32.
`contexts_to_server` previously manufactured an i386 secondary context whenever
no WoW64 context existed and the process image machine differed. On resume,
`contexts_from_server` overlaid that secondary context on the real native
context. RIP, RSP, RBP and RAX–RDI were truncated to 32 bits; R8–R15 survived.

Patch0908 excludes only private native-domain threads from that synthetic
conversion and secondary restore. Real WoW64 contexts and ordinary guest
fallback behavior are unchanged. Both suspend handling and debug-event
roundtrips use these helpers. This is independent of callback-table routing.

The captured GTA failure returned to low32 FF6D50C4 instead of
13FF6D50C4, the RET in NtUserMsgWaitForMultipleObjectsEx, with a similarly
truncated native stack. The old host runtime reproduces the same return-site
failure with a native child suspending its parent while the parent waits.
The failing process starts Wine's debugger and reaches the runner deadline;
that negative is a fault plus timeout, not a successful terminal run.

Run the focused actual mixed-domain regression:

```
python3 tests/lab/wine_native_suspend.py --wine-build BUILD --prefix PREFIX --output NEW_OUTPUT
```

It executes ten service load/run/unload cycles, eight native suspend/get-context/
resume operations each, on an explicitly allocated stack above 4 GiB. Every
captured RIP/RSP must remain above 4 GiB. A small assembly wrapper verifies
high-bit RBX/R13 sentinels after returning from the wait loop. The fixture updates
its TEB stack bounds while using this test stack and restores them before free.
Ordinary PE32 suspension runs before and after all native cycles. Existing TLS,
exception, child-domain, token, malformed-bootstrap and missing-DLL checks remain.

The corrected host run passes all 80 operations with clean exit0. Persistent
receipts live under prospero-win-artifacts/bridge-recovery-20261009/native-suspend;
these are fresh evidence, independent of historical receipts lost in reboot.
No console acceptance is claimed by this host regression.
