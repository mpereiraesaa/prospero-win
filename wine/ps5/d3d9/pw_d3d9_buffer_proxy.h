/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_BUFFER_PROXY_H
#define PW_D3D9_BUFFER_PROXY_H
#include <d3d9.h>
#include "pw_d3d9_session.h"
#include "../pw_d3d9_resource_wire.h"
struct pw_d3d9_buffer_proxy_ops {
 HRESULT (*resource)(IDirect3DDevice9 *,struct pw_d3d9_object_ref,const struct pw_d3d9_resource_request *,struct pw_d3d9_resource_reply *);
 HRESULT (*release)(IDirect3DDevice9 *,struct pw_d3d9_object_ref);
 HRESULT (*defer)(IDirect3DDevice9 *,struct pw_d3d9_deferred *);
 void (*fail)(IDirect3DDevice9 *,HRESULT);
};
/* Install once before publishing the device vtable. Callbacks are immutable. */
HRESULT pw_d3d9_buffer_proxy_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_buffer_proxy_ops *);
/* Consumes one remote guest reference on every path; parent is borrowed. */
HRESULT pw_d3d9_buffer_proxy_wrap(IDirect3DDevice9 *,uint32_t,struct pw_d3d9_object_ref,void **);
/* Address lookup only: foreign pointers are never dereferenced. */
HRESULT pw_d3d9_buffer_proxy_resolve(IDirect3DDevice9 *,IUnknown *,uint32_t,struct pw_d3d9_object_ref *);
#endif
