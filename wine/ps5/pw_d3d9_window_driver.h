/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_WINDOW_DRIVER_H
#define PW_D3D9_WINDOW_DRIVER_H
#include "pw_d3d9_window.h"
#define PW_D3D9_WINDOW_DRIVER_CALL 0x50570020u
#define PW_D3D9_WINDOW_DRIVER_VERSION 2u
#define PW_D3D9_GUEST_WINDOW_CALL 0x50570021u
#define PW_D3D9_GUEST_WINDOW_VERSION 1u
/* Native local API via NtUserCallTwoParam(&request,sizeof(request),CALL).
 * This is NOT a wire payload: the local service HWND never crosses rings.
 * Only the opaque guest ID and returned association ID belong in messages.
 * Return value is pw_d3d9_window_result. ATTACH returns the driver's own
 * epoch/id/generation; it is independent of the wire session epoch.
 * All operations must run on the service window owner thread. */
enum pw_d3d9_window_driver_op {
 PW_D3D9_WINDOW_ATTACH=1, PW_D3D9_WINDOW_BEGIN, PW_D3D9_WINDOW_ACK,
 PW_D3D9_WINDOW_CLOSE, PW_D3D9_WINDOW_DETACH, PW_D3D9_WINDOW_QUERY_STATE
};
/* Guest owner-thread local call: NtUserCallTwoParam(hwnd,&request,GUEST_CALL).
 * Fixed-width payload is identical in PE32 and PE64. IDs are retired on
 * unregister/destruction; a later registration always advances generation. */
enum pw_d3d9_guest_window_op { PW_D3D9_GUEST_REGISTER=1, PW_D3D9_GUEST_UNREGISTER };
struct pw_d3d9_guest_window_request {
 uint32_t version,size,operation,reserved;
 struct pw_d3d9_window_id id;
 uint32_t reserved2;
};
struct pw_d3d9_window_driver_request {
 uint32_t version,size,operation,reserved;
 struct pw_d3d9_window_id guest;
 uint64_t service;
 struct pw_d3d9_window_id id;
 uint32_t reserved2;
 uint64_t sequence;
 struct pw_d3d9_window_state state;
 uint32_t hresult;
};
#endif
