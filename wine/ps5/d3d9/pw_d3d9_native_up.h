/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_UP_H
#define PW_D3D9_NATIVE_UP_H
#include <d3d9.h>
#include "../pw_d3d9_up_wire.h"
/* Serialized native device owner only. Upload must be committed and immutable.
 * Caller finishes on every return, retaining owned bytes until this call ends. */
HRESULT pw_d3d9_native_up_dispatch(IDirect3DDevice9 *,const struct pw_d3d9_up_upload *);
#endif
