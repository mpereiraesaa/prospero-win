/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_CURSOR_H
#define PW_D3D9_CURSOR_H
#include <d3d9.h>
#include "../pw_d3d9_cursor_wire.h"
#include "../pw_d3d9_objects.h"
#include "pw_d3d9_kinds.h"
struct pw_d3d9_cursor_ops {
 HRESULT (*call)(IDirect3DDevice9 *,const struct pw_d3d9_cursor_request *,struct pw_d3d9_cursor_reply *);
 HRESULT (*resolve)(IDirect3DDevice9 *,IUnknown *,uint32_t,struct pw_d3d9_object_ref *);
 void (*fail)(IDirect3DDevice9 *,HRESULT);
};
void pw_d3d9_cursor_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_cursor_ops *);
/* Acquire atomically validates kind/device/epoch/generation and returns one
 * owned exact surface reference. No registry lock remains across native call. */
typedef HRESULT (*pw_d3d9_cursor_acquire_fn)(void *,uint32_t,uint32_t,uint32_t,IDirect3DDevice9 *,void **);
HRESULT pw_d3d9_native_cursor(IDirect3DDevice9 *,const struct pw_d3d9_cursor_request *,struct pw_d3d9_cursor_reply *,pw_d3d9_cursor_acquire_fn,void *);
#endif
