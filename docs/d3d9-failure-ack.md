# Preserve the terminal native batch failure ACK

After publishing a typed native failure ACK, the service parks on the session
cancel event instead of receiving another request. The client may therefore
read the exact HRESULT/attempted prefix even when later requests are already
queued. Setting shared FAILED first would prevent `channel_receive` from reading
that ACK. No later request is consumed or executed while parked.

The service pumps native UI messages while parked, retaining existing native
window cleanup liveness. The client cancels on a received failed ACK, explicit
session cancellation, transport failure and close. It must cancel before broker
join. The domains share a process, so process exit terminates both. A live client
that never calls again leaves its session alive until it cancels/closes; no
timeout claims consumption of unacknowledged commands. Transport corruption or
an undeliverable ACK still uses ordinary cancellation and does not promise a
recoverable native HRESULT.

The controlled PE32/PE64 fixture queues two requests before consuming any ACK.
It reproduces the previous receive-next/cancel ordering hiding the first ACK,
then checks delayed exact failure consumption, an unconsumed second request,
native window-message pumping and cancellation wakeup. A dropped-ACK case joins
before modeled quarantine retirement. It uses real wire endpoints and the
production park helper; native COM dispatch and actual resource ticket callbacks
are not covered. Full producer/service pipeline proofs remain separate.
