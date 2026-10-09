# D3D9 bridge object identity and lifetime

The portable registry in `wine/ps5/pw_d3d9_objects.c` is the service-side
owner of bounded object IDs and generations. It is a foundation for the B1
adapter; it does not yet install a guest D3D9 proxy or claim game compatibility.

Each session/device owns caller-supplied storage. Every registry operation must
hold the adapter's lock. The registry performs no allocation, callbacks, COM
calls or waiting. Canonical IUnknown identities and backend contexts remain
local pointer-width values. Only `{id, generation}` is serialized, accompanied
by the transport's device ID and epoch. Validate those through lookup before
acting on untrusted transport targets. Do not serialize this registry's structs.

Creation reserves a slot before calling the backend outside the lock. On return,
commit requires a unique canonical identity. If another creation already
registered that identity, abort the reservation, acquire the existing entry
using its returned-reference path, and balance the incoming backend reference.
The adapter must handle a destroying identity by waiting for destruction to
finish; it cannot create a duplicate or revive the destroying entry. Commit
failure leaves the reservation for explicit abort and the backend reference
with the caller. Cancellation invalidates reservations, so late creation cannot
publish into a stopped session.

Guest and queued-work references are separate. Acquire queued references before
publishing a command, rolling them back if publication fails. Release each once
on completion or cancellation. Zero guest references retires the object; queued
work keeps its backend alive. A newly returned COM reference can revive a
retiring object, but ordinary AddRef cannot. The adapter must retain at least one
owning backend COM reference for the entire registered lifetime and balance
additional references returned by backend methods.

Once both counts are zero, take_destroy transfers the backend-release obligation
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
