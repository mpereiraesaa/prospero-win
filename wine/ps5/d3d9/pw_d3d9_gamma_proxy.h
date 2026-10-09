/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_GAMMA_PROXY_H
#define PW_D3D9_GAMMA_PROXY_H
#include <d3d9.h>
#include "../pw_d3d9_gamma_wire.h"
struct pw_d3d9_gamma_proxy_ops {
 HRESULT (*call)(IDirect3DDevice9 *,const struct pw_d3d9_gamma_request *,struct pw_d3d9_gamma_reply *);
 /* Void COM methods report transport failure by sticking/cancelling the session.
  * fail must never join synchronously during a reentrant callback. */
 void (*fail)(IDirect3DDevice9 *,HRESULT);
};
void pw_d3d9_gamma_proxy_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_gamma_proxy_ops *);
#endif
