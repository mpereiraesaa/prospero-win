# D3D9 transport profile

Set `PW_D3D9_PROFILE=1` in both domains before launch. Default off: no profile clock reads. This is independent of API diagnostics and does not change wire ABI, method results, ordering, or queue policy. The header-only accumulator is compiled through the shipping session source in both existing builders.

Client and service emit one aggregate record at each published Present boundary, including failure, and a final teardown record. Match `epoch`, `seq`, `object`, `generation`; `seq=0` identifies the final teardown record with no service counterpart. Unpublished/nested Present rejections are accumulated as `rejected_present` in the next boundary or final teardown; they never log while an enclosing RPC owns its critical section. `frame` is a local session published-Present count, not a displayed-frame count; request publication or reply failure may leave an unmatched boundary. `reply_valid` means a transport reply was received/published, not successful typed output or GPU completion. Final records have no request/status attribution (`seq=0`, `reply_valid=0`).

All deltas are **session intervals**, potentially containing several devices and calling threads. First interval includes startup. `pid/tid/domain` identify the emitting endpoint thread, not all contributors. Cumulative attempt/publication/reply totals are also emitted. Opcode counts include local rejected transaction attempts on the client and consumed records on the service. API calls are not counted here: local COM calls make no RPC, and one upload/readback API may issue many RPCs.

`sync_published` counts request records accepted by the channel, including a later wake failure; `replies` counts transport replies. `async_queued=0` states the current synchronous policy; ordinary ring insertion is not asynchronous acceptance. Bytes include the 64-byte protocol header and payload, excluding ring alignment padding. Failures include HRESULT failure and transport failure, not `S_FALSE`.

Timing uses QPC wall time in microseconds. `serial_wait_wall_us` covers lock acquisition, including contention message pumping. `guest_wait_wall_us` covers the reply receive/wait loop, including message callbacks; `roundtrip_wall_us` covers the entire transaction attempt before logging/deferred cleanup. These overlap and must not be summed. `service_dispatch_wall_us` covers dispatch, encoding, and reply publication, excluding idle receive waits. It includes backend waits, callbacks and scheduler delays. **None is CPU time or pure blocked time.** No game/service CPU split is inferred. Native wait instrumentation is separate.

Missing or backwards clocks yield zero duration and `clock_valid=0` for that interval. Counters saturate with sticky `saturated=1`; saturated data is not quantitatively valid. Snapshot state uses a short local lock; formatted logging happens after the transaction lock is released. Logging itself can perturb scheduling and is excluded from sampled intervals.

The portable accumulator test covers accumulation, absent/reversed clocks and saturation. `tests/lab/d3d9_transport_profile.c` includes the shipping session: controlled PE32 client/channel responder covers successful and failed Present, local callback rejection, exact sequence, mixed objects and disabled zero-clock behavior; PE64 exercises the same snapshot/format code. Its controlled responder makes no native D3D or console acceptance claim.

## Combined API and transport boundaries

With API observation compiled, each published Present emits its API snapshot after releasing the session lock, using the identical epoch/request sequence/object/generation. Backend failures are included; unpublished/nested rejects have no immediate API snapshot. Their external entry counts remain in the next process-wide snapshot. Client teardown flushes API histograms after broker join and before IPC destruction, outside transaction locks. API process scope differs from transport session scope; do not infer per-device API totals from correlation.

`tests/lab/d3d9_profile_transport.py --recovery PATH --output NEW_PATH` builds the actual paired DLLs and runs three existing D3DX smoke cycles, substituting only the previously tested ordinary-host native HWND adapter. It requires terminal success, exact client/service/API Present correlation, matching request/reply counts and byte totals, valid clocks, and final API flushes. This proves real native-service metric routing, not console throughput or CPU time.

## Pipeline accumulator fields

The local accumulator reserves four opt-in pipeline counters. No wire layout or
feature negotiation changes with this helper alone:

- `pipeline_published`: batches actually published to the request ring.
- `pipeline_acked`: batches retired by a fully validated ACK, including a valid
  failed-prefix ACK. Malformed or missing ACKs do not increment it.
- `pipeline_pending_peak`: maximum published, not-yet-retired batch count in the
  interval. Accumulator merge takes the maximum; it does not sum this field.
- `pipeline_wait_wall_us`: producer wall time spent waiting for an ACK at a
  credit/ring limit or synchronous boundary. It includes callbacks and scheduling
  delay and is not CPU time or pure blocked time.

The other three fields sum with the existing saturation rules. Pipeline-disabled
runs leave all four zero. Session integration and native overlap evidence are
separate changes. Existing `published` and historical emitted
`sync_published` counters retain their raw wire-publication meaning; consumers
can subtract `pipeline_published` to derive synchronous publications when these
new fields are emitted. Overlapping publication-to-ACK lifetimes must not be
summed as producer time. Existing guest-wait and roundtrip totals may overlap
with the new wait field, so it is a breakdown, not an additional elapsed interval.
