/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_DEVICE_PROXY_H
#define PW_D3D9_DEVICE_PROXY_H
#include <d3d9.h>
#include "pw_d3d9_session.h"
/* Caller retains the local parent during this operation. No pointer is wire. */
HRESULT pw_d3d9_device_proxy_create(IDirect3D9 *,struct pw_d3d9_session *,
    struct pw_d3d9_object_ref,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS *,IDirect3DDevice9 **);
void pw_d3d9_device_proxy_detach(void);
#endif
