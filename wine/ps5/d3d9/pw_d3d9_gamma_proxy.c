/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_gamma_proxy.h"
#include <string.h>
static struct pw_d3d9_gamma_proxy_ops ops;
static void invoke(IDirect3DDevice9 *device,uint32_t method,UINT swapchain,DWORD flags,const D3DGAMMARAMP *input,D3DGAMMARAMP *output)
{
 struct pw_d3d9_gamma_request q={.method=method,.swapchain=swapchain,.flags=flags,.has_ramp=input!=NULL};struct pw_d3d9_gamma_reply r={0};unsigned char wire[PW_D3D9_GAMMA_REPLY_BYTES];HRESULT hr;
 IDirect3DDevice9_AddRef(device);
 if(input){memcpy(q.ramp,input->red,512);memcpy(q.ramp+256,input->green,512);memcpy(q.ramp+512,input->blue,512);}
 hr=ops.call(device,&q,&r);
 if(SUCCEEDED(hr) && (hr!=S_OK || r.hresult!=(uint32_t)hr || pw_d3d9_gamma_reply_encode(wire,sizeof(wire),&q,&r)))hr=E_FAIL;
 if(FAILED(hr))ops.fail(device,hr);
 else if(output){memcpy(output->red,r.ramp,512);memcpy(output->green,r.ramp+256,512);memcpy(output->blue,r.ramp+512,512);}
 IDirect3DDevice9_Release(device);
}
static void WINAPI set(IDirect3DDevice9 *d,UINT chain,DWORD flags,const D3DGAMMARAMP *r){invoke(d,PW_D3D9_GAMMA_SET,chain,flags,r,NULL);}
static void WINAPI get(IDirect3DDevice9 *d,UINT chain,D3DGAMMARAMP *r){invoke(d,PW_D3D9_GAMMA_GET,chain,0,r,r);}
void pw_d3d9_gamma_proxy_install(IDirect3DDevice9Vtbl *table,const struct pw_d3d9_gamma_proxy_ops *callbacks){ops=*callbacks;table->SetGammaRamp=set;table->GetGammaRamp=get;}
