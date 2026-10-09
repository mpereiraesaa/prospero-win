/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_stateblock.h"
#include <assert.h>
#include <stdio.h>
static IDirect3DDevice9 device;static IDirect3DStateBlock9 block;static HRESULT result=0x1234;static unsigned calls,releases;static int produce=1;
static HRESULT WINAPI create(IDirect3DDevice9 *d,D3DSTATEBLOCKTYPE type,IDirect3DStateBlock9 **out){assert(d==&device && type==0xffffffffu);calls++;if(produce)*out=&block;return result;}
static HRESULT WINAPI begin(IDirect3DDevice9 *d){assert(d==&device);calls++;return result;}
static HRESULT WINAPI end(IDirect3DDevice9 *d,IDirect3DStateBlock9 **out){assert(d==&device);calls++;if(produce)*out=&block;return result;}
static HRESULT WINAPI capture(IDirect3DStateBlock9 *b){assert(b==&block);calls++;return result;}
static ULONG WINAPI release(IDirect3DStateBlock9 *b){assert(b==&block);releases++;return 0;}
int main(void)
{
 IDirect3DDevice9Vtbl dv={0};IDirect3DStateBlock9Vtbl bv={0};unsigned slots[]={59,60,61,4,5};
 dv.CreateStateBlock=create;dv.BeginStateBlock=begin;dv.EndStateBlock=end;device.lpVtbl=&dv;
 bv.Capture=capture;bv.Apply=capture;bv.Release=release;block.lpVtbl=&bv;
 for(unsigned i=0;i<5;i++){
  struct pw_d3d9_stateblock_request q={slots[i],slots[i]==59?0xffffffffu:0};IDirect3DStateBlock9 *out=NULL;
  assert(pw_d3d9_native_stateblock_dispatch(&device,&block,&q,&out)==0x1234);
  assert(out==((slots[i]==59 || slots[i]==61)?&block:NULL));
  result=D3DERR_INVALIDCALL;out=(void *)(uintptr_t)1;
  assert(pw_d3d9_native_stateblock_dispatch(&device,&block,&q,&out)==D3DERR_INVALIDCALL && out==(void *)(uintptr_t)1);result=0x1234;
 }
 assert(calls==10 && releases==2);
 {struct pw_d3d9_stateblock_request q={59,0xffffffffu};IDirect3DStateBlock9 *out=NULL;produce=0;assert(pw_d3d9_native_stateblock_dispatch(&device,NULL,&q,&out)==E_FAIL && !out);assert(pw_d3d9_native_stateblock_dispatch(NULL,NULL,&q,&out)==D3DERR_INVALIDCALL);q.method=0;assert(pw_d3d9_native_stateblock_dispatch(&device,NULL,&q,&out)==E_NOTIMPL);}
 puts("PASS native stateblock:5 actual ABI calls, exact HRESULT, owned outputs and failure cleanup");return 0;
}
