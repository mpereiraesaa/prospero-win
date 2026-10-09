# Controlled production client pipeline proof

This fixture compiles the production PE32 session, codecs and bounded ledger,
using a concurrent controlled wire peer. Private ticket acquire/drop callbacks
run through the actual session ownership machinery. The peer replaces native
COM execution; this fixture makes no GPU, console or performance claim.

The first execution is blocked while the producer publishes eight batches. The
ninth publication must wait for a credit. After release, the peer checks contiguous
command sequences and all commands before synchronous getter, device Reset,
Present, Release and STOP replies. Tickets must drop outside session admission.

A failure case publishes two batches, executes only the first two commands of
the first batch, and publishes the exact native failure prefix. A cached getter
must harvest this failure before invoking its local answer callback. Callback
and active-thread reentry are rejected. The peer then delays thread exit after
cancellation: tickets for its unexecuted second batch must remain pinned until
the actual broker thread joins. No timeout is treated as consumed work.

The runner takes an explicit frozen `--source-root`, hashes every linked source
and all local headers, records compile/runtime commands and terminal status,
and checks source immutability. Store outputs and the isolated prefix in the
persistent workspace artifact directory. A timed-out runtime requires inspecting
and cleaning up its own prefix before any further run.

## Retained controlled evidence

`pipeline-client-r8/receipt.json` under the persistent
`prospero-win-artifacts/bridge-recovery-20261009` directory is PASS:
fixture `d850ac07`, production source `92c35e5d`, four successful compile/run
commands, 111 source/header hashes and two PE32 binaries. Receipt SHA256:
`8674a315cf3174fe445e7ab08da84e4485ea8c1e739048fa3f7430e73338433e`.

In addition to the credit and barrier controls above, this run covers malformed
and out-of-order ACKs, ticket-block allocation failure, and a request wake failure
after actual wire publication. The companion fixture checks final-parent close
from real ticket callbacks on ACK and cancellation, repeated close rejection,
exactly one session free, and all 129 ticket drops.

Four cached-getter failures return failure without modifying caller output:
cancelled channel with no recorded batch failure, a negative answer callback,
an invalid hit value, and malformed answer payload. The retained
`pipeline-client-cache-negative-r1` run against `c3bf2495` reproduces the original
cancelled-channel bug: S_OK and changed output. The corrected production code
resets the tentative result after successful ACK harvesting.

Earlier retained failures are fixture issues: r1 omitted required evidence-family
compile flags; r2 demanded the original failure HRESULT from a later terminal
binding call whose existing contract is E_FAIL; r5 accidentally parked the fake
peer after HELLO. The r3/r4/r6 passes retain their narrower source/coverage scopes.
No failed receipt is presented as positive evidence.

The actual wire endpoint control publishes eight maximum-size 8,128-byte frames
into the 65,536-byte request ring. A ninth returns FULL without advancing the
send sequence or pending count; all eight are copied into 8,192-byte scratch
buffers and checked against their original payload. This is a wire capacity
control; the independent production client control proves its eight-credit gate.

Idle, callback and admitted-call cancellation controls block the peer while
checking that published tickets remain pinned. Cleanup completes only after the
peer exits and the session joins it. Mixed-version HELLO, real native COM
rendering, paired service overlap, PS5 behavior and performance remain outside
this fixture.
