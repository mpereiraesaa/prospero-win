/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_DRAW_STATE_H
#define PW_D3D9_NATIVE_DRAW_STATE_H
#include <stdint.h>
#define PW_D3D9_DRAW_FEATURE 65536u
/* Native device-owned evidence. Only the serialized service mutates this state.
 * Zero is unknown: allocation alone does not prove backend creation succeeded. */
enum pw_d3d9_native_recording {PW_D3D9_RECORDING_UNKNOWN, PW_D3D9_RECORDING_LIVE, PW_D3D9_RECORDING_ACTIVE};
static inline void pw_d3d9_native_recording_outcome(unsigned *state,uint32_t method,uint32_t hr)
{
    if(method!=60u&&method!=61u)return;
    if(hr==0)*state=method==60u?PW_D3D9_RECORDING_ACTIVE:PW_D3D9_RECORDING_LIVE;
    else if(!(hr&0x80000000u))*state=PW_D3D9_RECORDING_UNKNOWN;
}
struct pw_d3d9_native_device;
unsigned pw_d3d9_native_device_recording(struct pw_d3d9_native_device *);
void pw_d3d9_native_device_recording_outcome(struct pw_d3d9_native_device *,uint32_t,uint32_t);
#endif
