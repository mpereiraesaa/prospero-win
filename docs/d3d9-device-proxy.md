# PE32 owner-thread device proxy

The optional device frontend maps Direct3DCreate9's CreateDevice to the native
persistent service. Guest HWND registration occurs on the actual owner thread;
only checked opaque IDs enter requests. A process-local per-HWND cache shares
registration ownership across devices. Final cleanup unregisters through a
message-only window on the owner thread after native device/window cleanup.
No proxy bookkeeping lock remains held across RPC or window messages.

Device IUnknown identity, AddRef/Release, parent factory and creation parameters
remain local COM ownership. Reset and Present use copied typed payloads and real
backend HRESULTs. Final Release during a pumped callback retains the device,
parent and registration until deferred remote cleanup can run outside the
outer transaction. Unexpected cleanup failure retains ownership and cancels
and joins the session before retrying unregistration.

All 119 device vtable signatures are generated from the pinned inventory.
Unimplemented methods emit their exact slot/name and make failure sticky;
HRESULT methods return D3DERR_NOTAVAILABLE. Value/void methods cannot carry an
HRESULT, so they also mark failure before returning. No unsupported operation
performs backend work. This stage does not claim complete game compatibility,
fullscreen, distinct focus/device windows, or support for non-owner creation.

`tests/lab/d3d9_device_proxy.py` builds an actual PE32 DLL and PE64 service.
The owner calls COM CreateDevice directly. Positive mode checks device identity,
parent ownership, creation parameters, Reset and Present. The explicit
`--expect-unavailable` mode instead requires actual backend surface failure and
null output, then complete cleanup across three sessions.

Host receipt `/tmp/prospero-d3d9-device-proxy-r1/receipt.json` passed the latter
mode using the real PS5 driver, whose host VK_KHR_display surface is unavailable.
It is a negative lifecycle proof, not a positive owner-thread presentation proof.
The earlier native service console result proves successful Create/Reset/Present
with a separate guest worker; positive owner-thread execution remains a gate.
