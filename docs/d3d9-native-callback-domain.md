# Native and WoW64 user callback routing

The [native service domain](d3d9-native-domain.md) shares a process with a PE32
guest. Loading PE64 user32 installs its callback table in the native PEB. Before
that load, wow64win installed its marshal callbacks in the same field. A single
process-global pointer therefore cannot identify the callback ABI of both
kinds of thread.

The observed mixed-window fixture created a real native DXVK device, then a
guest window callback was invoked in 64-bit mode. The guest procedure address
and truncated guest stack in the fault identified the incorrect dispatch.

Patch `0903-ntdll-native-callback-domain.patch` preserves wow64win's table in
ntdll during wow64win process attachment. Its private registration export
rejects NULL and replacement with a different table. Re-registering the same
table is harmless. Native user32 retains its normal PEB callback table.

The x86-64 callback dispatcher checks the current native TEB's WowTebOffset.
A nonzero offset selects the saved WoW64 marshal table; zero selects the native
PEB table. The choice is made for every callback, including native service
children. There is no process-global table swap around service execution.
The existing exception handler and callback-return frame remain unchanged.

## Regression scope

The synthetic mixed-window fixture exercises guest callbacks and exceptions
before, during and after three native service load/run/unload cycles. Native
parent and child threads own their own window procedures and exception
handlers. The guest window procedure remains unchanged. A UI-only mode
isolates routing; the full mode additionally calls real DXVK CreateDevice,
Reset and Present. Results must be recorded separately for these two modes.

This change addresses callback dispatch only. It does not establish console
presentation-plane ownership, guest input ownership, a persistent bridge
session, or a complete D3D9 proxy.

## Recorded host result

Both UI-only and real-DXVK modes passed three complete cycles after this patch.
Every full-mode cycle returned S_OK from CreateDevice, Reset and Present. Each
cycle recorded four guest notices, two guest callbacks before/after service,
three guest exceptions, two native exceptions and one native child window.
The registration NULL/replacement rejection checks passed. This is host
execution evidence; console acceptance remains pending.
