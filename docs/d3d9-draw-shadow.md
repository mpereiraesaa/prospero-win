# Declaration evidence for ordered draws

This portable foundation does not enable queued draws. It models only immediate
HRESULT eligibility for pinned backend revision5fde742b, with no resource handles
or native reference ownership. Runtime integration and actual backend parity
remain required.

`DrawPrimitive` and `DrawIndexedPrimitive` first reject a NULL active declaration,
even for zero primitives, and otherwise have no additional immediate HRESULT
validation (device.cpp2893–2995). The bounded policy accepts only defined
primitive enums1..6, exact no-payload command shapes and known-present declaration
evidence. Unknown state remains synchronous. It never fabricates local success.

Successful non-recorded SetVertexDeclaration changes live evidence. SetFVF0
preserves it; nonzero successful FVF establishes a declaration. Recording updates
separate pending capture, leaving the live declaration unchanged. The actual
stateblock owns its metadata with its service ID/generation lifetime; no second
unbounded ID registry is introduced. Create ALL/VERTEX captures declaration;
PIXEL does not. Capture replaces only selected fields. Apply of captured NULL
preserves the existing declaration, as the pinned backend explicitly skips that
setter (stateblock.cpp48–80). Unknown Apply provenance invalidates evidence.

Reset boundaries conservatively invalidate live evidence rather than assuming
NULL. Pinned ResetState does not clear vertexDecl or m_recorder. Existing recording
state is retained; fresh successful setter/getter evidence restores eligibility.
No memory or native resource remains alive solely because of this shadow.

## Integration requirements

All outcome updates and draw admission must hold the same session serialization
lock. Updating after transact unlock races another thread's enqueue. Only exact
S_OK completes evidence transitions; malformed replies or unexpected success
codes cancel the session. Every object binding, lock, upload, stateblock operation,
Reset, release and other synchronous call drains queued draws in order.

The native service must independently verify actual declaration presence before
executing a draw-containing batch. That check must release any temporary owned
GetVertexDeclaration reference and must not consult guest pointers. A distinct
negotiated draw-policy capability is required before activation; existing batch
and constant-policy features alone do not authorize draw queueing.

## Retained host evidence

Portable normal and ASan/UBSan checks passed four commands. A separate native
fixture extension passed204 declaration/recording/Reset comparisons per process
in hardware, mixed and software creation modes, plus the existing real primitive
and indexed draws with changed-pixel readback. Its receipt freezes93 inputs and
nine successful commands. This supports the evidence model, without claiming
queued-session integration or console acceptance.

## Native comparison fixture

The native-command fixture adds direct/helper zero-primitive comparisons for all
six topology enums, with absent/present declarations, recorded null/non-null
changes, FVF0, Capture and typed ALL/PIXEL/VERTEX Apply. Early-invalid, late-failed
and successful Reset invalidate evidence; a native declaration getter restores
it. The original two real draws and changed-pixel readback remain and now compare
shadow eligibility against the actual draw HRESULT. All three creation modes
passed the retained native proof above; this extension does not test queued
sessions.

## Caller-owned stateblock evidence hook

With `PW_D3D9_ENABLE_STATE_EVIDENCE`, each local stateblock shell embeds an
extensible evidence aggregate. The required observed-call callback receives
that storage directly, eliminating a separate session ID registry. Create/End
allocate it before RPC; Capture/Apply pin the shell throughout the call; Begin
passes NULL. The callback must decode and validate typed replies, then update
device and block evidence under the same session gate before unlocking. It may
not retain the pointer. Disabled builds retain the original callback path.

Evidence disappears only with actual local shell cleanup, including deferred
cleanup. A balancing remote RELEASE is not a metadata lifetime event. Canonical
cache hits preserve the existing owner. After full typed validation, the observer
commits preallocated output into the cache while still holding the original
session gate. Commit takes the cache lock, pins and invalidates an existing alias,
or installs the new shell. No RPC, COM call or allocation runs in commit. The
caller publishes the committed result and balances extra remote references only
after unlocking. A later observer failure rolls back the committed ownership.
This also closes the gap where an earlier reply had not yet published its shell.
No runtime builder enables this hook in the foundation change.

The controlled hook fixture passed PE32 and PE64 with the hook disabled and
enabled: eight successful compile/execution commands and 93 frozen source/header
inputs. It covers alias publication before the first caller returns, invalidation
under the original gate, rollback after a later observer failure, failed End
recording preservation, and malformed/non-S_OK reply rejection. Earlier fixture
compile failures were retained separately. This is a controlled callback proof;
production session observers and queued draws remain separate integration work.
