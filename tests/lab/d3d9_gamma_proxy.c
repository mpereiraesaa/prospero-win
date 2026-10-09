/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_gamma_proxy.h"
static IDirect3DDevice9 parent;static LONG refs=1;static unsigned calls,failures;static int mode;static uint16_t stored[768];
static ULONG WINAPI addref(IDirect3DDevice9 *d){assert(d==&parent);return InterlockedIncrement(&refs);}
static ULONG WINAPI release(IDirect3DDevice9 *d){assert(d==&parent);return InterlockedDecrement(&refs);}
static void fail(IDirect3DDevice9 *d,HRESULT hr){assert(d==&parent&&FAILED(hr)&&refs==2);failures++;}
static HRESULT call(IDirect3DDevice9 *d,const struct pw_d3d9_gamma_request *q,struct pw_d3d9_gamma_reply *r)
{
 unsigned char wire[PW_D3D9_GAMMA_REQUEST_BYTES];struct pw_d3d9_gamma_request decoded;struct pw_d3d9_gamma_reply reply;
 assert(d==&parent && refs==2);calls++;assert(!pw_d3d9_gamma_request_encode(wire,sizeof(wire),q));assert(!pw_d3d9_gamma_request_decode(&decoded,wire,sizeof(wire)));q=&decoded;
 if(mode==1)return E_FAIL;
 memset(r,0,sizeof(*r));r->method=q->method;
 if(q->method==PW_D3D9_GAMMA_SET){assert(q->flags==0xfedcba98u);if(q->has_ramp)memcpy(stored,q->ramp,sizeof(stored));}
 else {assert(!q->flags);r->has_ramp=q->has_ramp;if(q->has_ramp)memcpy(r->ramp,q->swapchain?q->ramp:stored,sizeof(stored));}
 assert(!pw_d3d9_gamma_reply_encode(wire,PW_D3D9_GAMMA_REPLY_BYTES,q,r));assert(!pw_d3d9_gamma_reply_decode(&reply,q,wire,PW_D3D9_GAMMA_REPLY_BYTES));*r=reply;if(mode==2)r->method=0;return S_OK;
}
int main(void)
{
 IDirect3DDevice9Vtbl table={0};struct pw_d3d9_gamma_proxy_ops ops={call,fail};D3DGAMMARAMP ramp,read,original;
 table.AddRef=addref;table.Release=release;parent.lpVtbl=&table;pw_d3d9_gamma_proxy_install(&table,&ops);assert(!table.Present);
 for(unsigned i=0;i<256;i++){ramp.red[i]=(WORD)(i*257);ramp.green[i]=(WORD)(i*19);ramp.blue[i]=(WORD)(65535-i*257);}
 IDirect3DDevice9_SetGammaRamp(&parent,0,0xfedcba98u,&ramp);memset(&read,0xa5,sizeof(read));IDirect3DDevice9_GetGammaRamp(&parent,0,&read);assert(!memcmp(&ramp,&read,sizeof(ramp)));
 memset(&read,0xa5,sizeof(read));original=read;IDirect3DDevice9_GetGammaRamp(&parent,0xffffffffu,&read);assert(!memcmp(&read,&original,sizeof(read)));
 IDirect3DDevice9_SetGammaRamp(&parent,0,0xfedcba98u,NULL);IDirect3DDevice9_GetGammaRamp(&parent,0,NULL);
 for(mode=1;mode<=2;mode++){IDirect3DDevice9_GetGammaRamp(&parent,0,&read);assert(!memcmp(&read,&original,sizeof(read)));}
 assert(refs==1&&failures==2&&calls==7);puts("PW_GAMMA_PROXY PASS");return 0;
}
