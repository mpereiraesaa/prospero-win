# Optional native command-pool fanout

Set `PW_VK_REPLAY_POOL_FANOUT=1` with `PW_VK_REPLAY_THREADS` set to at least 2
to allow a logical Wine command pool to own several physical driver pools.
The default is one physical pool. Fanout additionally requires a callback-safe
version 3 batch entry that creates the pool. Raw PE64, legacy batching, disabled
batching, non-null creation `pNext`, and allocation callbacks retain one pool.
The original WOW64 creation prefix is checked before Wine conversion can discard
an unknown extension chain.

The fixed bound is the configured worker count, at most eight physical pools
per logical pool. Extra pools are created lazily during allocation, and only
inside an active version 3 allocation dispatch. New buffers are assigned round
robin; their physical slot stays fixed until free. Primary and secondary levels,
queue family, creation flags, allocation chains and original public handles are
preserved. Extra pool memory failures reduce the bound to the slots already
created, without retrying growth on every allocation. Other creation failures
propagate through allocation failure and its normal partial-buffer cleanup.
Physical pools survive buffer allocation failures for bounded reuse and are all
destroyed with the logical pool.

Each physical slot is a separate replay synchronization domain. Begin, end and
reset of a command buffer wait its physical slot. Logical pool reset, trim,
free and destroy wait all slots. Reset and both trim entry points dispatch to
every physical pool with the original flags; reset reports success only if all
physical calls succeed. Free uses each native wrapper's physical slot, including
partial allocation cleanup. Extra native pool handles have object-map aliases
that translate back to the original client handle for diagnostics without
changing that client's `unix_handle`.

`PW_VK_BATCH_STATS=1` emits one `PW_VK_REPLAY_POOL` line for logical creation,
each successful extra slot and a failed growth attempt. The line identifies the
logical pool, requested flag, actual slot count, current bound and driver result.
This shows whether console execution actually created additional physical pools.

The existing Vulkan external synchronization requirements still apply. No
command buffer moves between pools, no driver command is replayed concurrently
with another command in the same physical pool, and the number of physical pools
does not grow with command-buffer count. Secondary execute remains synchronous
and waits the referenced secondary lanes before recording their dependency.

## Host checks

`tests/lab/vk_replay_fanout.py` transforms the real pinned Wine lifecycle bodies
and regenerates the entire thunk file byte for byte. It compiles and executes
those bodies against a controlled native driver with ASan/UBSan. Checks cover
bounded allocation, native aliases, primary/secondary buffers, unknown chains,
allocator exclusion, partial allocation and driver failures, extra-pool memory
failure, reset error propagation, both trim aliases and exact destruction.
A negative control must detect freeing a buffer through the wrong physical pool.

The `fanout` mode of `tests/lab/vk_replay_async.py` runs the real scheduler and
adapter with two physical slots inside one logical pool. It checks overlap,
same-slot exclusion, physical versus logical wait scopes and the original
WOW64 eligibility checks. A staged native/PE build and console measurements are
required before enabling this option for normal use.

## Combined candidate evidence

These receipts describe the combined candidate at `abae8cb`, including the
separate runtime wiring and empty-admission changes. Staging alone does not
enable this feature. Console results remain pending review.

| Check | Local receipt | Result |
| --- | --- | --- |
| Native lifecycle, regeneration, ASan/UBSan and wrong-pool-free negative | `/tmp/prospero-replay-fanout-lab-r2/receipt.json` | Passed |
| Actual scheduler, physical/logical scopes, init exclusion and missing-lock negative | `/tmp/prospero-replay-fanout-async-r2/receipt.json` | Passed |
| Full PE modes, legacy compatibility, empty barriers and generator equality | `/tmp/prospero-replay-fanout-pe-r1/receipt.json` | Passed |
| Exact pool telemetry event counts with sanitizers | `/tmp/prospero-replay-fanout-telemetry-lab/receipt.json` | Passed |
| Mutant that skips mandatory empty barriers | `/tmp/prospero-replay-fanout-build/negative-admission/receipt.json` | Both resource and callback cases rejected it |
| SDK native and PE build, control comparison and import audit | `/tmp/prospero-replay-fanout-build/receipt.json` | Passed |

The SDK native control was byte identical; the PE control matched after removing
link timestamp/checksum fields. The candidate uses title-visible imports and
contains no raw syscall instruction. Telemetry was added through a verified
single-source-file (`vulkan.c`) rebuild; the remaining object inputs matched
the preceding complete build.

Immutable package: `/tmp/prospero-replay-fanout-build/package/PPSA99995`.
Only the Wine Vulkan PE DLL, native PRX and `SOURCES.txt` differ from the resolver
baseline. Inventory: `/tmp/prospero-replay-fanout-build/package.json`.

- PE SHA-256: `8af2966b402642a71d0885a91d7bf997bc5b2384603c354e4890215af7315de5`
- PRX SHA-256: `c0331ae81b056d603eca97246e0b1b355c6aaada9f38ed6a961ddb8887563269`

Use the same package and worker count to compare fanout disabled versus enabled.
Require `PW_VK_REPLAY_POOL` growth records and worker overlap before attributing
any console change to physical pool splitting. Host tests do not establish a
performance gain or game compatibility.
