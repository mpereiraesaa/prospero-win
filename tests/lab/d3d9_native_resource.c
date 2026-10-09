/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real native backend buffer proof plus one isolated null-output fault guard. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <assert.h>
#include "../../wine/ps5/d3d9/pw_d3d9_native_resource.h"
static struct pw_d3d9_resource_reply call(struct pw_d3d9_native_resource *r,struct pw_d3d9_resource_request *q)
{
 unsigned char wire[PW_D3D9_RESOURCE_MAX_WIRE];size_t n;
 struct pw_d3d9_resource_request decoded;struct pw_d3d9_resource_reply reply,result;
 assert(!pw_d3d9_resource_request_encode(wire,sizeof(wire),&n,q));
 assert(!pw_d3d9_resource_request_decode(&decoded,wire,n));
 pw_d3d9_native_resource_call(r,&decoded,&reply);
 assert(!pw_d3d9_resource_reply_encode(wire,sizeof(wire),&n,&reply));
 assert(!pw_d3d9_resource_reply_decode(&result,wire,n));return result;
}
static void buffer(IDirect3DDevice9 *device,int index,D3DPOOL pool)
{
 struct pw_d3d9_native_resource *r=NULL;struct pw_d3d9_resource_request q={0};
 struct pw_d3d9_resource_reply reply;uint64_t generation;uint32_t span,offset,i;
 q.operation=index?PW_D3D9_RESOURCE_CREATE_IB:PW_D3D9_RESOURCE_CREATE_VB;q.length=16384;
 q.pool=pool;q.usage=pool==D3DPOOL_DEFAULT?D3DUSAGE_DYNAMIC:0;q.format_fvf=index?D3DFMT_INDEX16:D3DFVF_XYZ;
 pw_d3d9_native_resource_create(device,&q,&reply,&r);assert(reply.hresult==S_OK&&r);
 assert(reply.desc.size==16384&&reply.desc.type==(index?D3DRTYPE_INDEXBUFFER:D3DRTYPE_VERTEXBUFFER));
 assert(pw_d3d9_native_resource_kind(r)==(index?PW_D3D9_KIND_INDEX_BUFFER:PW_D3D9_KIND_VERTEX_BUFFER));
 assert(pw_d3d9_native_resource_identity(r)&&pw_d3d9_native_resource_backend(r));
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_DESC};reply=call(r,&q);assert(reply.hresult==S_OK&&reply.desc.pool==pool);
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_LOCK,.offset=16385};reply=call(r,&q);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);
 q.offset=8;q.flags=D3DLOCK_DISCARD|D3DLOCK_NOOVERWRITE;reply=call(r,&q);assert(reply.hresult==S_OK&&reply.length==16376);
 generation=reply.lock_generation;span=reply.length;
 reply=call(r,&q);assert(reply.hresult==(uint32_t)E_NOTIMPL);
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_UNLOCK,.lock_generation=generation};reply=call(r,&q);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);
 q.operation=PW_D3D9_RESOURCE_WRITE;q.offset=1;q.count=1;reply=call(r,&q);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);
 for(offset=0;offset<span;offset+=q.count){
  q.offset=offset;q.count=span-offset<4096?span-offset:4096;
  for(i=0;i<q.count;i++)q.data[i]=(unsigned char)((offset+i)*17+index);
  reply=call(r,&q);assert(reply.hresult==S_OK);
  memset(q.data,0xee,sizeof(q.data)); /* caller storage can be overwritten */
 }
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_UNLOCK,.lock_generation=generation};reply=call(r,&q);assert(reply.hresult==S_OK);
 reply=call(r,&q);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_LOCK,.offset=8,.flags=D3DLOCK_READONLY};reply=call(r,&q);assert(reply.hresult==S_OK&&reply.lock_generation>generation);
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_READ,.lock_generation=generation,.count=1};reply=call(r,&q);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);
 q.lock_generation=++generation;
 for(offset=0;offset<span;offset+=q.count){
  q.offset=offset;q.count=span-offset<4096?span-offset:4096;reply=call(r,&q);assert(reply.hresult==S_OK);
  for(i=0;i<q.count;i++)assert(reply.data[i]==(unsigned char)((offset+i)*17+index));
 }
 q.operation=PW_D3D9_RESOURCE_WRITE;q.offset=0;q.count=1;reply=call(r,&q);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_UNLOCK,.lock_generation=generation};reply=call(r,&q);assert(reply.hresult==S_OK);
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_LOCK,.offset=8,.length=128,.flags=D3DLOCK_DISCARD};reply=call(r,&q);assert(reply.hresult==S_OK);
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_CANCEL_LOCK,.lock_generation=reply.lock_generation};reply=call(r,&q);assert(reply.hresult==S_OK);
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_LOCK,.flags=D3DLOCK_READONLY};reply=call(r,&q);assert(reply.hresult==S_OK);
 assert(pw_d3d9_native_resource_destroy(r)==S_OK);
 printf("PW_NATIVE_BUFFER kind=%u pool=%u bytes=%u pass=1\n",index?4:3,(unsigned)pool,span);fflush(stdout);
}
static void adopted_binding(IDirect3DDevice9 *device,int index)
{
 struct pw_d3d9_native_resource *r=NULL,*adopted=NULL;struct pw_d3d9_resource_reply reply;
 struct pw_d3d9_resource_request q={.operation=index?PW_D3D9_RESOURCE_CREATE_IB:PW_D3D9_RESOURCE_CREATE_VB,.length=1024,.format_fvf=index?D3DFMT_INDEX16:D3DFVF_XYZ};
 pw_d3d9_native_resource_create(device,&q,&reply,&r);assert(reply.hresult==S_OK&&r);
 uintptr_t identity=pw_d3d9_native_resource_identity(r);
 if(index)assert(IDirect3DDevice9_SetIndices(device,pw_d3d9_native_resource_backend(r))==S_OK);
 else assert(IDirect3DDevice9_SetStreamSource(device,0,pw_d3d9_native_resource_backend(r),0,12)==S_OK);
 assert(pw_d3d9_native_resource_destroy(r)==S_OK); /* backend binding retains it */
 if(index){IDirect3DIndexBuffer9 *buffer=NULL;assert(IDirect3DDevice9_GetIndices(device,&buffer)==S_OK&&buffer);
  assert(pw_d3d9_native_resource_adopt(device,PW_D3D9_KIND_INDEX_BUFFER,buffer,&adopted)==S_OK);}
 else {IDirect3DVertexBuffer9 *buffer=NULL;UINT offset,stride;assert(IDirect3DDevice9_GetStreamSource(device,0,&buffer,&offset,&stride)==S_OK&&buffer&&stride==12);
  assert(pw_d3d9_native_resource_adopt(device,PW_D3D9_KIND_VERTEX_BUFFER,buffer,&adopted)==S_OK);}
 assert(adopted&&pw_d3d9_native_resource_identity(adopted)==identity);
 if(index)assert(IDirect3DDevice9_SetIndices(device,NULL)==S_OK);else assert(IDirect3DDevice9_SetStreamSource(device,0,NULL,0,0)==S_OK);
 assert(pw_d3d9_native_resource_destroy(adopted)==S_OK);
 printf("PW_NATIVE_BUFFER_ADOPT kind=%u pass=1\n",index?4:3);
}
static HRESULT WINAPI null_buffer(IDirect3DDevice9 *d,UINT size,DWORD usage,DWORD fvf,D3DPOOL pool,IDirect3DVertexBuffer9 **out,HANDLE *shared)
{(void)d;(void)size;(void)usage;(void)fvf;(void)pool;(void)shared;*out=NULL;return S_OK;}
static void null_output_guard(void)
{
 static IDirect3DDevice9Vtbl vtable={.CreateVertexBuffer=null_buffer};IDirect3DDevice9 bad={&vtable};
 struct pw_d3d9_native_resource *r=NULL;struct pw_d3d9_resource_reply reply;
 struct pw_d3d9_resource_request q={.operation=PW_D3D9_RESOURCE_CREATE_VB,.length=128};
 pw_d3d9_native_resource_create(&bad,&q,&reply,&r);assert(reply.hresult==(uint32_t)E_FAIL&&!r);
}
int wmain(int argc,WCHAR **argv)
{
 HMODULE module;IDirect3D9 *(WINAPI *factory)(UINT);IDirect3D9 *d3d;IDirect3DDevice9 *device=NULL;
 HWND window;D3DPRESENT_PARAMETERS pp={0};HRESULT hr;WNDCLASSW cls={0};
 if(argc!=2)return 2;
 null_output_guard();
 cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW_NATIVE_RESOURCE";
 assert(RegisterClassW(&cls));window=CreateWindowW(cls.lpszClassName,L"resource",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 module=LoadLibraryW(argv[1]);assert(module);factory=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);d3d=factory(D3D_SDK_VERSION);assert(d3d);
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.hDeviceWindow=window;
 hr=IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device);assert(hr==S_OK&&device);
 adopted_binding(device,0);adopted_binding(device,1);
 buffer(device,0,D3DPOOL_DEFAULT);buffer(device,1,D3DPOOL_DEFAULT);buffer(device,0,D3DPOOL_MANAGED);buffer(device,1,D3DPOOL_MANAGED);
 /* Compare an actual backend creation failure with the adapter result. */
 {IDirect3DIndexBuffer9 *bad=NULL;struct pw_d3d9_native_resource *r=NULL;struct pw_d3d9_resource_reply reply;
 struct pw_d3d9_resource_request q={.operation=PW_D3D9_RESOURCE_CREATE_IB,.length=0,.format_fvf=D3DFMT_INDEX16};
 hr=IDirect3DDevice9_CreateIndexBuffer(device,0,0,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&bad,NULL);assert(FAILED(hr)&&!bad);
 pw_d3d9_native_resource_create(device,&q,&reply,&r);assert(reply.hresult==(uint32_t)hr&&!r);}
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);
 puts("PW_NATIVE_RESOURCE PASS");return 0;
}
