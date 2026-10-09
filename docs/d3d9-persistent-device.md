# Persistent native D3D9 device calls

The device adapter extends [persistent sessions](d3d9-persistent-session.md)
with opcode19 and the [device codec](d3d9-device-wire.md). CREATE targets a
factory ID; RESET and PRESENT target a device ID. The registry checks interface
kind and generation before a backend call. Returned device pointers and their
canonical IUnknown identity remain native-local.

## Window ownership

The guest UI thread registers its HWND through the private driver API and
passes only the resulting epoch/id/generation. The native service creates its
own popup HWND and WNDPROC. Driver ATTACH resolves the guest identity locally,
checks session token and window owners, and returns current guest client
geometry. Every operation performs QUERY_STATE, BEGIN, native geometry/show
updates outside driver locks, then ACK. Native service waits pump messages.
Guest input ownership remains with the guest window through the driver pair.

Initial coverage supports windowed presentation with one guest focus/device
window. Different focus/device windows, fullscreen presentation and a Present
override naming another guest window return D3DERR_NOTAVAILABLE. These paths
are explicit unsupported cases. A zero presentation width/height is passed to
DXVK unchanged so it derives dimensions from the mirrored native window.
Null window fields remain null; only a native service window returned by the
backend can map back to its opaque guest identity. No HWND enters the queues.

CREATE and RESET return backend in/out presentation parameters even when the
backend fails, according to the codec. PRESENT converts bounded rectangles
and dirty regions field by field. The backend HRESULT is preserved. These
operations alone do not provide a complete IDirect3DDevice9 game proxy.

## Cleanup

Device release first closes driver admission, then releases the backend device,
destroys its native window and detaches after surface leases drain. Unexpected
CLOSE/DETACH/window destruction failures retain the context in a native cleanup
list and mark failure. Session cleanup retries while pumping messages; the
module and backend remain loaded until windows, associations and class callbacks
have drained. It never force-unloads live callback code. A permanent driver
cleanup failure can therefore prevent the broker join from completing; this is
an explicit failure condition rather than a successful teardown.

## Fixture

`tests/lab/d3d9_persistent_device.py` builds a PE32 UI client and PE64 service.
The guest owner registers its window, pumps messages while a guest worker sends
calls, then unregisters after three complete native service cycles. The default
mode requires real CREATE/RESET/PRESENT success in every cycle.

An explicit `--expect-unavailable` mode requires real CreateDevice to return
D3DERR_NOTAVAILABLE, no device ID, preserved failed-call parameters and clean
session/association cleanup in all three cycles. The isolated host PS5-driver
runtime lacks a usable VK_KHR_display surface, so only this negative mode is
expected there. Receipt `/tmp/prospero-d3d9-device-unavailable-r1/receipt.json`
records this limited host proof. No positive console device result is claimed
by this document yet.
