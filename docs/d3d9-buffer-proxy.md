# PE32 vertex/index buffer COM proxies

`pw_d3d9_buffer_proxy_install` installs CreateVertexBuffer and CreateIndexBuffer
on a device vtable before publication. Immutable callbacks perform actual resource
RPC, remote reference release, deferred cleanup, and fatal session cancellation.
The parent argument is a valid local device proxy; wrap consumes one service
reference on all paths after that precondition. A wrap allocation failure cancels
the session because no cleanup node can be retained safely.

The cache keys parent, kind, object ID and generation. IUnknown, Resource9 and the
concrete buffer interface share one local address. GetDevice retains the local
parent. Binding resolution compares addresses under a cache lock and never calls
QueryInterface or reads a foreign pointer. No RPC or parent COM call runs under
that lock. Each active transport method pins its proxy; an overlapping operation
on that same proxy returns the reentrancy error. Final Release removes the cache
entry before contacting the service. If called during a pumped callback it queues
an embedded node and retains its parent until the outer RPC completes. If enqueue
fails, fail marks the session cancelled without joining, while the shell, staging
and parent remain retained: cleanup must not nest
session cancellation or final parent Release inside the active RPC callback.
A deferred callback that still sees the reentrancy error requeues itself. Remote
context destruction releases any native lock; local staging is then freed.

Lock/Unlock use the bounded copied buffer client, preserving untouched bytes and
READONLY semantics. GetDesc returns checked actual backend fields. Shared handles
are rejected. Private-data methods own copied guest-local metadata and IUnknown
references through the shared helper. Their method pins protect callbacks; final
cleanup disposes metadata before dropping the strong parent. GetPriority,
SetPriority and PreLoad invoke actual native resource operations. SetPriority
returns the backend previous u32 value; PreLoad completes its native void call
before reply. A transport/reentrancy failure in these non-HRESULT methods marks
the session failed. No local pool policy replaces backend semantics.

`tests/lab/d3d9_buffer_proxy.py` compiles actual PE32 COM vtables and runs Wine with
controlled transport callbacks. It covers VB/IB descriptions, canonical identity,
parent references, foreign-pointer rejection, copied low-address locks, deferred
Release of a locked buffer, and final Release during an active GetDesc callback.
The fixture does not establish native backend or console acceptance; session
integration requires its own actual DXVK proof.
