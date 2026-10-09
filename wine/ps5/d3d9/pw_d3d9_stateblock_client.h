/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_STATEBLOCK_CLIENT_H
#define PW_D3D9_STATEBLOCK_CLIENT_H
#include <d3d9.h>
#include "pw_d3d9_session.h"
#include "../pw_d3d9_stateblock_wire.h"
struct pw_d3d9_stateblock_client_ops {
 HRESULT (*call)(IDirect3DDevice9 *,struct pw_d3d9_object_ref,const struct pw_d3d9_stateblock_request *,struct pw_d3d9_stateblock_reply *);
 HRESULT (*release)(IDirect3DDevice9 *,struct pw_d3d9_object_ref);
 HRESULT (*defer)(IDirect3DDevice9 *,struct pw_d3d9_deferred *);
 void (*fail)(IDirect3DDevice9 *,HRESULT);
};
/* Install once before vtable publication. Ref0/0 targets the parent device.
 * fail cancels/sticks the session; native objects then belong to service teardown.
 * Failed deferred enqueue retains local ownership; never joins inside reentry. */
void pw_d3d9_stateblock_client_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_stateblock_client_ops *);
#endif
