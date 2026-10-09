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


## Owned groups and ordered epochs

The replay core can own one chunk covering up to 256 distinct command-buffer
lanes. Each lane receives a completion marker for that chunk. Pure groups reserve
all their pool domains and may overlap disjoint groups. Ordered epochs cannot
pass earlier queued or active jobs, exclude other jobs while running, and prevent
later jobs from passing them. Every lane/pool consumer also observes the latest
ordered epoch, including descriptor-only epochs with no command-buffer lane.
Global completion includes such epochs. This permits descriptor updates and
recording to share a larger owned chunk without weakening update ordering.
The core fixture blocks preceding, ordered and following jobs and checks group
markers, descriptor-only consumer dependencies, duplicate lanes and bounds.

A pure group blocked on one pool also reserves its position in every other pool
it touches: later overlapping groups cannot pass it. The fixture repeats the
blocked preceding/group/following sequence without an ordered epoch to check
this multi-pool dependency and monotonic lane completion.


## Physical pools and initialization exclusion

The `fanout` fixture mode places two physical replay domains inside one logical
command pool. It proves overlapping work on the separate physical slots, strict
exclusion within each slot, a command-buffer wait that does not wait for another
slot, and a logical pool wait that includes every slot. The real adapter checks
original WOW64 argument prefixes before permitting optional pool growth; raw,
legacy, callback-allocation and unknown-chain cases remain excluded.

The `init-race` mode pauses worker construction while another API thread enters
a raw lifecycle hook. The hook must wait for initialization under the admission
mutex. A negative build removes that mutex acquisition and must fail at the
premature waiter-completion assertion. This verifies the initialization fix
rather than relying on a probabilistic stress race.

Positive host and ASan/UBSan runs plus the missing-lock negative are recorded in
`/tmp/prospero-replay-fanout-async-r2/receipt.json`. Actual staged native allocation,
reset, trim, free and destroy coverage and the immutable combined candidate are
listed in [the fanout evidence](vulkan-replay-pool-fanout.md#combined-candidate-evidence).
The candidate is not a console acceptance claim. Keep the runtime and fanout
changes in draft until the owner reviews matched console results.


## Adapter descriptor epochs and measured handoffs

The adapter keeps contiguous reviewed recording and descriptor updates in one
owned group, up to 256 distinct buffer lanes. A descriptor update makes the
whole group globally ordered. Unknown records still flush the group, wait for
all replay work, and execute on the caller. Destructive raw fallbacks retain the
same completion barrier. Callback disable/retirement uses the existing complete
flush; the admission-only sentinel does not replace it.

Only the inspected direct native implementations of `vkUpdateDescriptorSets`
and `vkUpdateDescriptorSetWithTemplate` qualify. The classifier verifies the
device unwrap helper and exact direct-call body. The KHR template thunk currently
contains Wine `TRACE` and remains synchronous. Allocation callbacks and guest
callbacks still require the existing quiesce-and-disable protocol; this change
does not enable workers in a callback-unsafe session.

Template ownership is already encoded by `pw_vk_wire_template`: supported
entries contain descriptor values and handles, not retained source pointers;
holes are zeroed and unsupported entry types fail encoding. Manual replay copies
the inline payload into worker-local storage. The generated template codec
snapshots the same payload during encoding and reconstructs `pData` in its own
decode arena. Other generated descriptor pointer graphs use the existing offset
codec. Admission retains no decoded pointers: the immutable wire bytes are
copied into the job and decoded again on the worker. Template/layout/resource
destruction cannot pass earlier epochs because unknown/destructive fallbacks
wait globally. Lane and pool consumers observe the ordered epoch even when it
contains no recording commands; buffers without a lane use the pool barrier.

The actual-adapter fixture builds the prior adapter from integration `9896ac5`
and the new adapter against the same scheduler and controlled driver. For 24
alternating recording/template-update fragments (48 records):

| Policy | Jobs | Admission global-wait calls |
| --- | ---: | ---: |
| Previous adapter | 24 | 24 |
| Owned ordered group | 1 | 0 |

The new group is completed at the actual consumer boundary. Condition-variable
wait counts depend on scheduling and are not the deterministic comparison. This
is a handoff count proof, not a console FPS claim. Globally ordered groups may
serialize across otherwise independent descriptor users; pure disjoint groups
still overlap. Empty flush elision and physical pool fanout alone do not remove
the old descriptor-update handoffs.

Run the lab with `--baseline-adapter` pointing to the previous actual source for
the comparison. Host and ASan cases clobber the source immediately after
admission, verify all descriptor/recording values in order, hold the worker while
a destructor waits, and check a malformed descriptor-only epoch cannot return
success through a buffer consumer with no lane. The generated classifier is
checked against the real pinned Wine headers; generated codec execution remains
covered by its separate PE lab, not the manual-codec adapter fixture.

Local receipts: `/tmp/prospero-replay-epochs-async-r6/receipt.json` and
`/tmp/prospero-replay-epoch-negatives/receipt.json`. Removing the queued overlap
check or ordered-epoch dependency makes the blocked core fixture fail. These
changes require fresh staged PE/SDK checks and owner-approved console comparison;
the previously rejected one-worker package stays immutable and is not retried.
