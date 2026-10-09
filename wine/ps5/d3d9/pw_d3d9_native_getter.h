/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_GETTER_H
#define PW_D3D9_NATIVE_GETTER_H
#include "d3d9.h"
#include "../pw_d3d9_getter_wire.h"
/* Caller retains a live device and executes on its native service owner thread.
 * OK publishes an exact backend reply, including failed HRESULT with no data.
 * Invalid/unsupported requests leave the reply untouched. */
int pw_d3d9_native_getter_dispatch(IDirect3DDevice9 *,
        const struct pw_d3d9_getter_request *,struct pw_d3d9_getter_reply *);
#endif
