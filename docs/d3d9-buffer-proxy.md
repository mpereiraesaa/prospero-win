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
an embedded node and retains its parent until the outer RPC completes. Remote
context destruction releases any native lock; local staging is then freed.

Lock/Unlock use the bounded copied buffer client, preserving untouched bytes and
READONLY semantics. GetDesc returns checked actual backend fields. Shared handles
are rejected. Private-data operations return NOTAVAILABLE. Priority and PreLoad
currently invoke the fatal unsupported-operation callback; they do not claim an
unperformed backend operation succeeded. These methods remain a coverage gap.

`tests/lab/d3d9_buffer_proxy.py` compiles actual PE32 COM vtables and runs Wine with
controlled transport callbacks. It covers VB/IB descriptions, canonical identity,
parent references, foreign-pointer rejection, copied low-address locks, deferred
Release of a locked buffer, and final Release during an active GetDesc callback.
The fixture does not establish native backend or console acceptance; session
integration requires its own actual DXVK proof.
