# First-wave client command batching

`PW_D3D9_ENABLE_BATCH` negotiates HELLO feature8192 and uses opcode33; paired shipping features14335. Six audited setter families only: policy rejection uses the existing synchronous path. This is bounded client staging followed by synchronous batch acknowledgement, not the completed producer-arena/worker design. No timer or idle dispatch guarantee.

One serialized session owns at most128 commands/8064 encoded bytes and one device target. Append copies all data. Commands receive contiguous non-reused logical sequences. Target change, capacity or >=1ms oldest residence checked on the next enqueue flushes the prior batch. Every synchronous RPC, remote Release, implicit owner phase, Reset, Present and STOP first flushes under the same serialization lock, so another thread cannot overtake between flush and RPC. STOP transitions from READY only after flushing. Callback reentry retains existing rejection/deferred cleanup rules.

The device's remote guest reference remains live until ordered Release; service pins it throughout batch execution. This phase admits no resource/object-reference commands. Cancellation discards pending staging and wakes the service. A failed/malformed batch acknowledgement makes the exact first failure sticky, cancels the session, and forbids subsequent command execution. Full sequence/count/attempted/failed-index/HRESULT correlation is mandatory.

`async_queued` counts locally accepted setter records; `batch_flushes` counts published batch RPCs, `batch_commands` their carried commands, and `batch_residence_wall_us` sums oldest-command staging residence through flush start. These differ from true API counts, RPCs and executed commands after failure. Service reports carried batch commands, while local acceptance is client-only. Opcode33 is included in bounded histograms.

The controlled PE32 fixture covers copied data, contiguous replies, capacity/next-enqueue age flush, target change, concurrent producers, callback rejection, Release/STOP ordering and sticky partial-batch failure. Actual native lifecycle and matched workload acceptance are separate required gates before console enablement.

Runtime admission requires `PW_D3D9_ASYNC=1`; absent or other values retain synchronous calls. HELLO advertises compiled capability independently. The controlled fixture proves both default-off and enabled admission.
