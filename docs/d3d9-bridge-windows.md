# D3D9 bridge window association prerequisite

The [approved bridge design](d3d9-bridge-design.md) keeps the PE32 game window as
input/message owner and creates a separate PE64 service window for DXVK. This
change supplies a tested registry and display-ownership state machine. It is not
yet connected to Wine's UI or Vulkan driver and does not claim a working device,
window bridge, or game proxy.

## Local state and wire identities

`wine/ps5/pw_d3d9_window.[ch]` owns fixed-capacity associations. Public identities
are `{epoch, id, generation}`; local guest/service handle identities are 64-bit
opaque values and never go on the wire. The caller must validate actual window,
process, thread, and execution-domain ownership before attaching a pair. Session
epochs must not be reused while old work can survive. Slot generations never
wrap. Reset the whole registry only after the old session and surfaces stop.

A state mirror is explicitly two phase: `begin(sequence, state)` takes an owned
snapshot; the caller releases all registry/device locks, applies the native UI
operations and any guest callbacks, then acknowledges that exact sequence with
its real HRESULT. A stale acknowledgement cannot complete newer work. A failed
HRESULT remains recorded and blocks new presentation; it is not converted to
success. The first successful mirror is required before a display lease.

Only the associated service handle can reserve the display plane. Unknown probe
windows and the guest handle cannot reserve one. One pair owns the plane at a
time. Multiple distinct leases from that pair support creating a replacement
surface before destroying the previous surface during Reset. A failed native
surface creation must release its reserved lease. Final surface destruction
releases the lease; stale/double releases cannot release a later surface.

Closing blocks new admission and input routing. Pending mirror completion and
all surface releases must finish before detach. Reusing a slot changes its
generation. Local input lookup maps either member of a visible live pair back
to the original guest window; closing, hidden, or failed pairs have no target.

All functions require caller serialization. They contain no allocation, waits,
locks, Wine calls, or callbacks. This allows the adapter to take a snapshot under
its lock and perform reentrant user32 calls after releasing that lock.

## Inspected integration points and next patch

The pinned Wine is `490f6d5dcbb2a5047345b8af88d114bbcaad69a8`, with this project's
PS5 driver patches. The inspected DXVK reference is v2.6.2
`9d6f54a1ade20d1d27dd421024717a636f3d8c68`.

| Location | Current behavior | Required association hook |
| --- | --- | --- |
| `win32u/ps5drv.c:ps5_CreateWindow` | Registers every creating thread for input wakeups. | Record validated guest/service roles through an explicit bridge registration, not window title matching in production. Service creation must not make it input owner. |
| `ps5_WindowPosChanged` and `view_rect` | Track shown top-level windows and use their visible bounding box. | Keep guest presentation geometry authoritative; exclude the paired service window from the desktop view box. Publish ordered size/visibility mirrors. |
| `process_input` and `last_shown_window` | Foreground window or newest shown top-level receives hardware input. | Resolve a selected paired service handle back to its live guest before `NtUserSetForegroundWindow` and `NtUserSendHardwareInput`. Apply the same mapping to an already foreground service window. |
| `ps5_vulkan_surface_create` | Every window can release the title's video output and create a surface on plane zero. | Resolve `client_surface` using `is_client_surface_window`, reserve a checked service lease **before** `release_display` or display enumeration, and cancel the lease on every failed exit. Reject unassociated probe surfaces while the bridge owns the display. |
| `win32u/vulkan.c:win32u_vkDestroySurfaceKHR` | Destroys the host surface, releases client surface, then frees the wrapper. | Release the bridge lease after actual host destruction using `(instance, host surface)` identity. The current `vulkan_driver_funcs` has no surface-destroy callback; add a PS5-specific internal notification or a reviewed driver-ABI extension. Do not merely decrement at window destruction. |
| `ps5_DestroyWindow` | Removes the local visible-window entry. | Close the matching pair immediately; keep its generation and leases until native surfaces and pending mirror work have completed. Service destruction must not destroy the guest window. |
| DXVK `d3d9_window.cpp` | Fullscreen installs `D3D9WindowProc` through `GWLP_WNDPROC` and calls the saved procedure. | Pass only the service HWND to DXVK; its previous WNDPROC must also be PE64. Never install or call that procedure through the guest HWND. |

The native registration API is still needed. It must authenticate the persistent
bridge session/epoch and validate both local HWND owners through Wine's server
state. The wire agent reserves opcodes `0x20+` for association/mirror operations;
those numbers are not active protocol implementations. A production operation
must receive a registered ID, not look up arbitrary HWNDs from a guest packet.
The current one-shot bootstrap cannot supply this persistent registration and
callback lifetime by itself.

## Tests and remaining UI proof

`tests/test_d3d9_window.c` checks local handles above 4 GiB, stale epochs and
generations, duplicate handle rejection, copied mirror state, exact sequence and
HRESULT handling, hidden/probe rejection, two leases for one pair, exclusion of
another pair, lease exhaustion, double/stale release, input ownership, pending
close, detach, and counter exhaustion. It runs in the normal host/sanitizer suite.

Actual UI evidence must additionally prove a PE32 owner thread pumping scalar
callbacks while a separate broker waits for the PE64 service; a service-owned
WNDPROC; real DXVK CreateDevice, Reset and Present results; focus, size, input and
normal teardown. Fullscreen subclass/restore and hardware input/display-plane
ownership must be tested separately. A registry pass does not establish any of
those platform results.
