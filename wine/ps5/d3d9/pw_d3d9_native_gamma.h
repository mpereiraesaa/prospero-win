/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_GAMMA_H
#define PW_D3D9_NATIVE_GAMMA_H
#include <d3d9.h>
#include "../pw_d3d9_gamma_wire.h"
/* Borrowed device is pinned/serialized by the service dispatcher. */
void pw_d3d9_native_gamma_call(IDirect3DDevice9 *,const struct pw_d3d9_gamma_request *,struct pw_d3d9_gamma_reply *);
#endif
