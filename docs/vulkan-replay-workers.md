# Host Vulkan replay workers

The portable scheduler is a dependency for native Vulkan parallel replay. It is
not yet connected to Wine dispatch, and makes no game performance claim.

Each command-buffer generation has a lane with monotonically increasing tickets.
Jobs copy their input bytes before admission returns. Workers execute a lane in
order. Lanes sharing a command-pool identity exclude one another: Vulkan requires
external synchronization for the pool, including recording its command buffers.
Independent pools can execute on different host pthreads at the same time.

A lane wait uses an explicit ticket; it does not drain unrelated pools. Pool and
global barriers are for lifecycle operations and fallback. Their caller must stop
new submissions in that scope. Before freeing or recycling a command buffer, drop
its lane. Before pool reset/trim/destruction, wait for all lanes in that pool.
Callbacks never run under the scheduler mutex and must never call guest code or
re-enter the scheduler. Callback context stays alive until successful lane drop
or scheduler destruction. A callback error becomes sticky; waiting callers receive
failure, and no further queued job starts. Already executing independent callbacks
finish. Destruction cancels pending jobs and joins workers before freeing memory.

The byte limit includes queued and executing allocations. Admission waits for
capacity without stopping workers. Oversized jobs and counter exhaustion fail
without publication. Worker count is explicit (one through eight); the runtime
adapter will supply the default of two and profile/environment overrides. Stats
include per-worker jobs, peak concurrent callbacks, owned bytes and wait counts.
These counters establish actual overlap in host tests, not console performance.

The synthetic fixture blocks one pool, proves a second pool executes concurrently,
checks a different buffer from the first pool stays excluded, and waits for the
second pool without releasing the first. It also checks 3,000 ordered records,
owned stack inputs, pool identity reuse, bounded storage and sticky failure.
A blocked callback forces capacity backpressure; releasing it lets admission
finish. Injected failure creating the second pthread checks partial-start cleanup.

Integration still requires command-buffer-to-pool tracking, lifecycle barriers,
secondary-buffer dependencies, submit reference waits, guest/native layout
classification, native thread startup on PS5, and measured two-worker activity.
If workload pools do not provide concurrency, logical pool fanout is needed;
this core does not manufacture independent pools or change Vulkan semantics.
Synchronous begin/end errors must retain their contract.

Reference: [Vulkan command pool synchronization](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html#commandbuffers-pools).
