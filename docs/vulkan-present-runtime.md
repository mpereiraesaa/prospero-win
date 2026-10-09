# Shared PE32 and PE64 Present timing

`PW_VK_BATCH_STATS=1` enables the same CPU queue-return interval accumulator in
both Wine Vulkan PE architectures. The PE64 path is used by the native D3D9 bridge;
the PE32 path remains available for matched baseline captures. Other PE64 Vulkan
calls retain their direct Unix thunk. The wrapper forwards the original arguments
and status exactly once, captures QPC immediately after Present returns, then
updates diagnostic state under a separate lock.

`PW_VK_PRESENT_INTERVAL version=1 scope=queue_return` reports queue, timestamp,
frequency, actual result/status, interval validity, microseconds, cumulative
interval count, maximum and strict counts above 25/33/50 ms. These are CPU return
intervals, not scanout, GPU completion or end-to-end frame latency. Failed calls
re-prime the next sample; undefined result output is never read after a failed
Unix call. QPC failure and successful device destruction clear all queue histories
conservatively. Eight queues are supported; excess queues emit a capacity marker.

The PE32 batching path already flushes before presenting. Device destruction that
is piggybacked on a batch clears the accumulator after successful dispatch too.
No replay worker or admission policy changes are part of this integration.

The controlled fixture `tests/lab/vk_present_pe.py` builds and executes the actual
wrapper as PE32 and PE64 with real Win32 synchronization and a controlled thunk
and clock. It covers enabled/disabled diagnostics, argument and status forwarding,
failed-output preservation, thresholds, suboptimal success, error recovery,
capacity, clock failure and device lifetime reset. It does not prove actual
Vulkan presentation, console cadence or game performance. Final integration must
build against pinned Wine headers and deploy both PE DLLs, and matched captures
must use the same instrumentation on both sides.

## Present carried by a batch

A PE32 flush can carry Present inside the batch Unix call. Its outer opcode is
not Present, so the direct wrapper cannot observe that completed tail itself.
After batch dispatch succeeds, the flush now observes the tail's actual status
without dispatching Present again. Standalone flushes are ignored; direct calls
retain one observation. Batched device destruction uses the same completion path.

The controlled fixture exercises both direct calls and an already-completed batch
tail, including nested failure and recovery. `--host-control` runs the actual
observer with scalar Win32 API stubs and a controlled thunk/clock, without Wine
or GPU initialization. It does not prove Win32 synchronization or real batch/GPU
execution. PE32/PE64 compile checks and later console integration remain distinct.
