# Async Vulkan adapter host fixture

Run the adapter fixture against the exact repository and full pinned Wine tree:

```sh
python3 tests/lab/vk_replay_async.py --source /path/to/wine \
  --build /path/to/configured/wine-build --output /tmp/async-replay-check
```

`--runtime-root` optionally selects a separate repository worktree. The runner
copies its actual Unix adapter, uses real Wine parameter/object declarations,
and links the real stream framing, manual Vulkan wire codec, and pthread replay
scheduler. A controlled native driver receives push constants. Client objects
and batch buffers occupy real low addresses to exercise the batch ABI's 32-bit
pointers; the test does not invent invalid command-buffer objects.

Two independent command pools hold both workers inside driver callbacks at the
same time. The test overwrites all producer input after each async return, then
checks values only after releasing those callbacks. It checks per-buffer order,
exclusion between two buffers from one pool, and a target buffer wait completing
while an unrelated pool remains blocked. Pool wait and lane removal must remain
blocked until their real driver callbacks finish. Recreating a removed lane
checks reuse and final scheduler statistics, including both workers doing work,
peak concurrency of two, and no remaining owned bytes.

The async contract requires version 3 admission and its `unix_count + 1`
admission-only sentinel. Version 2 rejects that sentinel. A separate process checks that
version 2 remains synchronous and does not start workers. Another process feeds
a deliberately malformed owned stream to a valid lane, then calls the actual
buffer wait hook. Success requires `SIGABRT` and the replay-failure diagnostic;
returning from that lifecycle hook after failed replay is rejected.

Both the ordinary and ASan/UBSan builds must pass. The receipt contains command
outputs and SHA-256 identities. This fixture exercises manual recording codecs;
its generated decoder deliberately returns failure and cannot fake successful
generated dispatch. The existing generated-codec and classifier fixtures cover
that separate path. The fixture calls the adapter's scoped wait hooks directly;
it does not claim to test every patched Wine submit/lifecycle call site or any
console driver behavior.


## Opt-in startup localization

`PW_VK_REPLAY_TRACE=1` enables native stderr records before and after worker
creation, on worker entry, and around the first eight jobs and admissions. Each of the first eight
worker callbacks traces up to 32 record dispatches, tagged by callback ID, record
number, command buffer and pool; unrelated jobs cannot consume this local bound. The default emits none. These traces use native `fprintf`
and do not call Wine logging or guest TLS helpers. A completed create followed
by a job/dispatch begin without its matching end narrows the next investigation;
it does not by itself establish a driver, stack, or scheduler diagnosis.

The original diagnostic retained the pthread default stack. Opt-in tracing queries a fresh
pthread attribute object for its default stack size; query failures are reported
and do not alter the configured stack choice. This reports the default attribute value, not
a measured live worker stack extent. The SDK title stub exports the three
standard attribute APIs used; no Linux-only stack query API is assumed. The host regression captures stderr and checks exact
startup counts and the eight-job bound across twenty jobs, plus default silence.
The prior immutable console package is unchanged; the two-worker startup process
failure requires a separately identified diagnostic package and console review.

The trace bounds fixture replays ten jobs of forty records and checks exactly
32 begin/end record pairs for each of the first eight jobs, with no ninth-job
dispatch trace. A prior diagnostic observed both workers enter and multiple
driver calls complete, so thread construction alone does not explain the console
startup failure. Missing final stderr records still require cautious interpretation.


## Explicit replay-worker stack candidate

Replay workers request a fixed 1 MiB native stack, bounded by the existing
maximum of eight workers (at most 8 MiB requested stack space, excluding guards
and pthread bookkeeping). The console default attribute was observed as 64 KiB.
The adapter's manual replay path alone uses over 8 KiB of local stack before
entering the driver; 1 MiB provides headroom for driver recording call chains and
matches the scale already used for native service threads. This is an initial
bounded engineering choice, not a measured worst-case driver requirement or a
claim that stack exhaustion caused the startup failure.

Attribute initialization or size-setting failure prevents worker creation.
Partial pthread creation and attribute-destruction failure stop and join every
started worker before returning failure. The host fixture checks the exact
attribute size on every worker creation and injects init, set, destroy and
partial-create failures. Trace records retain the observed default size and
report the configured size and setup results. The candidate requires separate
one-worker and then two-worker console validation; previous trace packages stay
immutable.
