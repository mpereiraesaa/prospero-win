/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SESSION_H
#define PW_D3D9_SESSION_H
#include <windows.h>
#include "../pw_d3d9_objects.h"
#include "../pw_d3d9_factory_wire.h"
#include "../pw_d3d9_device_wire.h"
#ifdef PW_D3D9_ENABLE_RESOURCE
#include "../pw_d3d9_resource_wire.h"
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
#include "../pw_d3d9_texture_wire.h"
#endif
struct pw_d3d9_session;
#ifdef PW_D3D9_ENABLE_IMPLICIT
#if !defined(PW_D3D9_ENABLE_DEVICE) || !defined(PW_D3D9_ENABLE_TEXTURE)
#error Implicit owners require device and texture support
#endif
#include "../pw_d3d9_implicit_wire.h"
HRESULT pw_d3d9_session_implicit(struct pw_d3d9_session *,struct pw_d3d9_object_ref,
 const struct pw_d3d9_implicit_request *,struct pw_d3d9_implicit_reply *);
#endif
#ifdef PW_D3D9_ENABLE_METHODS
#include "../pw_d3d9_command_wire.h"
#include "../pw_d3d9_getter_wire.h"
HRESULT pw_d3d9_session_command(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_command *);
HRESULT pw_d3d9_session_getter(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_getter_request *,struct pw_d3d9_getter_reply *);
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
#include "../pw_d3d9_program_wire.h"
#include "../pw_d3d9_program_query.h"
HRESULT pw_d3d9_session_program_query(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_program_query_request *,struct pw_d3d9_program_query_reply *);
HRESULT pw_d3d9_session_program(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_program_request *,struct pw_d3d9_program_reply *);
#endif
#ifdef PW_D3D9_ENABLE_GAMMA
#include "../pw_d3d9_gamma_wire.h"
HRESULT pw_d3d9_session_gamma(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_gamma_request *,struct pw_d3d9_gamma_reply *);
#endif
#ifdef PW_D3D9_ENABLE_CURSOR
#include "../pw_d3d9_cursor_wire.h"
HRESULT pw_d3d9_session_cursor(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_cursor_request *,struct pw_d3d9_cursor_reply *);
#endif
#ifdef PW_D3D9_ENABLE_QUERY
#include "../pw_d3d9_query_wire.h"
HRESULT pw_d3d9_session_query(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_query_request *,struct pw_d3d9_query_reply *);
#endif
#ifdef PW_D3D9_ENABLE_STATEBLOCK
#include "../pw_d3d9_stateblock_wire.h"
HRESULT pw_d3d9_session_stateblock(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_stateblock_request *,struct pw_d3d9_stateblock_reply *);
#endif
#ifdef PW_D3D9_ENABLE_OBJECT_GETTER
#include "../pw_d3d9_object_getter.h"
HRESULT pw_d3d9_session_object_getter(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_object_getter_request *,struct pw_d3d9_object_getter_reply *);
#endif
#ifdef PW_D3D9_ENABLE_UP
#include "../pw_d3d9_up_wire.h"
HRESULT pw_d3d9_session_up(struct pw_d3d9_session *,struct pw_d3d9_object_ref,const struct pw_d3d9_up_request *,struct pw_d3d9_up_reply *);
#endif
/* Local PE32 ownership only: no member pointer or Windows handle is wire data.
 * Calls are serialized internally. Close requires all API callers to finish;
 * it cancels/joins before releasing mappings, including on startup failure. */
struct pw_d3d9_session;
/* Intrusive cleanup ownership stays with the caller until function returns.
 * Callbacks must not close the session; their enclosing COM call retains it. */
struct pw_d3d9_deferred {
    struct pw_d3d9_deferred *next;
    void (*function)(void *);
    void *context;
    unsigned queued;
};
/* Client callback admission: final cleanup must defer before any ownership RPC. */
int pw_d3d9_session_in_callback(void);
HRESULT pw_d3d9_session_defer(struct pw_d3d9_session *,struct pw_d3d9_deferred *);
/* DLL detach only; no sessions or callers may remain. No waits or callbacks. */
void pw_d3d9_session_process_detach(void);
HRESULT pw_d3d9_session_open(const WCHAR *service_path,const WCHAR *backend_path,
                            struct pw_d3d9_session **);
HRESULT pw_d3d9_session_create(struct pw_d3d9_session *,UINT sdk_version,
                              struct pw_d3d9_object_ref *);
HRESULT pw_d3d9_session_factory(struct pw_d3d9_session *,struct pw_d3d9_object_ref,
                               const struct pw_d3d9_factory_request *,struct pw_d3d9_factory_reply *);
HRESULT pw_d3d9_session_device(struct pw_d3d9_session *,struct pw_d3d9_object_ref,
                              const struct pw_d3d9_device_request *,struct pw_d3d9_device_reply *);
#ifdef PW_D3D9_ENABLE_RESOURCE
HRESULT pw_d3d9_session_resource(struct pw_d3d9_session *,struct pw_d3d9_object_ref,
 const struct pw_d3d9_resource_request *,struct pw_d3d9_resource_reply *);
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
HRESULT pw_d3d9_session_texture(struct pw_d3d9_session *,struct pw_d3d9_object_ref,
 const struct pw_d3d9_texture_request *,struct pw_d3d9_texture_reply *);
#endif
HRESULT pw_d3d9_session_release(struct pw_d3d9_session *,struct pw_d3d9_object_ref);
/* Cancellation wakes both endpoints; close still joins and frees ownership. */
void pw_d3d9_session_cancel(struct pw_d3d9_session *);
/* Join without freeing a cancelled session; callers must still close it. */
HRESULT pw_d3d9_session_join(struct pw_d3d9_session *);
HRESULT pw_d3d9_session_close(struct pw_d3d9_session *);
#ifdef _WIN64
struct pw_d3d9_native_device;
void pw_d3d9_native_device_call(void *factory,struct pw_d3d9_native_device *,
    const struct pw_d3d9_device_request *,struct pw_d3d9_device_reply *,struct pw_d3d9_native_device **);
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *);
uintptr_t pw_d3d9_native_device_identity(struct pw_d3d9_native_device *);
int pw_d3d9_native_device_destroy(struct pw_d3d9_native_device *);
int pw_d3d9_native_device_shutdown(void);
/* Serialized service transaction state; no native pointers on the wire. */
enum pw_d3d9_implicit_phase { PW_D3D9_IMPLICIT_IDLE, PW_D3D9_IMPLICIT_PREPARED,
 PW_D3D9_IMPLICIT_DONE_RESTORED, PW_D3D9_IMPLICIT_DONE_RETIRED, PW_D3D9_IMPLICIT_DRAINED };
unsigned pw_d3d9_native_device_implicit_phase(struct pw_d3d9_native_device *);
void pw_d3d9_native_device_implicit_set_phase(struct pw_d3d9_native_device *,unsigned);
int pw_d3d9_native_device_reset_succeeded(struct pw_d3d9_native_device *);
int pw_d3d9_native_device_reset_preserved(struct pw_d3d9_native_device *);
#endif
#endif
