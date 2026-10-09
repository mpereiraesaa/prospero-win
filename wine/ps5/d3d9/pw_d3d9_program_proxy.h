/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_PROGRAM_PROXY_H
#define PW_D3D9_PROGRAM_PROXY_H
#include <d3d9.h>
#include "pw_d3d9_session.h"
#include "../pw_d3d9_program_wire.h"
struct pw_d3d9_program_proxy_ops {
 HRESULT (*program)(IDirect3DDevice9 *,const struct pw_d3d9_program_request *,struct pw_d3d9_program_reply *);
 HRESULT (*query)(IDirect3DDevice9 *,struct pw_d3d9_object_ref,uint32_t,void *,UINT *);
 HRESULT (*release)(IDirect3DDevice9 *,struct pw_d3d9_object_ref);
 HRESULT (*defer)(IDirect3DDevice9 *,struct pw_d3d9_deferred *);
 void (*fail)(IDirect3DDevice9 *,HRESULT);
};
void pw_d3d9_program_proxy_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_program_proxy_ops *);
HRESULT pw_d3d9_program_proxy_resolve(IDirect3DDevice9 *,IUnknown *,uint32_t,struct pw_d3d9_object_ref *);
/* Consumes one returned remote guest reference on every path. */
HRESULT pw_d3d9_program_proxy_wrap(IDirect3DDevice9 *,uint32_t,struct pw_d3d9_object_ref,void **);
#endif
