# Admitted D3D9 session observers

Scoped callbacks observe completed typed replies and accepted owned commands
inside the existing session admission gate. They may update caller-owned
metadata; they must not issue RPCs, COM calls, callbacks, or allocations.
A rejected admission does not invoke an observer. A completed reply is validated
before observation and before publication to its caller. Observer failure cancels
the session, and retained binding tickets retire outside the gate.

An optional owned GetTransform answer is admitted through the same gate. The
session checks callback depth, active thread, READY state, and sticky failure
first. With asynchronous mode enabled, a known answer can avoid flushing an
already accepted setter batch. Unknown answers follow the ordinary ordered
flush and native transaction. With asynchronous mode disabled every getter uses
the synchronous transaction. Deferred backend failure timing is the same as for
accepted queued setters; a known failed acknowledgement rejects later hits.

## Evidence and limits

The retained draw-client-control-r2 proof tested the unchanged session and fixture
bytes from commit 20de9bec. Its strict PE32 fixture completed compilation and
execution successfully. It covers original-gate callback ordering, callback
rejection, flush ordering, sticky failures, malformed replies, completed and
queued observer failures, outside-gate binding ticket retirement, and owned
matrix cache hit/miss/off paths. The controlled peer and matrix observer do not
establish actual native Transform or queued draw parity.

The draw-pair-compile-r1 receipt records successful full PE32 proxy and PE64
service compilation at the same source commit with draw capability 65536.
This change provides the scoped transport hooks; device-owned draw and Transform
observers, negotiated combined capability, and actual runtime evidence follow
separately. No console result is claimed here.

## Combined device integration

The device proxy owns declaration evidence and Transform state for its lifetime.
Successful typed replies and accepted queued commands update those structures
under the original session gate. State block evidence belongs to the canonical
state block shell; Create/End commit canonical identity before unlocking, and
aliases invalidate evidence conservatively. Failed End leaves recording
knowledge uncertain. Reset invalidates relevant cached live evidence while
preserving the pinned backend's recording semantics.

The paired build option `--draws` enables both draw admission and Transform
observers, adding capabilities 65536 and 131072 to the binding-enabled pair.
Both sides advertise exactly 260095. A different capability mask is rejected at
HELLO. Runtime batching and local getter serving remain controlled together by
`PW_D3D9_ASYNC`; its off mode keeps native getter round trips.

The frozen combined source 1e49a4d6 successfully compiled the complete PE32 proxy
and PE64 service with diagnostics disabled and mask 260095. Both binaries and
all 154 build inputs were independently reverified. Native service draw/pixel
parity and controlled observer evidence are prerequisites, not a claim that the
full combined runtime has passed. Combined runtime acceptance and its retained
receipts remain pending; no console result is claimed by this publication.
