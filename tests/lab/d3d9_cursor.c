/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_cursor.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
static IDirect3DDevice9 guest,native;static IDirect3DSurface9 surface;static ULONG refs=1;static unsigned calls,held,released,failures,mode;
static ULONG WINAPI addref(IDirect3DDevice9 *d){assert(d==&guest);return ++refs;}
static ULONG WINAPI release(IDirect3DDevice9 *d){assert(d==&guest&&refs);return --refs;}
static ULONG WINAPI surface_release(IDirect3DSurface9 *s){assert(s==&surface&&held);held--;released++;return 1;}
static HRESULT WINAPI properties(IDirect3DDevice9 *d,UINT x,UINT y,IDirect3DSurface9 *s){assert(d==&native&&x==7&&y==9&&(!s||(s==&surface&&held)));calls++;return s?(HRESULT)0x1234:D3DERR_INVALIDCALL;}
static void WINAPI position(IDirect3DDevice9 *d,INT x,INT y,DWORD flags){assert(d==&native&&x==INT_MIN&&y==-1&&flags==0x80000001);calls++;}
static BOOL WINAPI show(IDirect3DDevice9 *d,BOOL b){assert(d==&native&&(uint32_t)b==0xffffffff);calls++;return (BOOL)0x80000002;}
static HRESULT resolve(IDirect3DDevice9 *d,IUnknown *s,uint32_t kind,struct pw_d3d9_object_ref *out){assert(d==&guest&&refs>=2&&s==(IUnknown *)&surface&&kind==6);*out=(struct pw_d3d9_object_ref){5,8};return S_OK;}
static HRESULT acquire(void *context,uint32_t id,uint32_t generation,uint32_t kind,IDirect3DDevice9 *d,void **out){assert(context==&surface&&id==5&&generation==8&&kind==6&&d==&native);held++;*out=&surface;return S_OK;}
static void fail(IDirect3DDevice9 *d,HRESULT hr){assert(d==&guest&&refs&&FAILED(hr));failures++;}
static HRESULT call(IDirect3DDevice9 *d,const struct pw_d3d9_cursor_request *q,struct pw_d3d9_cursor_reply *r){assert(d==&guest&&refs>=2);if(mode==1)return D3DERR_DEVICELOST;if(mode==2){*r=(struct pw_d3d9_cursor_reply){99,0,0};return S_OK;}if(mode==3){assert(refs==2);release(d);}return pw_d3d9_native_cursor(&native,q,r,acquire,&surface);}
int main(void)
{
 IDirect3DDevice9Vtbl gv={.AddRef=addref,.Release=release},nv={.SetCursorProperties=properties,.SetCursorPosition=position,.ShowCursor=show};IDirect3DSurface9Vtbl sv={.Release=surface_release};struct pw_d3d9_cursor_ops ops={call,resolve,fail};guest.lpVtbl=&gv;native.lpVtbl=&nv;surface.lpVtbl=&sv;pw_d3d9_cursor_install(&gv,&ops);
 assert(gv.SetCursorProperties(&guest,7,9,&surface)==0x1234&&released==1&&!held);assert(gv.SetCursorProperties(&guest,7,9,NULL)==D3DERR_INVALIDCALL);
 gv.SetCursorPosition(&guest,INT_MIN,-1,0x80000001);assert((uint32_t)gv.ShowCursor(&guest,(BOOL)0xffffffff)==0x80000002&&calls==4);
 mode=1;gv.SetCursorPosition(&guest,0,0,0);assert(!gv.ShowCursor(&guest,0)&&failures==2&&calls==4);assert(gv.SetCursorProperties(&guest,7,9,NULL)==D3DERR_DEVICELOST&&failures==2);
 mode=2;assert(!gv.ShowCursor(&guest,0)&&failures==3&&refs==1);mode=3;assert((uint32_t)gv.ShowCursor(&guest,(BOOL)0xffffffff)==0x80000002&&refs==0);
 puts("PASS cursor adapters: typed surface pin, exact native HRESULT/BOOL/signed bits, void dispatch, sticky failures, device lifetime");return 0;
}
