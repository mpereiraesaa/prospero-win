# Mixed-domain D3D9 window probe

This synthetic lab exercises the window prerequisite from the
[bridge design](d3d9-bridge-design.md). It is not a proxy DLL or a PS5 display-plane
association. The native-domain bootstrap and callback-domain follow-up must be
present in the supplied host Wine build.

```sh
python3 tests/lab/d3d9_bridge_window.py \
  --wine-build /path/to/patched/wine-build --prefix /path/to/test-prefix \
  --backend64 /path/to/pinned/x64/d3d9.dll --output /tmp/window-proof
```

The PE32 main thread owns its own HWND and WNDPROC and pumps messages. A separate
PE32 broker calls the private native-domain bootstrap. The PE64 service creates
its own HWND and WNDPROC, loads the explicit backend, and calls real
Direct3DCreate9, CreateDevice, Reset, Clear, Present and Release. Its window uses
`WS_EX_NOACTIVATE`; size and activation-message mirrors address only that native
window. The original guest WNDPROC must remain unchanged throughout the run.

A uniquely named shared mapping carries fixed-width counters, dimensions and
session/window/generation IDs. Neither HWND nor callback pointers are passed
through that control mapping. The lab locates its own unique windows by class
name; this is test coordination, not the production registration/security API.
The session name is supplied at process launch because updates to the guest
PE32 environment are not a reliable way to update the PE64 service environment.

Three service cycles check guest callbacks and guest vectored exceptions before,
during and after native user32 use. A native child creates and destroys another
native window and handles its own callback and vectored exception. Native parent
exceptions are also handled. Callback routines verify the thread's execution
domain. Every native thread is joined before the service unloads. Scalar guest
notifications are acknowledged by the owning guest UI thread while the broker
waits; there is no direct call to a WNDPROC from the other architecture.

The service also checks the private callback-table registration API rejects both
null and replacement registrations. The WoW64 marshal table must already have
been registered and remain immutable.

`--ui-only` deliberately skips every DXVK call to isolate callback routing. Its
unexecuted HRESULT fields stay `0xdeadbeef`; the receipt explicitly marks device
calls disabled. A full proof requires the default mode, all three iteration rows,
actual HRESULTs, and a normal process exit. A log reaching CreateDevice before
crashing is not a pass. The runner saves failing commands and outputs as well as
successful receipts and artifact identities.

## Bug exposed by the initial probe

The original native-domain bootstrap can load a PE64 factory without proving
mixed user32 callbacks. In the pinned Wine, `wow64win/syscall.c` initializes the
PEB64 kernel callback table with WoW64 marshalers. Loading native user32 replaces
that process-global table in `user32/user_main.c`. The original x64
`KiUserCallbackDispatcher` selects the PEB table unconditionally. Consequently,
a later guest UI callback can jump to a PE32 WNDPROC while still executing in the
native domain. The probe reproduced this on host, including a run where native
CreateDevice had already returned before the guest callback faulted.

The follow-up must preserve distinct native and WoW64 callback tables and select
the proper table using the current thread's domain. Per-process or temporary
table swapping is insufficient while native and guest threads run concurrently.

Remaining independent proofs include fullscreen DXVK subclass/restore, real
hardware input routing, cursor coordinates, guest/service visibility policy, and
the [PS5 display lease integration](d3d9-bridge-windows.md). This host fixture
cannot establish those console-driver behaviors.

The host proof on 2026-10-09 passed three UI-only cycles and three complete DXVK
cycles after callback-domain routing was added. Every complete cycle returned
`D3D_OK` from CreateDevice, Reset and Present, kept the guest procedure unchanged,
handled four guest notifications and one guest key event, executed 33 native
window callbacks, handled three guest and two native vectored exceptions, and
completed native-child and normal teardown checks. Both pre/post guest callback
checks ran in each cycle. This is host evidence for the inspected backend and
callback fix; PS5 window/display/input acceptance remains separate.

The subsequent console run exposed a guest IME builtin-procedure teardown
exception after native user32 had initialized. Therefore the passing host cycle
markers do not establish clean console teardown. Wine's process-global builtin
window-procedure table is a second, independent domain-routing issue. A focused
follow-up adds explicit unsubclassed builtin windows before/during/after native
service execution; that expanded UI-only fixture reproduces a guest execute
fault in `SetWindowTextW` on host. Its fix and regression evidence are separate
from this initial fixture and the PEB callback-dispatch fix.
## Builtin window procedures

The follow-up fixture also keeps an unsubclassed PE32 `STATIC` child alive
across native user32 loading, calls it before/during/after each service cycle,
and creates and destroys additional guest builtin windows during and after.
The native parent and native child thread each create, call, and destroy a
`STATIC` child. Each roundtrip sets and reads text through the real builtin
procedure; expected counters are `guest_builtins=3 native_builtins=2`. This
exercises Wine's builtin procedure table separately from custom WNDPROCs.
Failed runner receipts now preserve the exact source and compiled binary
hashes. The earlier immutable console package is not changed by this fixture.

The expanded UI-only fixture reproduced the builtin-table defect on host: the
first native-service notification led to a guest execute fault in
`SetWindowTextW` on the retained `STATIC` control. After the separate builtin
procedure table fix, the expanded UI-only and full DXVK fixtures each passed
three host cycles with builtin counters 3/2. Every full-cycle CreateDevice,
Reset and Present returned zero. These receipts use the final callback/builtin
runtime without the later session-token changes. Console confirmation of the
expanded fixture remains pending.

## Opaque driver association mode

`--driver-association --ui-only` compiles the fixture against the private
window-driver API and requires the actual PS5 Wine driver. The guest registers
its own window and passes only an epoch/ID/generation. The native service
attaches its own window, queries real guest geometry, mirrors and acknowledges
two sizes, then closes and detaches after destroying its HWND. Guest and native
wrong-domain requests and stale IDs must fail. Each cycle unregisters the guest.

The isolated host PS5-driver run passed three complete cycles, including the
existing builtin-window, callback and exception checks. This proves the local
driver API and teardown; UI-only mode does not exercise Vulkan display-plane
leases or console hardware input. It does not establish a successful D3D9
device on a host without `VK_KHR_display`.
