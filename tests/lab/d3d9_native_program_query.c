/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../wine/ps5/d3d9/pw_d3d9_native_program_query.h"
static void put(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static void readback(struct pw_d3d9_native_program *p)
{
 uint32_t kind=pw_d3d9_native_program_kind(p);unsigned char expected[8192],actual[8192],wire[4128];UINT required=0,size;HRESULT direct;
 void *object=pw_d3d9_native_program_backend(p);struct pw_d3d9_program_query_request q={.operation=PW_D3D9_PROGRAM_SIZE,.kind=kind,.capacity=17};struct pw_d3d9_program_query_reply r,out;size_t written;
 pw_d3d9_native_program_query(p,&q,&r);assert(r.hresult==S_OK);
 if(kind==7)direct=IDirect3DVertexDeclaration9_GetDeclaration((IDirect3DVertexDeclaration9 *)object,NULL,&required);
 else if(kind==8)direct=IDirect3DVertexShader9_GetFunction((IDirect3DVertexShader9 *)object,NULL,&required);
 else direct=IDirect3DPixelShader9_GetFunction((IDirect3DPixelShader9 *)object,NULL,&required);
 assert(r.hresult==(uint32_t)direct&&r.size==required&&r.total==required*(kind==7?8:1));
 const UINT capacities[]={0,1,3,17,8192};
 for(unsigned c=0;c<5;c++){
  memset(expected,0xab,sizeof(expected));memset(actual,0xab,sizeof(actual));size=capacities[c];
  if(kind==7)direct=IDirect3DVertexDeclaration9_GetDeclaration((IDirect3DVertexDeclaration9 *)object,(void *)expected,&size);
  else if(kind==8)direct=IDirect3DVertexShader9_GetFunction((IDirect3DVertexShader9 *)object,expected,&size);
  else direct=IDirect3DPixelShader9_GetFunction((IDirect3DPixelShader9 *)object,expected,&size);
  uint32_t available=kind==7?required*8:(capacities[c]<required?capacities[c]:required),offset=0;
  do {
   q=(struct pw_d3d9_program_query_request){PW_D3D9_PROGRAM_READ,kind,capacities[c],offset,4096};
   pw_d3d9_native_program_query(p,&q,&r);assert(r.hresult==(uint32_t)direct&&r.size==size);
   assert(!pw_d3d9_program_query_reply_encode(wire,sizeof(wire),&written,&q,&r));assert(!pw_d3d9_program_query_reply_decode(&out,&q,wire,written));
   assert(out.offset==offset&&out.count<=4096);memcpy(actual+offset,out.data,out.count);offset+=out.count;
  }while(offset<available);
  assert(!memcmp(expected,actual,sizeof(actual)));
 }
 q.kind=kind==7?8:7;pw_d3d9_native_program_query(p,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&!r.count&&!r.total);
}
static void program(IDirect3DDevice9 *device,uint32_t kind,const unsigned char *bytes,size_t count)
{
 unsigned char storage[8192],wire[PW_D3D9_PROGRAM_WIRE_MAX];
 struct pw_d3d9_program_upload upload;struct pw_d3d9_program_request q={0},decoded;
 struct pw_d3d9_native_program *p=NULL;uint64_t transfer;size_t offset,n;HRESULT hr;
 pw_d3d9_program_upload_init(&upload,storage,sizeof(storage));q.operation=PW_D3D9_PROGRAM_BEGIN;q.kind=kind;q.total=count;
 assert(!pw_d3d9_program_upload_apply(&upload,&q,&transfer));
 q=(struct pw_d3d9_program_request){.operation=PW_D3D9_PROGRAM_WRITE,.kind=kind,.transfer=transfer};
 for(offset=0;offset<count;offset+=q.count){
  q.offset=offset;q.count=count-offset<4096?count-offset:4096;memcpy(q.data,bytes+offset,q.count);
  assert(!pw_d3d9_program_encode(wire,sizeof(wire),&n,&q));assert(!pw_d3d9_program_decode(&decoded,wire,n));
  memset(q.data,0xee,sizeof(q.data));assert(!pw_d3d9_program_upload_apply(&upload,&decoded,&transfer));
 }
 q.operation=PW_D3D9_PROGRAM_COMMIT;q.offset=q.count=0;
 assert(!pw_d3d9_program_upload_apply(&upload,&q,&transfer)&&upload.ready);
 hr=pw_d3d9_native_program_create(device,kind,upload.storage,upload.total,&p);assert(hr==S_OK&&p);
 assert(pw_d3d9_native_program_kind(p)==kind&&pw_d3d9_native_program_identity(p));
 pw_d3d9_program_upload_finish(&upload);memset(storage,0,sizeof(storage));
 if(kind==PW_D3D9_PROGRAM_DECL){
  D3DVERTEXELEMENT9 e[65];UINT size=65;IDirect3DVertexDeclaration9 *decl=pw_d3d9_native_program_backend(p);
  assert(IDirect3DVertexDeclaration9_GetDeclaration(decl,e,&size)==S_OK&&size==2);
  assert(e[0].Stream==0&&e[0].Offset==0&&e[0].Type==D3DDECLTYPE_FLOAT3&&e[1].Stream==255);
  assert(IDirect3DDevice9_SetVertexDeclaration(device,decl)==S_OK);assert(IDirect3DDevice9_SetVertexDeclaration(device,NULL)==S_OK);
 }else {
  DWORD copied[2048];UINT size=sizeof(copied);size_t i;
  if(kind==PW_D3D9_PROGRAM_VS){
   IDirect3DVertexShader9 *shader=pw_d3d9_native_program_backend(p);
   assert(IDirect3DVertexShader9_GetFunction(shader,NULL,&size)==S_OK&&size==count);
   assert(IDirect3DVertexShader9_GetFunction(shader,copied,&size)==S_OK);
   assert(IDirect3DDevice9_SetVertexShader(device,shader)==S_OK);assert(IDirect3DDevice9_SetVertexShader(device,NULL)==S_OK);
  }else {
   IDirect3DPixelShader9 *shader=pw_d3d9_native_program_backend(p);
   assert(IDirect3DPixelShader9_GetFunction(shader,NULL,&size)==S_OK&&size==count);
   assert(IDirect3DPixelShader9_GetFunction(shader,copied,&size)==S_OK);
   assert(IDirect3DDevice9_SetPixelShader(device,shader)==S_OK);assert(IDirect3DDevice9_SetPixelShader(device,NULL)==S_OK);
  }
  assert(size==count);for(i=0;i<count/4;i++){unsigned char value[4];put(value,copied[i]);assert(!memcmp(value,bytes+4*i,4));}
 }
 {
  struct pw_d3d9_native_program *adopted=NULL;IUnknown *owned=pw_d3d9_native_program_backend(p);
  IUnknown_AddRef(owned);assert(pw_d3d9_native_program_adopt(device,kind,owned,&adopted)==S_OK&&adopted);
  assert(pw_d3d9_native_program_identity(adopted)==pw_d3d9_native_program_identity(p));
  pw_d3d9_native_program_destroy(adopted);
  IUnknown_AddRef(owned);assert(pw_d3d9_native_program_adopt(device,999,owned,&adopted)==(uint32_t)D3DERR_INVALIDCALL&&!adopted);
 }
 readback(p);pw_d3d9_native_program_destroy(p);printf("PW_NATIVE_PROGRAM kind=%u bytes=%u pass=1\n",kind,(unsigned)count);
}
int wmain(int argc,WCHAR **argv)
{
 WNDCLASSW cls={0};HWND window;HMODULE module;IDirect3D9 *(WINAPI *factory)(UINT);IDirect3D9 *d3d;
 IDirect3DDevice9 *device=NULL;D3DPRESENT_PARAMETERS pp={0};unsigned char vs[20],ps[5000]={0};size_t i;
 const DWORD vertex[]={0xfffe0101,1,0xc00f0000,0x90e40000,0xffff};
 const DWORD pixel[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0xffff};
 unsigned char decl[]={0,0,0,0,2,0,0,0,255,0,0,0,17,0,0,0};
 if(argc!=2)return 2;
 cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW_NATIVE_PROGRAM";
 assert(RegisterClassW(&cls));window=CreateWindowW(cls.lpszClassName,L"program",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 module=LoadLibraryW(argv[1]);assert(module);factory=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);d3d=factory(D3D_SDK_VERSION);assert(d3d);
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.hDeviceWindow=window;
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 for(i=0;i<5;i++)put(vs+4*i,vertex[i]);
 /* Opaque comment forces a multi-chunk upload; END inside it is payload. */
 put(ps,pixel[0]);put(ps+4,(1244u<<16)|0xfffe);put(ps+8,0xffff);
 for(i=1;i<5;i++)put(ps+(1245+i)*4,pixel[i]);
 program(device,PW_D3D9_PROGRAM_DECL,decl,sizeof(decl));program(device,PW_D3D9_PROGRAM_VS,vs,sizeof(vs));program(device,PW_D3D9_PROGRAM_PS,ps,sizeof(ps));
 {struct pw_d3d9_native_program *p=(void *)1;assert(pw_d3d9_native_program_create(device,PW_D3D9_PROGRAM_VS,vs,19,&p)==(uint32_t)D3DERR_INVALIDCALL&&!p);}
 {IDirect3DVertexDeclaration9 *implicit=NULL;struct pw_d3d9_native_program *p=NULL;
 assert(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZ|D3DFVF_DIFFUSE)==S_OK);
 assert(IDirect3DDevice9_GetVertexDeclaration(device,&implicit)==S_OK&&implicit);
 assert(pw_d3d9_native_program_adopt(device,7,implicit,&p)==S_OK&&p);readback(p);pw_d3d9_native_program_destroy(p);}
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);
 puts("PW_NATIVE_PROGRAM PASS");return 0;
}
