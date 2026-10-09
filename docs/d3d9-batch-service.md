# Ordered service batch execution

The opt-in compile flag `PW_D3D9_ENABLE_BATCH` exposes a serialized service
helper. The live transport does not call it yet. A future paired session must
negotiate the batch feature, initialize exactly one sequence state per session,
and drain all queued work before synchronous boundaries and object releases.

The helper owns and validates the entire batch and every method's eligibility
before making a native call. The first request starts at command sequence 1;
subsequent requests must continue the exact contiguous stream. The device's
generation is checked and one registry queue reference spans the whole batch.
Current eligibility excludes all commands with referenced objects. Adding
bindings requires their own lifetime proof; this helper does not grant it.

Native dispatch preserves record order. The first failing HRESULT stops further
execution and is returned with the exact attempted prefix and failing sequence.
The failure stays in session state; later requests cannot execute. A nonzero
success result such as S_FALSE is unexpected for eligible methods: it records
the unexpected result and fails the protocol rather than reporting S_OK.
Malformed batches, ineligible later records, duplicate/gapped sequences and
insufficient reply storage perform no native calls. Sequence exhaustion closes
the stream without wrapping. Every ordinary completion/failure releases the
device queue reference before returning. A stale device is a typed failure at
the first command's target-validation attempt.

The caller must preserve this terminal state, send valid failure acknowledgements
before cancelling, and wake all waiters. Reset, Release, Present, getters and
other synchronous operations still need client-side ordering integration. This
is not an asynchronous-success stub or a finished transport optimization.

`d3d9_batch_service.py` tests actual helper, registry, policy and codecs using a
controlled native dispatcher. Normal, sanitizer and PE64 runs check ordered
execution, every failure prefix, sticky failure, malformed/ineligible final
records, target kind/generation, S_FALSE rejection, sequence exhaustion and a
last guest release while the device is pinned. The fixture does not prove real
DXVK behavior, paired session ordering or console performance.
