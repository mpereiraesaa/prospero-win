# Native command-buffer pool membership

The batch staging path gives each Wine native command buffer a private wrapper
containing its original `vulkan_command_buffer` prefix, its owning native command
pool, and an initially null replay-lane pointer. Shared Wine headers, client
handles and the PE/Unix wire format retain their existing layouts. The private
wrapper is allocated by the original Wine allocation function and freed by the
original cleanup paths.

This metadata is a prerequisite for worker synchronization. Vulkan requires a
command pool to be externally synchronized even when recording different command
buffers allocated from it. A later worker adapter must wait for the pool before
begin/end/reset/free/trim/destruction, retain wrappers until jobs finish, handle
secondary and submit dependencies, and drop replay lanes before freeing wrappers.
This change does not start workers or make dispatch asynchronous.

`tools/stage_vk_replay_lifecycle.py` performs checked transformations before the
normal batch generator runs. The lab fixture `tests/lab/vk_replay_lifecycle.py`
takes source/build/output paths, extracts the transformed Wine allocation, free,
create-pool and destroy-pool function bodies, and compiles those bodies with real
Wine headers and controlled driver callbacks. It checks primary/secondary
membership across two pools, wrapper-prefix identity, zero-initialized lanes,
reuse, driver/allocation failures after partial success, and balanced cleanup.
It runs normally and under ASan/UBSan without a driver or console. Wine debug
macros are disabled in this fixture; it does not test native worker TLS setup.

Reference: [Vulkan command pools](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html#commandbuffers-pools).
