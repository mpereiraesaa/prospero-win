# PE boundary for native asynchronous replay

The PE adapter first requests `__wine_pw_vk_batch_async_v2` with capability
`0x50570202`. Success selects batch entry version 3, whose records retain the
version 2 codec layout. Otherwise it probes the original version 2 capability
and preserves synchronous behavior. No new sentinel reaches an older backend.
The runtime owns these capability constants and must dispatch versions 1 and 2
synchronously even when native workers are available.

Version 3 always crosses the Unix boundary on a flush, including zero records.
An empty PE stream does not imply that previously transferred worker jobs are
finished. Ordinary fallback calls therefore retain a backend opportunity to
wait before destroying or mutating resources. The original `unix_count` flush
sentinel completes all outstanding jobs and is used for callback disable,
allocation-failure retirement and explicit completion boundaries.

Queue and GPU progress calls use `unix_count + 1` to transfer records without
waiting for unrelated native jobs. The backend must copy all records before
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
progress calls, including an empty second call. They assert admission does not
complete those jobs, raw progress executes with the PE gate released, empty
resource fallback reaches completion, and empty callback disable completes work
before callback reentry. Original version 2 and unsupported-backend cases remain
in the same suite. These tests exercise real Win32 APIs and PE structures; the
backend is mocked and full native scheduler validation is separate.
