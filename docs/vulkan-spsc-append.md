# Producer-owned Vulkan record transport

The SPSC core is a dependency for removing the PE batching gate from append.
The PE adapter integration is under validation; no console performance gain is
claimed.

Each producer owns a bounded byte ring with power-of-two capacity. One serialized consumer copies records
into its own scratch and releases capacity only after copying. Release/acquire
publication protects payload bytes in both directions. Ring wrap preserves the
existing 32-byte record framing; the consumer validates framing before release.
Vulkan semantic preflight must still validate the complete owned batch before any
native dispatch.

All producers share an atomic sequence counter. Reservation precedes publication;
a preempted producer can leave a hole. The consumer requests the exact next global
sequence, scanning every registered producer. PENDING must never be interpreted
as permission to skip a sequence. A drain marker snapshots the highest reserved
sequence and must wait for its entire prefix. Appending a later sequence does not
wait for an earlier producer or for a foreign drain. Capacity exhaustion affects
the producer whose ring is full. Counters stop before integer wrap.

The core rejects targets without lock-free 64-bit atomics. It allocates no memory,
uses no mutex, and executes no guest or driver code. The adapter must publish
registry membership before reserving a sequence and keep nodes alive until their
producer has stopped and consumer has drained them. No reset/reinitialization is
allowed while either side can access a ring. The consumer must supply scratch
separate from every ring and control object. Full/error results consume nothing.

## Adapter obligations

Keep encoder storage in the producer; protect descriptor-template
snapshots and metadata retirement independently of replay; preserve registry
publication and safe thread retirement; merge exact sequence prefixes; preserve
synchronous fallback, callback disabling, flush boundaries and fatal replay-error
semantics. The portable core alone does not establish adapter correctness or
performance gains. Parallel host workers additionally require command-pool
ownership and buffer lifecycle synchronization.

## Tests

`make all` and `make sanitize` run `test_vk_spsc`. The fixture exercises wraparound,
owned copies, capacity preservation, sequence exhaustion, an unpublished reservation
that does not block later append, and three concurrent producers with 30,000
records consumed in global order. It does not exercise the Wine adapter or GPU.

The same fixture supports native Win32 threads when compiled as PE32:

```sh
i686-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -I. \
  tests/test_vk_spsc.c wine/ps5/pw_vk_spsc.c wine/ps5/pw_vk_command_stream.c \
  -o /tmp/test_vk_spsc.exe
wine /tmp/test_vk_spsc.exe
```

This checks actual PE32 atomics and Win32 thread scheduling without Wine Vulkan
or a graphics driver. The fixture also rejects malformed framing and verifies
that failed reads do not reclaim ring storage.

## PE adapter integration (in validation)

The PE adapter now owns encoder storage per producer and merges SPSC records
through a reserved-sequence marker into replay scratch. Ordinary append does not
enter the replay critical section. A separate short registry lock protects node
publication and reclamation, and template snapshots use a separate metadata lock;
neither is held across native replay. A producer's full ring may request a drain.

Callback/allocator disabling first blocks new publication and waits for active
publishers, then captures and drains the completed prefix before making raw
callback dispatch available. A producer releases its active state before asking
for a full-ring drain, avoiding a cycle with a disable owner. Enqueued statistics
now describe the accepted prefix at the drain marker, rather than racing a shared
increment on every append. Client allocations retire only after owned replay.

The actual PE fixture holds a foreign thread inside mocked Unix replay and
requires another producer's append to return before releasing it. The later
record must remain pending until a subsequent drain. Existing template ownership,
thread retirement, sticky callback, synchronous status and progress-API fixtures
remain required. These fixtures do not establish driver correctness or console
performance; the integration is not yet an accepted runtime change.
