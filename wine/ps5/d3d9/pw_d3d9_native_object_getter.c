/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_object_getter.h"
#include <d3d9.h>
#include <string.h>
void pw_d3d9_native_object_result_release(struct pw_d3d9_native_object_result *r)
{if(r){if(r->object)IUnknown_Release((IUnknown *)r->object);memset(r,0,sizeof(*r));}}
uint32_t pw_d3d9_native_object_getter(void *native,const struct pw_d3d9_object_getter_request *q,struct pw_d3d9_native_object_result *out)
{
 IDirect3DDevice9 *device=native;struct pw_d3d9_native_object_result r={0};
 unsigned char wire[24];size_t n;HRESULT hr;
 union {IDirect3DSurface9 *surface;IDirect3DBaseTexture9 *texture;
 IDirect3DVertexDeclaration9 *declaration;IDirect3DVertexShader9 *vs;
 IDirect3DPixelShader9 *ps;IDirect3DVertexBuffer9 *vb;IDirect3DIndexBuffer9 *ib;IUnknown *unknown;} p={0};
 if(!out)return E_POINTER;
 memset(out,0,sizeof(*out));
 if(!device||pw_d3d9_object_getter_encode(wire,sizeof(wire),&n,q))return D3DERR_INVALIDCALL;
 switch(q->method){
 case 18:hr=IDirect3DDevice9_GetBackBuffer(device,q->args[0],q->args[1],q->args[2],&p.surface);break;
 case 38:hr=IDirect3DDevice9_GetRenderTarget(device,q->args[0],&p.surface);break;
 case 40:hr=IDirect3DDevice9_GetDepthStencilSurface(device,&p.surface);break;
 case 64:hr=IDirect3DDevice9_GetTexture(device,q->args[0],&p.texture);break;
 case 88:hr=IDirect3DDevice9_GetVertexDeclaration(device,&p.declaration);break;
 case 93:hr=IDirect3DDevice9_GetVertexShader(device,&p.vs);break;
 case 101:{UINT offset=0,stride=0;hr=IDirect3DDevice9_GetStreamSource(device,q->args[0],&p.vb,&offset,&stride);r.offset=offset;r.stride=stride;break;}
 case 105:hr=IDirect3DDevice9_GetIndices(device,&p.ib);break;
 case 108:hr=IDirect3DDevice9_GetPixelShader(device,&p.ps);break;
 default:return E_NOTIMPL;
 }
 r.object=p.unknown;
 if(SUCCEEDED(hr)&&p.unknown){
  if(q->method==64&&IDirect3DBaseTexture9_GetType(p.texture)!=D3DRTYPE_TEXTURE)hr=E_NOTIMPL;
  else r.kind=pw_d3d9_object_getter_schema(q->method)->kind;
 }
 if(FAILED(hr)){pw_d3d9_native_object_result_release(&r);return (uint32_t)hr;}
 *out=r;return (uint32_t)hr;
}
