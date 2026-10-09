/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_COMMAND_H
#define PW_D3D9_NATIVE_COMMAND_H
#include "d3d9.h"
#include "../pw_d3d9_command_wire.h"
#include "pw_d3d9_kinds.h"
/* On success acquire returns an owned reference to the exact typed interface.
 * The service validates generation, kind, device and epoch atomically with
 * pinning. No registry lock may remain held when acquire returns. */
typedef HRESULT (*pw_d3d9_command_acquire_fn)(void *, uint32_t, uint32_t,
        uint32_t, IDirect3DDevice9 *, void **);
HRESULT pw_d3d9_native_command_dispatch(IDirect3DDevice9 *,
        const struct pw_d3d9_command *, pw_d3d9_command_acquire_fn, void *);
#endif
