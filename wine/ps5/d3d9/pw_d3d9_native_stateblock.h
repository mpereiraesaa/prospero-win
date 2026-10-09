/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_STATEBLOCK_H
#define PW_D3D9_NATIVE_STATEBLOCK_H
#include <d3d9.h>
#include "../pw_d3d9_stateblock_wire.h"
/* Caller owns and validates the native device/block target on its owner thread.
 * Successful Create/End transfers one owned block reference through created.
 * Other methods and failed calls leave created untouched. Registry publication
 * and parent lifetime ownership belong to the session. */
HRESULT pw_d3d9_native_stateblock_dispatch(IDirect3DDevice9 *,IDirect3DStateBlock9 *,
 const struct pw_d3d9_stateblock_request *,IDirect3DStateBlock9 **created);
#endif
