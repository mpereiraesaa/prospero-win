/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SESSION_H
#define PW_D3D9_SESSION_H
#include <windows.h>
#include "../pw_d3d9_objects.h"
#include "../pw_d3d9_factory_wire.h"
#include "../pw_d3d9_device_wire.h"
/* Local PE32 ownership only: no member pointer or Windows handle is wire data.
 * Calls are serialized internally. Close requires all API callers to finish;
 * it cancels/joins before releasing mappings, including on startup failure. */
struct pw_d3d9_session;
HRESULT pw_d3d9_session_open(const WCHAR *service_path,const WCHAR *backend_path,
                            struct pw_d3d9_session **);
HRESULT pw_d3d9_session_create(struct pw_d3d9_session *,UINT sdk_version,
                              struct pw_d3d9_object_ref *);
HRESULT pw_d3d9_session_factory(struct pw_d3d9_session *,struct pw_d3d9_object_ref,
                               const struct pw_d3d9_factory_request *,struct pw_d3d9_factory_reply *);
HRESULT pw_d3d9_session_device(struct pw_d3d9_session *,struct pw_d3d9_object_ref,
                              const struct pw_d3d9_device_request *,struct pw_d3d9_device_reply *);
HRESULT pw_d3d9_session_release(struct pw_d3d9_session *,struct pw_d3d9_object_ref);
/* Cancellation wakes both endpoints; close still joins and frees ownership. */
void pw_d3d9_session_cancel(struct pw_d3d9_session *);
HRESULT pw_d3d9_session_close(struct pw_d3d9_session *);
#ifdef _WIN64
struct pw_d3d9_native_device;
void pw_d3d9_native_device_call(void *factory,struct pw_d3d9_native_device *,
    const struct pw_d3d9_device_request *,struct pw_d3d9_device_reply *,struct pw_d3d9_native_device **);
uintptr_t pw_d3d9_native_device_identity(struct pw_d3d9_native_device *);
int pw_d3d9_native_device_destroy(struct pw_d3d9_native_device *);
int pw_d3d9_native_device_shutdown(void);
#endif
#endif
