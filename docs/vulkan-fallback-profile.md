# Vulkan fallback profiling

Enable `PW_VK_BATCH=1`, `PW_VK_BATCH_STATS=1` and
`PW_VK_BATCH_FALLBACK_PROFILE=1` for a diagnostic run. The default is off.
Stats disabled also disables profiling even if its environment variable is set.
No deployment or frozen candidate artifact is changed by this feature.

Every 300 QueuePresentKHR ordinals, `PW_VK_FALLBACK` reports the interval
start/end, exact fallback call count and top-entry count. Up to eight
`PW_VK_FALLBACK_TOP` rows give rank, exact staged Unix code/function name,
interval calls and cumulative calls. Ties sort by ascending code. The first
interval includes initialization before the first present. Unreported functions
remain included in the interval total. Function names are generated from the
same final staged loader thunk enum; regeneration preserves existing indices.

Scope is **32-bit PE winevulkan intercepted fallback calls, process cumulative**.
This counts the existing fallback path while batching is active, including
pre-negotiation calls, unsupported functions, failed encodes and producer
allocation failures. It excludes enqueued commands, internal raw capability
queries, callback/allocator calls that disable batching, calls after sticky
disable, 64-bit PE Vulkan and transitions through all other libraries. It is
not a native total or per-thread count. A zero top count means no counted
fallbacks in that period, not absence of native calls. Disabled or sticky modes
do not produce histogram reports; a partial final period is not emitted.

Present ordinals include unsuccessful presents. Divide counts by ordinal span
only as calls per present invocation; successful rendered-frame inference
requires the existing present-result records. No FPS or performance gain is
inferred. Profiling itself adds counter writes and periodic scanning/logging;
leave it off for final FPS confirmation. With stats disabled it adds one false
branch at fallback, no histogram writes, allocations, scans, logs or crossings.

The histogram and reporting use the existing ordering gate. No driver callbacks,
new Unix calls, capability negotiation or stream/version changes are added.
Storage is fixed at two 64-bit counters per enum entry; no allocator failure
path is introduced. Actual Win32 host tests mock only the Unix boundary and
verify original paths, exact counts/deltas, unsupported command names, bounded
top-eight output, deterministic ties, stats-off suppression and ordering.
Console driver integration and performance overhead have not been measured.
