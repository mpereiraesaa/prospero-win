# Persistent D3D9 native service session

This adapter connects the [portable transport](d3d9-bridge-transport.md),
[factory codec](d3d9-factory-wire.md), and
[object registry](d3d9-object-lifetime.md) to a real Wine PE32 client and PE64
DXVK service. It is a factory foundation; device and game proxy support follow
separately. Unsupported factory methods return E_NOTIMPL.

## Ownership and startup

A PE32 broker thread invokes the synchronous native bootstrap and remains
blocked until the PE64 service returns. API callers use a client critical
section to serialize request/reply exchanges. A process permits one session
at a time. Its positive 32-bit epoch never wraps or reuses an earlier value.

The client creates a fixed 552-byte descriptor, shared transport mapping, two
notification events, readiness event and cancellation event. Names include
process ID and epoch; the bootstrap descriptor has a fixed process-local
name. Existing objects are rejected. The descriptor contains fixed-width
fields and an absolute UTF-16 backend path, with no pointers or handles.
Publication precedes broker creation. The service copies and validates the
descriptor and opens its own handles and mapped address before signaling
readiness. Both endpoints open empty queues before the first HELLO. The
32-byte HELLO checks version, epoch, queue capacity, backend identity and
supported factory slots.

The backend is pinned to DXVK2.6.2-prospero1 x64 d3d9.dll, 3919872 bytes and
CRC32 0x6d86db72. The file remains open denying writes while LoadLibrary runs.
CRC is an accidental mismatch check, not authentication against same-process
code. The fixture receipt records SHA256
`1d626ff743d93f78e1369c1f28daf5bb3b7c0832eef2876a6f352ec48f575710`.

## Calls and teardown

CREATE9 returns a service-authoritative object ID and generation in its
payload. Factory slots4–14 invoke actual native IDirect3D9 methods. Every
field is converted through the codec; no native D3D struct is copied to wire.
Backend canonical IUnknown identity is retained locally by the registry.
Invalid targets return an outer failure with no payload; count methods never
manufacture an output. Client reply DTOs are published only after decoding,
method and outer-HRESULT validation complete.
Remote RELEASE retires the guest reference and releases the native object
only when queued references permit destruction. Stale IDs never reach DXVK.

STOP is published only after STOPPING. The service releases remaining objects
and unloads DXVK before publishing its acknowledgement and STOPPED. The
client joins its broker before closing mappings and handles. Cancellation
marks the shared channel failed and wakes both sides; close still joins and
reclaims ownership. Startup failures take the same cleanup path. No native
thread is forcibly terminated and no borrowed result pointer survives a call.
Client waits have a 30-second diagnostic timeout. A native backend call cannot
be forcibly interrupted; safe cleanup may wait for it to return. Callers must
finish using the session before close; cancel may wake an active call.

## Focused execution

Run `tests/lab/d3d9_persistent_session.py` with `--wine-build`, `--prefix`,
`--backend64` and a fresh `--output` directory. The runner compiles both ABIs
with warnings as errors and records source/binary hashes and exit codes.
It requires the native bootstrap and mixed-domain callback/builtin fixes.

Host receipt `/tmp/prospero-d3d9-persistent-r3/receipt.json` passed three
complete sessions, all slots4–14, 1200 concurrent guest calls, malformed
startup recovery, exclusive-session rejection, backend failure HRESULT,
unsupported methods, stale IDs, generation reuse, live-object STOP cleanup,
and cancellation followed by a new clean session.

Claude subsequently ran the exact r3 client/service on the accepted callback
r2 console runtime: all three cycles reported adapters1/status0, cancellation
recovery reported1200 calls, and Wine exited cleanly with no access violations
or ignored callback exceptions. Console request cbe607ad identifies the run.
