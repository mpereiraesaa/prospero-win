# Bounded D3D9 bridge transport foundation

This implements portable framing, ownership and ordering for the
[approved bridge design](d3d9-bridge-design.md). It does not load a renderer or
expose a COM proxy. No D3D9 method is enabled by defining its transport opcode.

## Shared section and local views

A session has one bounded request ring and one bounded reply ring. Each is a
power-of-two size from 128 bytes through 1 MiB. Their exact offsets/capacities,
protocol version, nonzero session epoch and total length are checked when each
endpoint opens its view. Both endpoints must open before either publishes.
The shared section contains fixed-width integers and byte storage only. Mapped
addresses, `size_t`, COM pointers and callback pointers stay in local views.

A 128-byte prefix contains an 80-byte fixed structure and zero reserved bytes.
The structure's `uint32_t` atomic read/write positions begin at offset 48 and
its atomic state is at offset 64. All endpoints must support lock-free 32-bit
atomics and little-endian storage. PE32 and PE64 use the same offsets. Each
endpoint is called by one broker/service thread; client API callers must be
serialized by the future client adapter. Neither ring uses the Vulkan opcode
space, dispatch table or replay gate. Bounded copying and release/acquire
publication follow the existing SPSC arena contract.

Unsigned byte positions wrap modulo 2^32; the bounded producer/consumer distance
cannot exceed the ring capacity. Independently validated 64-bit message
sequences do not wrap. Each message occupies a 64-byte header plus copied
payload and zero padding to eight bytes:

| Offset | Fixed-width field |
| --- | --- |
| 0, 4 | magic `PWD9`, protocol version |
| 8, 12 | total bytes, opcode |
| 16, 20 | payload bytes, producer role (0 request / 1 reply) |
| 24, 28, 32, 36 | device ID, target object ID, target generation, session epoch |
| 40, 48 | 64-bit sequence, 64-bit reply ticket |
| 56, 60 | exact HRESULT bits, reserved zero |

The target ID and generation are both zero or both nonzero. Replies echo the
request's opcode and target identity; newly returned object IDs belong in a
typed reply payload. A bounded local ledger retains up to 32 pending requests.
Replies must match the next pending sequence/ticket/target/generation. Missing,
duplicate, stale, reordered and mismatched replies fail the session. This
correlation check does not replace the service's live object-generation table.

Consumers copy into caller-owned scratch, validate the complete header and
padding, and only then release arena capacity. Producers copy inputs before
returning. No caller payload can alias the shared section. A full ring or full
pending ledger returns `FULL`; insufficient scratch returns `SMALL` without
consuming anything. Checked offset/length slices reject overflow, bad alignment,
fixed-prefix overlap and noncanonical empty slices. Typed method decoders must
still validate their own complete schema before calling a backend.

## Startup, stop and cancellation

The first request and reply are HELLO at sequence/ticket 1. The service may
transition STARTING to READY only after receiving HELLO. The client cannot
publish ordinary calls until it consumes the HELLO reply. The future typed
handshake must validate backend identity, ABI/capabilities and section bounds
before the service accepts it; a transport state change alone is not proof.

The client transitions READY to STOPPING and queues exactly one STOP after prior
requests. Early STOP and repeated STOP publication are rejected.
The service replies to all accepted work, releases its owned backend objects
and joins children, acknowledges STOP, then enters STOPPED. The core refuses
STOPPED while the request ring is nonempty or an accepted request still awaits
a service reply. The client may drain the final replies after STOPPED.
Cancellation stores the first nonzero 31-bit transport failure reason atomically and makes subsequent
send/receive operations return CLOSED. Adapter cancellation must also signal
both wait events. Storage cannot be unmapped until the service/broker have
joined; these portable functions never allocate, map, free or wait.

For a persistent native service, the proposed Wine adapter uses a PE32 broker
thread blocked in the private bootstrap while a PE64 service owns the backend.
A versioned descriptor names bounded shared sections/events; each side maps its
own view and validates the epoch and sizes. STOP/CANCEL must wake both sides,
join native children, release backend objects and only then allow bootstrap to
return/unload. No pointer borrowed from a guest API stack is a descriptor or
reply target. This Windows adapter remains subsequent work.

## IDirect3D9 factory coverage policy

No methods below are implemented by this transport PR. The inventory accounts
for all 17 IDirect3D9 vtable slots before a factory proxy can be enabled:

| Method | Required implementation policy |
| --- | --- |
| QueryInterface | Canonical local IUnknown/IDirect3D9 identity; explicit interface inventory; unsupported interfaces return E_NOINTERFACE. No Ex exposure yet. |
| AddRef, Release | Local guest references plus separate queued references. Final backend release only after earlier uses; generation changes before slot reuse. |
| RegisterSoftwareDevice | No guest callback pointer on wire. The pinned DXVK 2.6.2 implementation ignores its argument and returns D3D_OK; a compatibility implementation may call that exact backend synchronously with NULL only after identity negotiation. This does not implement software callbacks. |
| GetAdapterCount | Synchronous exact unsigned result; an optional cache requires immutable backend/session identity. |
| GetAdapterIdentifier | Synchronous HRESULT and explicit scalar/string/GUID serialization; never memcpy compiler-dependent structs. |
| GetAdapterModeCount, EnumAdapterModes, GetAdapterDisplayMode | Validate formats/indices; return exact count or HRESULT and explicitly encoded display mode fields. |
| CheckDeviceType, CheckDeviceFormat, CheckDeviceMultiSampleType | Synchronous exact HRESULT; multisample quality count is a checked optional output. |
| CheckDepthStencilMatch, CheckDeviceFormatConversion | Synchronous exact HRESULT with typed format/device inputs. |
| GetDeviceCaps | Synchronous HRESULT and field-wise caps encoding, including nested shader-capability fields. |
| GetAdapterMonitor | Map the native monitor to a validated guest monitor association; do not truncate a native HMONITOR. |
| CreateDevice | Requires the separate guest/service window association and full device proxy. Preserve HRESULT, presentation parameters, callback/thread ownership and object lifetime. Remains unavailable until those gates pass. |

Pinned source reference is DXVK v2.6.2 commit
`9d6f54a1ade20d1d27dd421024717a636f3d8c68`, `src/d3d9/d3d9_interface.cpp`.
Factory coverage is not device coverage; all IDirect3DDevice9 methods need their
own inventory and codecs. A smoke launch cannot replace that work.

## Validation

`test_d3d9_bridge_wire` covers wrapped copying, 32-bit counter wrap, exact HRESULT
bits, bounded ring/ledger backpressure, malformed framing, wrong reply generation,
first-error cancellation, stop ordering, ownership after caller overwrite and
20,000 concurrent request/reply pairs. It runs in the host and sanitizer suites.

The optional `tests/lab/d3d9_bridge_wire.c` fixture builds unchanged as PE32 and
native Unix64. Run its `produce request.bin` in PE32, `consume request.bin
reply.bin` in Unix64, then `check reply.bin` in PE32. It exchanges actual shared
wire images, including a wrapped payload, 64-bit ticket `0x1234567800000021`,
32-bit target/generation values, and a failing HRESULT. It passed on host Wine;
this is wire compatibility proof, not a running persistent service.
