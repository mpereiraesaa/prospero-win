/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_TEXTURE_PROXY_H
#define PW_D3D9_TEXTURE_PROXY_H
#include <d3d9.h>
#include "pw_d3d9_session.h"
#include "pw_d3d9_texture_client.h"
#include "pw_d3d9_kinds.h"
struct pw_d3d9_texture_proxy_ops {
 HRESULT (*texture)(IDirect3DDevice9 *,struct pw_d3d9_object_ref,const struct pw_d3d9_texture_request *,struct pw_d3d9_texture_reply *);
 HRESULT (*release)(IDirect3DDevice9 *,struct pw_d3d9_object_ref);
 HRESULT (*defer)(IDirect3DDevice9 *,struct pw_d3d9_deferred *);
 void (*fail)(IDirect3DDevice9 *,HRESULT);
};
/* Once before vtable publication. fail cancels/sticks, without joining in reentry. */
void pw_d3d9_texture_proxy_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_texture_proxy_ops *);
/* Consumes one owned remote reference on every path. Zero levels requests an
 * actual native DESC query; never invents a level count. OOM cancels via fail. */
HRESULT pw_d3d9_texture_proxy_wrap(IDirect3DDevice9 *,uint32_t kind,struct pw_d3d9_object_ref,uint32_t levels,void **);
/* Address-only cache lookup: never dereferences a foreign COM pointer. */
HRESULT pw_d3d9_texture_proxy_resolve(IDirect3DDevice9 *,IUnknown *,uint32_t kind,struct pw_d3d9_object_ref *);
#endif
