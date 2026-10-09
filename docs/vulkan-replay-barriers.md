# Native replay lifetime and submit barriers

`tools/stage_vk_replay_barriers.py` applies checked transformations after
`stage_vk_replay_lifecycle.py`. The runtime adapter provides the declarations in
`pw_vk_async.h` and implements the four synchronization helpers. This staging
change alone does not enable replay workers.

| Operation | Dependency before native dispatch |
| --- | --- |
| Begin, end, reset command buffer | All previously admitted jobs for its command pool |
| Reset or trim command pool, including KHR alias | All jobs for the named pool |
| Allocate, free or destroy pool buffers | All jobs for the named pool |
| Free individual native wrapper | Wait and remove its replay lane before driver free and wrapper free |
| Execute secondary command buffers | Primary pool, followed by each referenced secondary buffer lane |
| QueueSubmit, QueueSubmit2, QueueSubmit2KHR | Only each explicitly referenced command buffer lane |

Submit hooks traverse original client handles before Wine converts arrays to
host handles. Both native and WOW64 thunks are covered, including the nested
`VkCommandBufferSubmitInfo` array. Empty submit lists do not wait. Ending a
command buffer remains synchronous, preserving the driver's result and error
contract. Same-pool command buffers remain serialized by the scheduler's pool
domain; separate pools can replay concurrently.

The runtime admission mutex serializes lane lookup, ticket snapshots and job
admission. Helpers release that mutex before the native Vulkan driver call.
Vulkan's existing external synchronization requirements prohibit the application
from concurrently recording/resetting/freeing the same command buffer or pool.
The adapter must release its admission mutex before an inline dispatch that can
reach these hooks; workers must not execute these lifecycle thunks. Runtime
failure is sticky and cannot allow a lifetime operation to proceed with queued
jobs still using the object.

All other resource destruction and synchronous fallback operations need the
runtime adapter's conservative global dependency barrier. This file supplies the
command-buffer and pool hooks; it does not itself classify every Vulkan call or
make native worker threads safe for Wine callbacks.

## Host verification

Run against the pinned original Wine source and the same registry XML used for
codec generation:

```sh
python tests/lab/vk_replay_barriers.py \
  --source /path/to/wine/source \
  --vk-xml /path/to/vk.xml --video-xml /path/to/video.xml \
  --output /tmp/vulkan-replay-barriers
```

The lab checks deterministic staging, rejects repeated staging, checks native
manual-hook order, and regenerates the entire real `vulkan_thunks.c` byte for
byte using the patched Wine generator. A compiled fixture extracts all twenty
injected thunk prefixes and checks pool versus lane dependencies, multiple
submits, all three submit APIs, secondary buffers and both pointer widths under
host C and ASan/UBSan. A negative control deletes a wait hook and must fail its
assertion. The fixture uses controlled pointer mapping for WOW64; the complete
staged Wine build and console acceptance remain integration checks.
