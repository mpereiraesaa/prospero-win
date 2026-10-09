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
