/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_UP_CLIENT_H
#define PW_D3D9_UP_CLIENT_H
#include <d3d9.h>
#include "../pw_d3d9_up_wire.h"
struct pw_d3d9_up_client_ops {
 HRESULT (*up)(IDirect3DDevice9 *,const struct pw_d3d9_up_request *,struct pw_d3d9_up_reply *);
 /* Nonblocking sticky cancellation; service owns abandoned upload teardown. */
 void (*fail)(IDirect3DDevice9 *,HRESULT);
};
void pw_d3d9_up_client_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_up_client_ops *);
#endif
