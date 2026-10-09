# D3D9 bridge object identity and lifetime

The portable registry in `wine/ps5/pw_d3d9_objects.c` is the service-side
owner of bounded object IDs and generations. It is a foundation for the B1
adapter; it does not yet install a guest D3D9 proxy or claim game compatibility.

Each session/device owns caller-supplied storage. Every registry operation must
hold the adapter's lock. The registry performs no allocation, callbacks, COM
calls or waiting. Canonical IUnknown identities and backend contexts remain
local pointer-width values. Only `{id, generation}` is serialized, accompanied
by the transport's device ID and epoch. Validate those through lookup before
acting on untrusted transport targets. Lookup exposes only live objects, or
retiring objects with queued work still holding their backend. Reserved,
fully drained, and destroying slots are never executable targets, even with a
matching generation. A new incoming command must acquire its own queued reference
before publication; lookup alone does not acquire a reference. Do not serialize
this registry's structs.

Creation reserves a slot before calling the backend outside the lock. On return,
commit requires a unique canonical identity. If another creation already
registered that identity, abort the reservation, acquire the existing entry
using its returned-reference path, and balance the incoming backend reference.
The adapter must handle a destroying identity by waiting for destruction to
finish; it cannot create a duplicate or revive the destroying entry. Commit
failure leaves the reservation for explicit abort and the backend reference
with the caller. Cancellation invalidates reservations, so late creation cannot
publish into a stopped session.

Guest, explicit backend-owner, and queued-work references are separate. Acquire queued references before
publishing a command, rolling them back if publication fails. Release each once
on completion or cancellation. Zero guest references retires the object only when no backend-owner leases remain;
queued work keeps a retiring backend alive. A newly returned COM reference can revive a
retiring object, but ordinary AddRef cannot. The adapter must retain at least one
owning backend COM reference for the entire registered lifetime and balance
additional references returned by backend methods.

Once all three counts are zero, take_destroy transfers the backend-release obligation
exactly once. Perform COM Release outside the registry lock, then finish_destroy
under the lock. Only finish_destroy or abort recycles an ID. Generations never
wrap: a slot reaching UINT32_MAX is permanently exhausted after retirement.
Epochs must also never be reused while old transport messages can survive.

Cancellation blocks new objects and references, retires live objects, and keeps
queued references intact until every queued or executing operation acknowledges
completion. Join workers and finish backend destruction before releasing storage.
Reinitializing a populated registry is forbidden by the caller contract.

`make build/host/test_d3d9_objects` builds the focused fixture. It covers capacity,
canonical identity, stale targets, reservation failure, reference overflow,
queued-work retirement, explicit revival, destruction handoff, late creation
following cancellation, high pointer values, and generation exhaustion. The
fixture also participates in `make all` and `make sanitize`.

## Explicit backend-owner leases

`pw_d3d9_object_owner_hold` and `pw_d3d9_object_owner_drop` record a proven
backend ownership relationship. They require a live generation and the adapter
lock. Holds are bounded by `UINT32_MAX`; a lease cannot revive a retiring or
destroying entry. A live entry with owner leases remains discoverable and
queueable at zero guest references, and ordinary AddRef may reacquire a guest
reference. Dropping the last owner retires an entry only if its guest count is
also zero. Queued work still delays the destruction handoff. Cancellation
consumes all owner leases and guest references; callers must not drop those
leases again. Generation checks reject stale operations even if a later object
reuses the same backend address.

A lease is registry accounting, not a COM AddRef, a device reference, or proof
that a native pointer remains valid. The adapter must establish and maintain
that proof, balance backend storage ownership, and revoke the lease at the
actual owner boundary. In particular, implicit default surfaces, currently
bound resources, and texture-owned subresources have distinct ownership rules.
This change does not install any of those leases in the production adapter or
implement a Reset transaction. It does not change existing adapters' behavior
when their owner count remains zero.

The focused fixture covers public-zero lookup and queue admission, public
reacquisition, owner overflow and underflow, retirement, queued destruction,
address reuse with a new generation, and cancellation with outstanding work.
