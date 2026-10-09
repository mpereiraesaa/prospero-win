# Implicit surface ownership protocol

This codec is a foundation for a paired proxy/service implementation. It does
not change shipping ownership or enable a new transport opcode by itself.

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
