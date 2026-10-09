/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* TEST ONLY: the profiling smoke's ordinary-HWND adapter plus the native
 * recording-domain tracking that draw admission reads. A published adapter
 * device exists only after a successful create, which the production device
 * records as LIVE; outcomes then follow pw_d3d9_native_recording_outcome. */
#include "d3d9_profile_transport_device.c"
#include "pw_d3d9_native_draw_state.h"
#define TRACKED 8
static struct {struct pw_d3d9_native_device *device;unsigned recording;} tracked[TRACKED];
static unsigned *slot(struct pw_d3d9_native_device *d)
{
 for(unsigned i=0;i<TRACKED;i++)if(tracked[i].device==d)return &tracked[i].recording;
 for(unsigned i=0;i<TRACKED;i++)if(!tracked[i].device){tracked[i].device=d;tracked[i].recording=PW_D3D9_RECORDING_LIVE;return &tracked[i].recording;}
 return NULL;
}
unsigned pw_d3d9_native_device_recording(struct pw_d3d9_native_device *d)
{unsigned *s=d?slot(d):NULL;return s?*s:PW_D3D9_RECORDING_UNKNOWN;}
void pw_d3d9_native_device_recording_outcome(struct pw_d3d9_native_device *d,uint32_t method,uint32_t hr)
{unsigned *s=d?slot(d):NULL;if(s)pw_d3d9_native_recording_outcome(s,method,hr);}
