# Actual cursor session transport

This regression installs the real typed cursor frontend on a minimal PE32 device shell. Its callbacks use production session calls, native device registry, texture resource acquisition and native cursor dispatch. A test-only ordinary native window adapter replaces PS5 association; shipping transport and cursor code remain unchanged. The test does not prove production COM identity/window installation or console behavior.

Each of three cycles creates two real native devices and uploads an opaque32x32 A8R8G8B8 surface through texture lock/write transport. SetCursorProperties receives that typed surface, SetCursorPosition receives signed negative coordinates and flags, and ShowCursor returns the exact previous noncanonical true value0xffffffff. Null, stale generation, released surface, wrong interface kind and another device's surface are rejected without cancelling the healthy session. Device/frontend references balance and session close completes. System cursor position is restored after the fixture.

The positive actual run is `/tmp/prospero-d3d9-cursor-transport-r1/receipt.json`, with three clean cycles. It records18 production C hashes; every one matches the tested `0cbfe55` source. Its header coverage was not recorded, so it is not evidence of complete source/header identity for a later combined build.

Subsequent r2 and r3 attempts to extend receipt header coverage timed out at the unchanged90-second host launch deadline without a completed fixture result. Original logs and receipts were preserved at the time of review; they were subsequently lost in the reboot described below. Separate `timeout-receipt.json` files in those output directories explicitly record timeout status and hashes of the originals. The r3 launched child terminated; scoped prefix cleanup left one explorer process observed uninterruptible in kernel `drm_open`. Further GPU starts were held. These failures do not invalidate r1 or establish a cursor-method failure.

The revised runner freezes every local wine/ps5 header alongside C/fixture sources before compilation. On timeout it now saves command, available stdout/stderr, source hashes and status `timeout` before rethrowing; nonzero exit saves status `failed`. Controlled injected timeout and failed-exit checks passed without starting Wine or a GPU. This final receipt-handling revision has no new successful GPU run while the host DRM handle remains blocked.

## Evidence retention after workstation reboot

The October 9 workstation reboot removed the `/tmp` artifacts named above, including the original receipts, binaries and logs. Those paths describe historical reviewed runs; they are no longer available for independent hash verification. The fixture sources and review history survive in Git. No result or receipt has been reconstructed as a new execution. Future runs must use a persistent output directory and record fresh artifact hashes.
