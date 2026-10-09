/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_TEXTURE_PROXY_H
#define PW_D3D9_TEXTURE_PROXY_H
#include <d3d9.h>
#include "pw_d3d9_session.h"
#include "pw_d3d9_texture_client.h"
#include "pw_d3d9_kinds.h"
/* texture must validate a CONTAINER device reply against the exact parent
 * remote identity before returning it; mismatches cancel the session. */
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
/* Device lifecycle integration. Install is called before public device exposure
 * and after successful Reset. Prepare freezes public-zero owned shells without
 * waiting; AddRef/QI/resolve cannot resurrect those shells until finish. */
HRESULT pw_d3d9_texture_proxy_owners_install(IDirect3DDevice9 *,const struct pw_d3d9_object_ref *,UINT);
HRESULT pw_d3d9_texture_proxy_owners_prepare_reset(IDirect3DDevice9 *,struct pw_d3d9_object_ref *,UINT,UINT *);
/* keep_old restores identity after early rejected Reset; otherwise remote zero
 * shell references were consumed by service, so no Release is sent for them. */
void pw_d3d9_texture_proxy_owners_finish_reset(IDirect3DDevice9 *,BOOL keep_old);
HRESULT pw_d3d9_texture_proxy_owners_dispose(IDirect3DDevice9 *);
/* On final decrement, closes owner admission and retains one internal teardown
 * sentinel while returning zero. Final cleanup frees the device directly. */
ULONG pw_d3d9_texture_proxy_parent_release(IDirect3DDevice9 *,LONG *);
#endif
