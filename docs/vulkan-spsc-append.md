# Producer-owned Vulkan record transport

The SPSC core is a dependency for removing the PE batching gate from append.
It is not connected to the Wine adapter yet and changes no runtime default.

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

## Remaining adapter work

Move encoder storage into the producer; protect immutable descriptor-template
snapshots and metadata retirement independently of replay; implement registry
publication and safe thread retirement; merge exact sequence prefixes; preserve
synchronous fallback, callback disabling, flush boundaries and fatal replay-error
semantics. This foundation alone does not remove the process gate or establish
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
