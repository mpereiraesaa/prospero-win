# Guest UI calls and bridge RPC reentrancy

PE32 session waits pump their calling thread's messages while waiting for a
reply, startup, serialization ownership or service join. The serialization
lock is acquired with TryEnterCriticalSection; contended callers wait on a
local event while pumping rather than blocking a UI owner indefinitely.
WM_QUIT is retained and reposted for the application's outer message loop.

A per-thread callback guard covers both wait syscalls and PeekMessage dispatch.
An active-transaction owner check also rejects direct recursive calls. Nested
RPC returns RPC_E_CANTCALLOUT_ININPUTSYNCCALL before publishing a request or
changing queue state. Startup/close/join reject callback reentry too. Replies
remain ordered and failed nested calls leave output DTOs unchanged.

Factory proxy bookkeeping locks cover only local lists/reference counts. They
are released before opening a session, making an RPC, releasing a remote
object or stopping/joining the service. A temporary local COM reference and
session operation count preserve lifetime across callbacks. Final Release during a pumped callback queues owned cleanup without waiting
on the transaction lock. An intrusive queue drains after the outer reply is
consumed and the lock released; the proxy and session operation reference stay
alive until remote Release completes. Genuine remote-release failures cancel
the session, making failure sticky. A new factory request
while startup/close is in progress fails with NULL instead of waiting while
holding a proxy lock.

The nonfreeing session join API is intended after cancellation, so a device
proxy can wait for native associations to drain before owner-thread guest
unregistration. Close remains required to free session ownership afterward.

## Deterministic fixture

`tests/lab/d3d9_rpc_ui.py` compiles a PE32 UI-owner fixture/proxy and actual
PE64 DXVK service. A fixture-only publication hook queues one posted message
and one cross-thread SendNotifyMessage while each real factory RPC is pending.
Each callback exercises local AddRef/Release, rejects nested GetDeviceCaps and
nested Direct3DCreate9, and checks unchanged failed output. The hook is absent
from normal builds and does not replace any backend operation.

Host proof requires20 real owner calls,41 callbacks,82 nested rejects and
preserved WM_QUIT code37 and21 deferred final releases without cancelling
the healthy session. Existing persistent and DLL proxy fixtures are also
run separately to cover1200 concurrent calls, cancellation/reopen, canonical
identity and final cleanup. This is a client RPC foundation; it does not itself
prove owner-thread CreateDevice/Reset or complete game compatibility.

Receipts: `/tmp/prospero-d3d9-rpc-ui-r8/receipt.json`,
`/tmp/prospero-d3d9-rpc-ui-proxy-final/receipt.json` and
`/tmp/prospero-d3d9-rpc-ui-session-final/receipt.json` all passed with the
final deferred-release implementation. Win32 TLS slots avoid an extra libgcc
DLL dependency and are freed without waiting during proxy detach.

A deterministic contended-call fixture pumps a final Release and injects a
serialization wait failure. Every safe outer exit drains cleanup, including
that wait failure, so the retained proxy/session reference cannot be stranded
when the competing operation drains before the callback enqueues its release.
