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
