/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_device_object_methods.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static IDirect3DDevice9 device;static LONG refs=1;static unsigned method,calls,wraps,failures,mode;static uint32_t args[3];static void *local=(void *)(uintptr_t)0x1234;
static ULONG WINAPI addref(IDirect3DDevice9 *d){assert(d==&device);return ++refs;}
static ULONG WINAPI release(IDirect3DDevice9 *d){assert(d==&device);return --refs;}
static HRESULT getter(IDirect3DDevice9 *d,const struct pw_d3d9_object_getter_request *q,struct pw_d3d9_object_getter_reply *r)
{
 const struct pw_d3d9_object_getter_schema *schema=pw_d3d9_object_getter_schema(q->method);
 assert(d==&device && refs>=2 && q->method==method && !memcmp(q->args,args,sizeof(args)));calls++;
 *r=(struct pw_d3d9_object_getter_reply){q->method,0x1234,schema->kind,7,9,0,0};
 if(q->method==101){r->offset=0xfedcba98;r->stride=0x87654321;}
 if(mode==1)return D3DERR_INVALIDCALL;
 if(mode==2)r->kind++;
 if(mode==3)r->kind=r->id=r->generation=0;
 if(mode==4)r->hresult++;
 if(mode==6){IDirect3DDevice9_Release(d);assert(refs==1);}
 return 0x1234;
}
static HRESULT wrap(IDirect3DDevice9 *d,uint32_t kind,struct pw_d3d9_object_ref ref,void **out)
{assert(d==&device && refs>=1 && kind==pw_d3d9_object_getter_schema(method)->kind && ref.id==7 && ref.generation==9);wraps++;if(mode==5)return E_OUTOFMEMORY;*out=local;return S_OK;}
static void fail(IDirect3DDevice9 *d,HRESULT hr){assert(d==&device && refs>=1 && hr==E_FAIL);failures++;}
static void setup(unsigned slot){method=slot;memset(args,0,sizeof(args));mode=0;}
int main(void)
{
 IDirect3DDevice9Vtbl table={0};struct pw_d3d9_device_object_methods_ops ops={getter,wrap,fail};IDirect3DSurface9 *surface;IDirect3DBaseTexture9 *texture;IDirect3DVertexDeclaration9 *decl;IDirect3DVertexShader9 *vs;IDirect3DPixelShader9 *ps;IDirect3DVertexBuffer9 *vb;IDirect3DIndexBuffer9 *ib;UINT offset,stride;
 table.AddRef=addref;table.Release=release;device.lpVtbl=&table;pw_d3d9_device_object_methods_install(&table,&ops);assert(!table.Present && !table.GetRenderState);
 setup(18);args[0]=2;args[1]=3;args[2]=D3DBACKBUFFER_TYPE_MONO;assert(IDirect3DDevice9_GetBackBuffer(&device,2,3,D3DBACKBUFFER_TYPE_MONO,&surface)==0x1234 && surface==local);assert(IDirect3DDevice9_GetBackBuffer(&device,2,3,D3DBACKBUFFER_TYPE_MONO,NULL)==D3DERR_INVALIDCALL);
 setup(38);args[0]=2;assert(IDirect3DDevice9_GetRenderTarget(&device,2,&surface)==0x1234 && surface==local);
 setup(40);assert(IDirect3DDevice9_GetDepthStencilSurface(&device,&surface)==0x1234 && surface==local);
 setup(64);args[0]=2;assert(IDirect3DDevice9_GetTexture(&device,2,&texture)==0x1234 && texture==local);
 setup(88);assert(IDirect3DDevice9_GetVertexDeclaration(&device,&decl)==0x1234 && decl==local);
 setup(93);assert(IDirect3DDevice9_GetVertexShader(&device,&vs)==0x1234 && vs==local);
 setup(101);args[0]=2;assert(IDirect3DDevice9_GetStreamSource(&device,2,&vb,&offset,&stride)==0x1234 && vb==local && offset==0xfedcba98 && stride==0x87654321);
 assert(IDirect3DDevice9_GetStreamSource(&device,2,NULL,&offset,&stride)==D3DERR_INVALIDCALL);assert(IDirect3DDevice9_GetStreamSource(&device,2,&vb,NULL,&stride)==D3DERR_INVALIDCALL);assert(IDirect3DDevice9_GetStreamSource(&device,2,&vb,&offset,NULL)==D3DERR_INVALIDCALL);
 setup(105);assert(IDirect3DDevice9_GetIndices(&device,&ib)==0x1234 && ib==local);
 setup(108);assert(IDirect3DDevice9_GetPixelShader(&device,&ps)==0x1234 && ps==local);assert(calls==9 && wraps==9 && refs==1);
 setup(101);args[0]=2;
 for(mode=1;mode<=5;mode++){
  vb=(void *)(uintptr_t)0x5555;offset=123;stride=456;HRESULT hr=IDirect3DDevice9_GetStreamSource(&device,2,&vb,&offset,&stride);
  if(mode==3)assert(hr==0x1234 && !vb && offset==0xfedcba98 && stride==0x87654321);
  else assert(hr==(mode==1?D3DERR_INVALIDCALL:mode==5?E_OUTOFMEMORY:E_FAIL) && vb==(void *)(uintptr_t)0x5555 && offset==123 && stride==456);
 }
 assert(failures==2 && refs==1);setup(105);mode=6;
 assert(IDirect3DDevice9_GetIndices(&device,&ib)==0x1234 && ib==local && refs==0);
 puts("PASS object getter methods:9 typed slots, exact HRESULT/null/stream data, atomic failures, device pin across callbacks");return 0;
}
