/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_FAILURE_WAIT_H
#define PW_D3D9_FAILURE_WAIT_H
#include <windows.h>
/* A native failure ACK has already been published. Do not mark shared FAILED
 * yet: receive rejects that state before reading queued replies. No further
 * transport requests may be consumed/dispatched while parked here.
 * The owner signals cancel after reading the failure, on close, or on any
 * local transport failure. Native and guest threads share one process: process
 * exit terminates both; closing a session must signal cancel before joining.
 * Pump native window messages so synchronous UI cleanup cannot deadlock the
 * client before it reaches cancel. This is not a new request-dispatch loop. */
static inline DWORD pw_d3d9_failure_wait(HANDLE cancel)
{
    for (;;) {
        DWORD status = MsgWaitForMultipleObjects(1, &cancel, FALSE, INFINITE, QS_ALLINPUT);
        if (status != WAIT_OBJECT_0 + 1) return status;
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            status = WaitForSingleObject(cancel, 0);
            if (status != WAIT_TIMEOUT) return status;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
}
#endif
