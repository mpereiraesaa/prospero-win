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
The SDK build explicitly stages patched PE64 wow64win beside ntdll; pairing
these modules is required for the private registration export. The staging
fixture checks its target and copied bytes, and excludes a PE32 wow64win target.

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


## Builtin window procedure preservation

The first console UI probe completed its three explicit callback cycles, but
reported four access violations while destroying the guest IME window. The
fault address was the truncated native NtdllImeWndProc_W address. Native user32
initialization had also overwritten win32u's process-global builtin window
procedure table. The first routing patch preserved custom window callbacks,
but did not preserve these builtin procedure addresses.

Patch `0904-win32u-native-builtin-procs.patch` retains a separate WoW64 builtin
procedure table. Builtin handles remain architecture-neutral indices, and
lookup selects addresses for the current callback domain. Dynamically allocated
procedure handles retain their existing storage. The first registered user32
module remains the stable owner of shared builtin classes; registration checks
use the caller domain's module.

An expanded host fixture reproduces the failure deterministically by using
unsubclassed STATIC windows on both sides. With only the first routing patch,
guest SetWindowTextW faults during the first native service cycle. A clean
console teardown result remains required before acceptance.

The builtin IME UI class is not a builtin (fnid) class, so 0904 does not cover
it. Guest and native imm32 each register "Wine IME" from `ImeInquire`. Upstream
registers it with `CS_GLOBALCLASS`, and the server and win32u match a global
class for any instance. In GTA SA teardown the guest registered first, the
native registration failed silently, and a native service thread created the
IME UI window with the guest procedure. It then executed 32-bit guest imm32 code in
64-bit mode (r8 and r7b: fault in guest imm32 with a truncated stack pointer).
Patch `0911-imm32-domain-local-ime-ui-class.patch` drops `CS_GLOBALCLASS`.
imm32 always creates this window with its own module as the instance, so each
domain resolves its own instance-local class, and imm32 is now built from the
patched source for both architectures. `tests/lab/wine_ime_domain.py` loads
the guest IME first, then resolves and creates the class on a native service
thread: without 0911 the native lookup returns the guest procedure and forced
creation crashes; with 0911 each domain gets its own procedure.

With both patches, the expanded host UI-only and real-DXVK modes each pass
three cycles, including three guest builtin-window checks and two native
builtin-window checks per cycle. CreateDevice, Reset and Present remain S_OK
in full mode. The failing baseline and passing runs use the same expanded
fixture, and retain source/binary identities in their receipts.
