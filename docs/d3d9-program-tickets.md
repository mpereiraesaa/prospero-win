# Private program queue tickets (inactive)

`PW_D3D9_ENABLE_BINDING_TICKETS` adds private lifetime tickets for declaration,
vertex shader and pixel shader proxy shells. No shipping builder or admission
policy enables the feature. The caller holds its session admission gate while
acquiring a ticket and publishing the corresponding copied command. Resolution
checks canonical local identity, parent, kind and positive public references
under the proxy cache lock. Foreign pointers are compared without dereferencing.

A ticket does not increment the public COM reference count. Public Release0
still removes the identity from the cache and invokes the remote Release barrier.
Native acknowledgement or cancellation and the last private ticket must both
complete before the shell releases its strong parent. A finishing sentinel keeps
the shell alive when ticket completion happens inside the Release callback.
Deferred cleanup retains the shell and parent, including failed defer attempts.
Closed shells cannot be resurrected through AddRef or QueryInterface.

Session integration must detach tickets while locked and invoke their drop
callbacks after unlocking. Drops can release the final parent reference. Each
ticket is move-only; clearing it before invoking its callback makes repeated drops
of the same storage harmless, but copying a live ticket is invalid.

The controlled fixture covers all three kinds, wrong parent/kind/foreign pointer,
public-zero barriers, duplicate tickets, completion during Release, deferred and
failed-defer cleanup, terminal cancellation and zero remaining parent references.
It uses actual proxy code and mock transport; it does not establish paired queue
or native backend correctness. Buffer and texture ticket support is separate.
