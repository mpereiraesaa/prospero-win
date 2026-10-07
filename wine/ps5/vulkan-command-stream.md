# 32-bit Vulkan command stream transport

This is the first dependency of the Vulkan batching implementation. It supplies
owned record storage and ordered collection/replay. It is not wired into Wine,
does not enqueue Vulkan calls yet, and has no console or performance acceptance.

The producer appends normalized payload bytes into a bounded per-thread arena.
Append copies its input immediately. The future Vulkan codec must deep-copy
arrays, descriptor template data and supported extension chains before append;
the generic transport cannot determine whether an opaque payload embeds a pointer.
Unsupported arguments must drain pending work and use the ordinary thunk.

A stream stores its owning registry; append checks that ownership in constant
time instead of walking the producer list on every hot call. Only register and
unregister mutate that ownership under caller serialization.

Each append receives a process-wide sequence under caller serialization. A drain
merges every registered arena by that sequence, preserving cross-thread handoffs.
Draining registry-list order would reorder an update on thread A and a bind on
thread B. Every unbatched Vulkan operation must globally drain before execution,
including reset/free/destroy calls; flushing only the calling thread is unsafe.

Only byte records cross the 32/64-bit boundary. Registry and stream structs are
local producer state. Headers encode magic, protocol version, total byte length,
explicit opcode, payload length, reserved zero and sequence in little endian.
Opcodes for the codec must be stable protocol values, not generated Wine Unix
enum indices. Records align to eight bytes and have zero padding. The replay
side validates all framing and ordering before invoking a callback; the codec
must separately validate all operation payloads before any Vulkan side effect.

Full arenas return FULL without changing sequence or queued bytes. The adapter
then flushes and retries enqueue once; oversized or unsupported records use the
ordinary thunk after the flush. Invalid drains preserve pending records and may
have modified scratch. Scratch and registered arenas must be disjoint; stream
nodes start zero-initialized and belong to exactly one registry. Unregister
refuses nodes with pending work. TLS teardown must drain/remove a node before
freeing its arena. Sequence exhaustion fails instead of wrapping.

A successful collection owns an immutable scratch batch until replay returns.
Registry locking and replay serialization are separate: acquire the replay gate,
lock the registry, collect, unlock the registry, replay and execute any triggering
ordinary operation, then release the gate. Never wait for the replay gate while
holding the registry lock. Producers can append newer work during replay, but
later drains and immediate operations cannot overtake the in-flight batch.
Callbacks cannot mutate or retain batch storage or reenter collection/replay.
Making the replay gate recursive does not prove callback order. Initial adapters
must disable batching for registered Vulkan debug callbacks and explicitly guard
reentrant replay until a tested callback policy exists.

Replay failure reports how many records completed. A partially executed batch
must never be retried or resubmitted via fallback. The adapter needs a fatal
failure path with the record index; void API calls do not make replay errors safe
to ignore.

Run `python3 tests/test_vk_command_stream.py --output /tmp/pw-vk-stream-tests`.
The fixture checks copied payload lifetime, interleaved global order, concurrent
producers under the registry mutex, malformed-batch preflight, failed replay
prefixes, capacity/overflow behavior, alias rejection and stream rundown. It runs
with ASan/UBSan and compiles the actual transport for i686 Windows. The x86 check
is compile-only; it does not prove Wine or console integration.

Remaining dependencies are the top-category Vulkan codecs (including template
creation metadata), PE enqueue/flush lifecycle and a versioned Unix replay entry,
then translator-first measurements and native crossings at actual present
boundaries. Console validation must use verified-medium GTA SA, GTA IV and HL2,
with fixed-point visual comparison against the unbatched build.

Descriptor-template lookup uses a fixed 1024-bucket hash of the client device
and both halves of the full 64-bit template handle. Lookup visits only its
collision chain; it neither scans all templates nor allocates or rehashes.
The index costs 4 KiB in PE32 (8 KiB on a 64-bit host), plus a count field and
owned metadata per template. Chains have no template-count limit; collisions
are checked against the exact device and full handle. Expected lookup cost is
constant at ordinary load; worst-case adversarial collisions remain linear in
one bucket. Device teardown deliberately scans all buckets, outside the hot
update path. All existing caller locking and drain requirements remain.

`tests/test_vk_wire.py` verifies three forced collisions with head/middle/tail
removal, 4096 live templates across devices, full-width handle identity and
successful same-handle reuse on allocator failure. Successful unsupported or
failed-allocation metadata replaces stale eligibility; failed Vulkan creation
preserves the existing entry. These are host correctness checks, not a claimed
FPS gain or target benchmark.
