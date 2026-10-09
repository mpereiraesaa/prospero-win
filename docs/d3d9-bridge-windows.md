# D3D9 bridge window association prerequisite

The [approved bridge design](d3d9-bridge-design.md) keeps the PE32 game window as
input/message owner and creates a separate PE64 service window for DXVK. This
change supplies a tested registry and display-ownership state machine. The adapter described below connects it to Wine driver hooks; runtime
acceptance and a game proxy remain separate work.

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
Handle lookup distinguishes an ordinary unassociated HWND from a known pair
whose input is suppressed, so the driver can preserve ordinary dialog routing.

All functions require caller serialization. They contain no allocation, waits,
locks, Wine calls, or callbacks. This allows the adapter to take a snapshot under
its lock and perform reentrant user32 calls after releasing that lock.

## Inspected integration points

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

The native registration adapter below validates its lifecycle token and both
local HWND owners through Wine server state. Persistent service integration
still has to call it and correlate its returned IDs with the wire session. The wire agent reserves opcodes `0x20+` for association/mirror operations;
those numbers are not active protocol implementations. A production operation
must receive a registered ID, not look up arbitrary HWNDs from a guest packet.
The original one-shot bootstrap does not supply the token foundation by itself;
the token extension and persistent cleanup discipline are separate dependencies.

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

## Driver adapter (patch 0906)

`pw_d3d9_window_driver.[ch]` connects the registry to PS5 window lifetimes,
input selection, view geometry, and Vulkan surface creation/destruction. It
requires the separate native lifecycle-token query: current-thread
`NtQueryInformationThread` class `0x50570002`, version 1, size 16. Tokens correlate
service lifetimes and native children; they are not a same-process security
boundary.

The service invokes native `NtUserCallTwoParam(&request, sizeof(request),
0x50570020)` with the native request in the header (version 2 is 88 bytes). The WoW64 thunk rejects this
operation and the driver independently requires a native token. Creation hooks
record hidden and visible HWND owners/domains. Attach checks actual Wine server
owners, a WoW64 guest, and a service owned by the calling native thread/token.
At the 256-entry owner limit, native window creation fails before showing the
window. Untracked windows are excluded from input/view selection while the
table is full; recorded ordinary windows retain their routing. Unrecorded
windows cannot join a bridge.

The driver assigns a monotonic nonzero epoch, independently of wire session
epochs; exhaustion fails. A new token waits until prior associations and surfaces
are gone. All API operations require the original service owner thread. UI
operations and scalar callbacks run outside driver locks between BEGIN and ACK;
ACK must carry the real result. Destroyed associated windows retain tombstones
so ACK/CLOSE/DETACH can drain after destruction. A window destruction never drops
a live surface lease. Explicit detach is required before bootstrap returns;
missing cleanup fails closed and blocks the next session.

Native service/probe windows are excluded from input fallback and view bounds.
Foreground service input maps to the live guest; suppressed associations cannot
send null-target events back through the server's foreground selection. Ordinary
unassociated dialogs retain their routing. Every surface reserves one of 64
bounded slots before display release/enumeration. Existing ordinary surfaces
block first attach; native probes cannot reserve before attach. Associated
service/token checks and generation validation precede leases. Same-pair reset
replacement surfaces may overlap. Failures cancel their reservation; the exact
(instance, host surface) lease is released only after actual host destruction.
Client-surface lookup runs outside the driver lock to avoid lock inversion.

The host contract includes the actual adapter C with controlled Wine owner/token
and surface-lookup boundaries. It checks rejection, ownership, mirror gating,
input, replacement/failure, post-destroy cleanup and stale epochs. Modified Wine
ps5drv.c/sysparams.c/vulkan.c compile with the pinned SDK commands. These are
compile/contract results: token-enabled runtime packaging, service integration,
and console input/display acceptance remain pending. The earlier device probe
does not call this API and does not establish these hooks' runtime acceptance.

## Opaque guest registration (patch 0907)

Production transport carries only `pw_d3d9_window_id {epoch,id,generation}`.
Guest HWNDs are resolved exclusively inside the driver. On its owning PE32 UI
thread, the proxy invokes `NtUserCallTwoParam(hwnd, &request, 0x50570021)` using
the fixed 32-byte guest request, version 1. REGISTER returns a nonzero opaque ID;
duplicate registration returns the same live identity. UNREGISTER requires that
identity and fails while an association still references it. Native callers,
foreign windows, other owner threads, malformed requests and unknown handles
are rejected. The WoW64 thunk forwards only this explicitly pointer-free request;
the HWND parameter is a local syscall argument and never enters the wire.

Each registration has a driver-issued monotonic epoch plus its owner-slot ID and
generation. Unregister followed by register advances both counters. Destruction
retires the identity; retaining a tombstone solely for native cleanup cannot
make a destroyed window attachable again. Slot/handle reuse cannot revive an old
ID, and counter exhaustion fails without wrapping. These IDs remain independent
of the wire frame session epoch, which the transport validates separately.

Native request version 2 is 88 bytes and replaces the old local guest HWND field
with the opaque 12-byte registration ID. The native service HWND remains local.
The driver snapshots and resolves the guest ID under its lock, queries actual
Wine owners outside the lock, then validates the same identity again before
changing an association. Only the guest-registration and association IDs may be
serialized. All-zero IDs are reserved for null/inherit in typed message fields
where that operation permits it; REGISTER and ATTACH require real identities.

ATTACH returns a checked guest client rectangle in physical screen coordinates,
with visibility and foreground flags, in `request.state`. Native QUERY_STATE
(operation 6) refreshes that snapshot before Reset/Present; it has the same token,
ID and owner checks. This permits zero-width/height presentation parameters to
use the real guest dimensions without transporting an HWND or guessing a size.
Queries run outside the driver lock, followed by identity validation. The service
still applies the returned state between BEGIN and ACK and supplies the actual
UI operation result. Fullscreen policy remains an explicit service decision.
