/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_up.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static IDirect3DDevice9 device;static IDirect3DVertexDeclaration9 decl;static unsigned draws,releases,missing;static unsigned char bytes[128];
static HRESULT WINAPI declaration(IDirect3DDevice9 *d,IDirect3DVertexDeclaration9 **out){assert(d==&device);*out=missing?NULL:&decl;return S_OK;}
static ULONG WINAPI release(IDirect3DVertexDeclaration9 *d){assert(d==&decl);releases++;return 1;}
static HRESULT WINAPI elements(IDirect3DVertexDeclaration9 *d,D3DVERTEXELEMENT9 *out,UINT *n){D3DVERTEXELEMENT9 e[]={{0,0,D3DDECLTYPE_FLOAT4,0,0,0},D3DDECL_END()};assert(d==&decl&&*n>=2);memcpy(out,e,sizeof(e));*n=2;return S_OK;}
static HRESULT WINAPI draw(IDirect3DDevice9 *d,D3DPRIMITIVETYPE t,UINT n,const void *v,UINT s){assert(d==&device&&t==4&&s==20&&v);if(n){assert(n==1&&v==bytes);}draws++;return (HRESULT)0x1234;}
static HRESULT WINAPI indexed(IDirect3DDevice9 *d,D3DPRIMITIVETYPE t,UINT min,UINT n,UINT p,const void *ix,D3DFORMAT fmt,const void *v,UINT s){assert(d==&device&&t==4&&min==2&&n==3&&p==1&&ix==bytes+100&&fmt==101&&v==bytes&&s==20);draws++;return D3DERR_DEVICELOST;}
int main(void){IDirect3DDevice9Vtbl dv={.GetVertexDeclaration=declaration,.DrawPrimitiveUP=draw,.DrawIndexedPrimitiveUP=indexed};IDirect3DVertexDeclaration9Vtbl vv={.Release=release,.GetDeclaration=elements};struct pw_d3d9_up_upload u={0};device.lpVtbl=&dv;decl.lpVtbl=&vv;u.storage=bytes;u.capacity=sizeof(bytes);u.transfer=u.ready=1;u.draw=(struct pw_d3d9_up_draw){83,4,0,0,1,20,0,60,0};u.total=u.received=60;assert(pw_d3d9_native_up_dispatch(&device,&u)==0x1234);assert(draws==1&&releases==1);missing=1;assert(pw_d3d9_native_up_dispatch(&device,&u)==D3DERR_INVALIDCALL&&draws==1);u.draw.primitive_count=0;u.draw.vertex_bytes=u.total=u.received=0;assert(pw_d3d9_native_up_dispatch(&device,&u)==0x1234&&draws==2);missing=0;u.draw=(struct pw_d3d9_up_draw){84,4,2,3,1,20,101,100,6};u.total=u.received=106;assert(pw_d3d9_native_up_dispatch(&device,&u)==D3DERR_DEVICELOST&&draws==3);u.ready=0;assert(pw_d3d9_native_up_dispatch(&device,&u)==D3DERR_INVALIDCALL&&draws==3);puts("PASS native UP: exact methods/HRESULT, owned indexed prefix, declaration bound, zero-call dispatch, incomplete rejection");return 0;}
