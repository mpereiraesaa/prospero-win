/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_gamma.h"
#include <string.h>
void pw_d3d9_native_gamma_call(IDirect3DDevice9 *device,const struct pw_d3d9_gamma_request *q,struct pw_d3d9_gamma_reply *r)
{
 unsigned char wire[PW_D3D9_GAMMA_REQUEST_BYTES];D3DGAMMARAMP ramp;
 memset(r,0,sizeof(*r));r->method=q->method;r->hresult=D3DERR_INVALIDCALL;
 if(!device || pw_d3d9_gamma_request_encode(wire,sizeof(wire),q))return;
 memcpy(ramp.red,q->ramp,512);memcpy(ramp.green,q->ramp+256,512);memcpy(ramp.blue,q->ramp+512,512);
 if(q->method==PW_D3D9_GAMMA_SET)IDirect3DDevice9_SetGammaRamp(device,q->swapchain,q->flags,q->has_ramp?&ramp:NULL);
 else {
  IDirect3DDevice9_GetGammaRamp(device,q->swapchain,q->has_ramp?&ramp:NULL);
  if(q->has_ramp){r->has_ramp=1;memcpy(r->ramp,ramp.red,512);memcpy(r->ramp+256,ramp.green,512);memcpy(r->ramp+512,ramp.blue,512);}
 }
 /* Both native methods are void; acknowledge only after the actual call. */
 r->hresult=S_OK;
}
