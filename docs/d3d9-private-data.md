# Local COM resource private data

The guest resource proxy owns `pw_d3d9_private_data`, initialized to zero. Every
method must pin its enclosing proxy through completion; dispose is its final
cleanup. No private data or IUnknown address crosses the bridge wire.

Byte metadata is copied. D3DSPD_IUNKNOWN accepts the interface pointer itself,
requires the local pointer width and owns one AddRef. Get returns a new reference.
A null interface is a stored null pointer; a null byte-data pointer removes the
key. Size queries, MORE_DATA and missing-key behavior follow the pinned DXVK
resource implementation. Free of an absent key succeeds.

A per-store SRW lock protects GUID lookup and list changes. Entry references keep
a removed entry alive while Get calls IUnknown::AddRef outside the lock. All
external AddRef/Release calls happen without the store lock, including replacement
and disposal; the closed flag rejects new entries during final destruction.
Allocation is bounded by 256 keys per store and 64 MiB aggregate live-entry storage.
Exhaustion returns E_OUTOFMEMORY without replacing existing metadata.

The actual PE32/PE64 fixture covers copied bytes, exact size queries, unchanged
short outputs, local IUnknown ownership, null interfaces, reentrant Free from an
IUnknown::AddRef callback, bounded entry/length rejection and final cleanup. It
uses controlled IUnknown callbacks; this is a local COM contract, not backend or
console acceptance.

With `--backend64`, the PE64 fixture compares the helper directly against actual
DXVK vertex/index buffers, a texture and an offscreen surface: unknown flags,
null-byte-data removal, missing-key Free/Get, size-only queries, short buffers,
null size pointers, interface-size rejection and local IUnknown ownership. The
comparison passed on the pinned backend. Source semantics were also checked in
[`d3d9_resource.h`](https://github.com/doitsujin/dxvk/blob/9d6f54a1ade20d1d27dd421024717a636f3d8c68/src/d3d9/d3d9_resource.h)
and [`com_private_data.cpp`](https://github.com/doitsujin/dxvk/blob/9d6f54a1ade20d1d27dd421024717a636f3d8c68/src/util/com/com_private_data.cpp).
