# PE boundary for native asynchronous replay

The PE adapter first requests `__wine_pw_vk_batch_async_v2` with capability
`0x50570202`. Success selects batch entry version 3, whose records retain the
version 2 codec layout. Otherwise it probes the original version 2 capability
and preserves synchronous behavior. No new sentinel reaches an older backend.
The runtime owns these capability constants and must dispatch versions 1 and 2
synchronously even when native workers are available.

Version 3 completion and resource fallback flushes cross the Unix boundary
even with zero records. An empty PE stream does not imply that previously
transferred worker jobs are finished. Ordinary fallback calls therefore retain
a backend opportunity to
wait before destroying or mutating resources. The original `unix_count` flush
sentinel completes all outstanding jobs and is used for callback disable,
allocation-failure retirement and explicit completion boundaries.

Queue and GPU progress calls use `unix_count + 1` to transfer records without
waiting for unrelated native jobs. If prefix collection yields zero records,
this admission-only crossing is skipped: there are no bytes to transfer, and
the following raw progress call retains its native dependency hooks. Collection
still waits for reserved but unpublished tickets in its prefix. The ordinary
`unix_count` completion sentinel is never skipped for an asynchronous backend.
The backend must copy all records before
returning, must not dispatch that sentinel as a function-table index, and must
not retain shared PE scratch. PE then releases its replay gate before invoking
the raw progress call. Native submit hooks wait only the command buffer lanes
referenced by that call. Other progress calls retain the driver's normal
synchronization. Guest callback registration first quiesces all producers and
completes every native job; sticky disable is published only afterward.

Client wrapper retirement remains after its corresponding native destruction.
Native command buffer/pool free hooks wait before dropping the native wrapper,
and other resource destruction uses the backend completion barrier. Merely
admitting records does not authorize destruction to bypass those hooks.

## Host verification

`tests/lab/vk_batch_runtime.py` includes the `async-lifetime` and `async-disable`
PE fixture modes. Their mocked backend retains outstanding jobs across two
progress calls, including an empty second call. They assert the second progress
call adds no crossing and admission does not
complete those jobs, raw progress executes with the PE gate released, empty
resource fallback reaches completion, and empty callback disable completes work
before callback reentry. Original version 2 and unsupported-backend cases remain
in the same suite. These tests exercise real Win32 APIs and PE structures; the
backend is mocked and full native scheduler validation is separate.

## Native adapter

`PW_VK_REPLAY_THREADS` selects 0 (synchronous), 1, or 2–8 host workers; the
initial default is 2. Only version 3 enables them. Versions 1 and 2 preserve
synchronous completion, including when an async client already initialized
workers in the process. Invalid configuration or startup failure fails the
batch rather than silently changing the requested mode.

The native entry preflights the entire batch, groups consecutive recording
commands for the same command buffer, then copies those chunks into a bounded
32 MiB scheduler. Each worker owns its decoder arena. It only calls reviewed
direct driver wrappers and the existing manual codecs; unsupported commands
and resource mutations wait globally and run on the calling Wine thread.
Per-pool exclusion preserves native command-pool synchronization. Lifecycle
hooks wait or drop lanes before reset/free/destroy; submits wait only referenced
buffers. Secondary execution completes the dependencies synchronously before
recording the parent command.

An admission mutex protects lane ownership and worker initialization. Workers
never acquire it, invoke guest code or enter Wine helpers requiring thread TLS.
Callback/layer/allocator guards permanently disable PE batching after completing
outstanding work. An old PE caller cannot accidentally opt into this mode.

With `PW_VK_BATCH_STATS=1`, bounded `PW_VK_REPLAY` reports include jobs per worker,
peak simultaneous jobs, owned bytes and waits. The initial pool-affinity adapter
may serialize DXVK buffers sharing a logical graphics pool; measured worker
activity, FPS and profiler results are required before claiming gains. Separate
pool fanout is not enabled here. No console performance result is implied by
the host concurrency tests.

## Empty-admission regression evidence

The full actual PE suite passed with the optimized path in
`/tmp/prospero-replay-fanout-pe-r1/receipt.json`. Its async cases retain pending
mocked native jobs across two progress calls while observing only one admission
crossing. A subsequent empty resource or callback-disable boundary still reaches
the backend and completes those jobs.

The negative control in
`/tmp/prospero-replay-fanout-build/negative-admission/receipt.json` changes the
optimization to skip every zero-byte flush. Both `async-lifetime` and
`async-disable` reject it at `!async_pending`, with the expected exit code 3.
The immutable combined package and SDK checks are recorded in
[fanout evidence](vulkan-replay-pool-fanout.md#combined-candidate-evidence).
