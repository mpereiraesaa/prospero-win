# Native replay dispatch classification

`tools/generate_vk_replay_dispatch.py` reads the exact staged Wine
`vulkan_thunks.c`, `loader_thunks.h`, and `include/wine/vulkan_driver.h`.
It emits `pw_vk_replay_dispatch.h` and a JSON eligibility manifest. A partial
source stage can supply its matching driver header through `--driver-header`.
Regenerate after Wine thunk regeneration; the output depends only on the input
files and uses sorted names, with no timestamps or historical opcode constants.

The generated `pw_vk_replay_command_buffer(code, decoded_native_params)` returns
the command-buffer handle for an eligible native recording call, or zero. It
reads the named member of the real native parameter structure. It must only be
called with fully decoded native parameters. It is disabled without `_WIN64`:
32-bit conversion thunks have a different ABI and may allocate or invoke Wine
helpers. The caller must retain the decoded arena and all referenced Vulkan
objects until replay finishes.

Eligibility requires the complete native thunk to consist of one parameter cast
and one device-driver call. Every argument must be a direct parameter member;
the only accepted helper is `vulkan_command_buffer_from_handle`. Its entire
body is audited against the pinned implementation: cast to the client object,
load `unix_handle`, cast to the native command-buffer object. There is no Wine
TLS, allocation, tracing, conversion, or callback wrapper in this path. Changes
to that helper fail generation. Other thunk changes exclude that command.
Secondary execution and diagnostic/debug commands are explicitly excluded even
when their current bodies have the direct-call shape.

This classification proves the Wine thunk path only. It assumes the native
Vulkan driver supports externally synchronized command recording on application
pthreads. Driver debug callbacks must not call guest code from these workers;
the runtime must disable worker replay for instances with guest callbacks or
provide a separately reviewed callback marshaler. Command-pool synchronization,
resource lifetime barriers, and secondary/submit dependencies remain the runtime
adapter's responsibility. Eligibility alone does not establish concurrency or a
performance improvement.

Run focused parser and native ABI checks with:

```sh
python3 tests/test_vk_replay_dispatch.py --source /path/to/pinned/wine
```

The fixture compiles against actual Wine native parameter declarations and
checks handle extraction for every eligible type, unknown codes, null input,
secondary execution, and the disabled ABI path. Synthetic mutations add tracing,
conversion calls, helper changes, and a wrong member type to ensure rejection.

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
