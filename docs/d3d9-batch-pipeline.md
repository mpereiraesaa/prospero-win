# Bounded batch pipeline ledger

This portable prerequisite tracks eight published batches. It does not enable
pipelining, change the wire, enlarge rings, or alter shipping behavior.

The session admission lock serializes reserve/send/commit. Reservation copies
only metadata and does not advance either sequence. A failed unpublished send
aborts. A successful publication followed by wake/cancellation failure commits
ownership before cancellation. Eight credits remain below the wire's 32 pending
requests. Ring space is a separate constraint; eight full batches do not fit in
the current 8192-byte ring.

ACKs must match epoch, outer sequence/ticket, target/generation, opcode, payload
size, contiguous command range, exact HRESULT and attempted prefix. Validation
precedes output publication and FIFO retirement. Invalid ACKs poison the ledger
without retiring anything. A valid native failure ACK retires only that batch;
later published entries remain quarantined and the first failure is sticky.
Unacknowledged entries become eligible for cleanup only after service broker
join, supplied explicitly by the lifecycle owner. The helper does not itself
perform a join, release a resource, or prove that the caller's assertion is true.

Each returned entry identifies caller-owned resource ticket storage. The caller
must detach those tickets before reusing its slot, and invoke drops outside the
admission lock while retaining a session operation hold. Caller inputs/outputs
must not alias ledger storage. No allocation, callback, per-operation clock, or
large ticket-array clear occurs here. Only initialization clears the ledger.

The portable test checks FIFO wraps, full-credit backpressure, malformed ACK
atomicity, native failure prefixes, cancellation after publication and cleanup
rejection before join. Actual ticket drops, UI reentry, service failure-ACK
visibility and producer/service overlap require subsequent session integration
and native proofs; these are not claimed by this prerequisite.
