# Typed stateblock proxy ownership

The module installs device CreateStateBlock59, BeginStateBlock60 and EndStateBlock61.
Its stateblock vtable implements canonical IUnknown/IDirect3DStateBlock9 identity,
AddRef/Release, local strong-parent GetDevice, and remote Capture/Apply.

Allocate the local shell before a creating RPC. A successful remote creation
reference is therefore either published or retired through a shell that already
owns a deferred-cleanup node. Cache keys include the local parent identity and
remote ID/generation. Cache lookup increments local ownership while holding the
same lock used by final release/removal. No COM or RPC call occurs under that lock.
Duplicate returned remote references are released separately after unlocking.

Every shell owns a parent-device reference through remote retirement. Final
release removes the shell from the cache first. RPC_E_CANTCALLOUT_ININPUTSYNCCALL
queues its embedded cleanup node. Other failures mark/cancel the session; native
objects then belong to service teardown and local cleanup can finish. Failed
deferred enqueue retains the shell and parent, marks failure, and never joins
inside the callback. This failure path intentionally retains ownership until
process teardown instead of freeing an object still needed for cleanup.

The call callback validates and pins the session/target. Ref0/0 means the parent
device. Malformed successful replies mark sticky failure before publication.
Callbacks are installed once before device vtable publication and remain immutable.

Actual PE32 and PE64 fixtures check all methods, canonical identity, strong parent,
512 concurrent create/capture/release cycles, duplicate remote reference cleanup,
backend failures, malformed replies and deferred retirement including enqueue
failure. Receipt: `/tmp/prospero-d3d9-stateblock-client-r1/receipt.json`.
These controlled callbacks prove proxy ABI and ownership. Native backend behavior
and production session wiring have separate receipts; no console claim is made.
