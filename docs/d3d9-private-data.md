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
