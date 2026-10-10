# Vulkan stream gate and driver progress

The process stream gate protects owned command encoding, registry/template-cache
mutation and replay. It must not remain held while a driver waits for work that
another guest thread needs to submit or signal. Previously a legal timeline
WaitSemaphores/SignalSemaphore pair could time out or deadlock solely because the
waiting thread held this gate.

The explicit progress classifier drains all pending records with the existing
flush-only sentinel while holding the gate, then releases it and invokes the
ordinary Unix thunk with the original synchronous arguments. Replay completes
before shared scratch can be reused. Replay failure still terminates without
issuing the ordinary call or retrying. An empty drain adds no Unix crossing.
A nonempty drain and ordinary progress call use two crossings instead of one
piggyback; this trades a small crossing cost for correct concurrent progress.

The classifier includes fence/timeline/present waits (and existing aliases),
queue/device idle, swapchain acquisition, query results including polling forms,
profiling-lock acquisition, deferred-operation join, NV latency sleeps, queue
submission/bind-sparse/present and host timeline signals. Command-buffer wait
recording remains ordinary gated recording. This whitelist does not prove that
all unknown/vendor driver APIs are nonblocking; additional APIs require auditing.

Wait/progress operations have no template registration or retirement posthooks.
Vulkan object lifetime and per-object external synchronization remain the
application's responsibility. Independent queues/objects may progress concurrently;
the adapter does not impose a global completion order. Immediate destruction,
reset and cache operations retain their existing drain/dispatch ordering.
Existing debug/custom-allocator sticky-disable guards run before this policy.

Fallback accounting remains under the gate. Opt-in raw-crossing accounting is
atomic. A returned QueuePresentKHR briefly reacquires the gate for its cumulative
process diagnostics, so non-atomic totals are never sampled while producers can
change them. Present counters/results describe completed present invocations,
not guaranteed displayed frames. Other progress calls need no post-call snapshot.
Stats disabled adds no diagnostic crossing/counter update or return-side lock.
No stream ABI, capability name, Unix table entry or 64-bit PE route changes.

The actual i686 PE/Win32 concurrency fixture mocks only the Unix/driver boundary.
It tests all 21 classifier entries with pending and empty batches: an old owned
record must replay before a wait starts; another thread records fresh work,
flushes it and signals while the wait remains outstanding. Finite event waits
prevent a stuck test. The fixture checks exact ordering, original owned bytes,
raw error status and present device-loss preservation, opt-in crossing counts,
TLS retirement and unchanged reset/destroy classification. Existing on/off/old
Unix/stats and callback-reentry fixtures remain required. A separate frozen-source
control reproduced timeout only with batching ON. Neither host fixture establishes
the cause of GTA's PS5 menu-input regression or proves console performance.
