/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_QUERY_PROXY_H
#define PW_D3D9_QUERY_PROXY_H
#include <d3d9.h>
#include "pw_d3d9_session.h"
#include "../pw_d3d9_query_wire.h"
struct pw_d3d9_query_proxy_ops {
 HRESULT (*call)(IDirect3DDevice9 *,struct pw_d3d9_object_ref,const struct pw_d3d9_query_request *,struct pw_d3d9_query_reply *);
 HRESULT (*release)(IDirect3DDevice9 *,struct pw_d3d9_object_ref);
 HRESULT (*defer)(IDirect3DDevice9 *,struct pw_d3d9_deferred *);
 void (*fail)(IDirect3DDevice9 *,HRESULT);
};
/* call returns exact backend HRESULT and populated reply even on backend errors;
 * transport errors may leave reply empty. Ref0/0 targets parent device. fail must
 * cancel/stick without joining inside reentry. Failed defer retains ownership. */
void pw_d3d9_query_proxy_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_query_proxy_ops *);
#endif
