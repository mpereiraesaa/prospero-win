# Guest fullscreen window side effects

The native service owns the backend window. Before this change, fullscreen
Create/Reset resized and showed only that window; a hidden 640x480 guest HWND
could remain unchanged after successful 1920x1080 creation. Applications querying
their guest client extent therefore observed different geometry from a direct
backend call.

After successful native Create/Reset, the PE32 owner thread applies the pinned
DXVK 2.6.2 Win32 fullscreen window operations to the guest HWND before returning:
strip overlapped styles on entry, use primary monitor bounds, become topmost,
and show with FRAMECHANGED and NOACTIVATE. Repeated fullscreen Reset updates
bounds/show/topmost without replacing the original style snapshot. A transition
to windowed conditionally restores styles only if the application has not changed
them (ignoring visible/topmost bits), restores prior topmost state and leaves
coordinates unchanged, matching `leaveFullscreenMode(..., false)`.

No focus is forced and no display mode is changed a second time. Native parameters
and driver leases remain authoritative. Final Release does not restore guest
coordinates/styles: pinned swapchain destruction restores the display mode and
WNDPROC, not the fullscreen window rectangle. The bridge never subclasses the
guest WNDPROC.

Reset holds a local device reference and a transition gate across the native RPC
and subsequent window callbacks. No session or cache lock is held during guest
window operations. Recursive Reset and wrong-owner Reset are rejected. A failed
backend call applies no guest transition. If a window callback destroys the HWND
or a guest transition fails after native success, the session is canceled;
partially applied local state is rolled back when the HWND still exists. Native
cleanup owns its window and leases independently.

The PE fixture starts hidden with a 640x480 client, reproduces the failed
1920x1080 extent predicate, then verifies fullscreen, repeated Reset, conditional
style restoration, preserved coordinates, real monitor bounds and destruction
inside a window callback on both ABIs. It is not console or full game acceptance.

Reference: DXVK v2.6.2 `src/wsi/win32/wsi_window_win32.cpp` enter/leave/update
fullscreen functions and `src/d3d9/d3d9_swapchain.cpp` Reset and destructor.

The current driver contract is a single primary monitor; this does not add
general multi-adapter/monitor selection. The fixture also compiles the real
production Reset body with a controlled transport, verifies a failed native
Reset leaves the hidden guest untouched, nested Reset never reaches the peer,
self-references survive guest callbacks, and destroyed-window failure cancels
the session. No complete native session is substituted for this fixture scope.
