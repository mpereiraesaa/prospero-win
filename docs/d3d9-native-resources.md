# Native buffer resource adapter

The service-local adapter owns real PE64 vertex/index buffers and keeps their
canonical IUnknown identities in the service registry. Shared kind tags identify
factory, device, vertex/index buffer, texture, surface, declaration and shader
objects before native casts. These tags do not create those later resources.
The session must retain its parent device/window context until child resources
are destroyed; the adapter also retains the backend device COM reference.

Create and GetDesc call the real backend. Lock validates the descriptor range
before calling the backend with unchanged flags. Zero length exposes the
remaining safe range; over-64-MiB spans and nested locks are explicitly unsupported.
A successful lock gets a new nonwrapping 64-bit generation. Reads copy bounded
chunks; writes must be contiguous and cover the entire writable span before
Unlock. READONLY prohibits writes and needs no upload. Stale generations,
duplicate writes and out-of-range requests fail without touching the mapping.

Unlock and CANCEL_LOCK return the actual backend HRESULT. Cancellation permits
staging allocation or transfer failure to end the real lock without requiring a
complete upload. Destruction attempts an outstanding Unlock, returns its HRESULT,
and releases owned interfaces even on failure. No mapped pointer enters wire
payloads. Session cancellation must prevent later draws after partial transfers.

`tests/lab/d3d9_native_resource.py` compiles the real adapter and runs it through
host Wine with an explicitly selected DXVK DLL. Three processes each create both
buffer kinds in DEFAULT/dynamic and MANAGED pools, transfer more than one ring's
worth of data, overwrite request storage after copies, read it back, reject stale
IDs/ranges/duplicate unlocks, cancel a lock and destroy an active mapping. The
fixture also compares a real zero-length buffer creation failure with the adapter
HRESULT. Those backend checks use real DXVK methods. A separate isolated
controlled CreateVertexBuffer returning success with a null output verifies the
defensive guard without dereferencing or publishing that output. This proves the PE64 adapter and copied
payloads; PE32 low-address staging, COM identity proxies and session dispatch are
separate integration work.

Owned references returned by GetStreamSource/GetIndices can be adopted after
the guest proxy was released while the backend retained a binding. Adoption
consumes the returned COM reference on every path, validates the VB/IB interface
with QueryInterface and checks GetDevice ownership. The session deduplicates
canonical identity or assigns a new generation. The actual host fixture binds
each buffer, destroys its first wrapper, gets it back, adopts it and checks the
same IUnknown identity before unbinding and final release.
