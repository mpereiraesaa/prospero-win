# Implicit surface ownership protocol

The session adapter reserves outer opcode 32. Its implementation is gated by
`PW_D3D9_ENABLE_IMPLICIT`, which requires device/texture support and adds HELLO
feature bit 4096. Shipping builders enable it only after frontend integration;
unpaired feature masks are rejected before object publication.

Version 1 carries at most 16 generation-checked surface IDs. All integers are
little endian; fixed request/reply sizes are 144/152 bytes. No native pointer
crosses the protocol. Duplicate IDs, zero identities, oversized lists, nonzero
unused fields, unknown versions/operations and mismatched replies are rejected.
Decoding validates a temporary structure before changing the caller output.

- LIST returns the service's actual current implicit backbuffer/auto-depth IDs.
- PREPARE carries public-zero frontend shells to freeze for a pending Reset.
  Unexposed implicit service objects already have zero guest references.
- FINISH returns RESTORED or RETIRED and the current owner list. RESTORED means
  the pinned backend rejected Reset before removing the old owners. RETIRED
  means Reset succeeded or destroyed the old owners before failing. The actual
  Reset HRESULT travels in the ordinary device reply, separately from this
  ownership transaction status.
- DRAIN removes device-owned leases during final device teardown.

Only LIST and FINISH may return IDs. Only FINISH may return a disposition.
Failed control replies carry a failing HRESULT and no IDs or disposition.
An empty list is permitted; list bounds never justify silently truncating owners.

The adapter must validate ownership, epoch, Reset sequencing and refcount state;
wire validation alone cannot prove these. The production integration must reject
an unpaired peer in HELLO before creating objects. Backend storage, queue and
public references remain separate obligations.

`make build/host/test_d3d9_implicit_wire` builds the focused portable fixture;
it is also included in the normal and sanitizer host suites.

## Service transaction

Per-device phases enforce LIST/PREPARE, one Reset, FINISH and final DRAIN.
Prepare validates the frontend zero-public snapshot; the service parks those
native storage references immediately before Reset, including unexposed owners.
The exact backend outcome is captured inside the native adapter, before a later
window mirror failure can replace its HRESULT. A successful native Reset always
retires old ownership, even if the subsequent mirror reports failure. Preflight
rejection and pinned-backend early validation rejection preserve old ownership.

Service finish destroys retired zero-public entries before capturing new native
default surfaces, preventing reuse of an old identity when the allocator reuses
an address. Genuine public references retain their native storage pin. Creation
and successful Reset capture actual normalized backbuffer count and initial
auto-depth ownership; later bound surfaces are not classified as implicit.
Internal ownership failures cancel the session; they cannot return a partially
published device or a success reply with incomplete lifetime tracking.
