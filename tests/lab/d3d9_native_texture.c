/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../wine/ps5/d3d9/pw_d3d9_native_texture.h"
/* Include the real implementation to cover its signed-pitch row copier with
 * controlled storage as well as the public API against actual DXVK. */
#include "../../wine/ps5/d3d9/pw_d3d9_native_texture.c"
static struct pw_d3d9_texture_reply call(struct pw_d3d9_native_texture *r,struct pw_d3d9_texture_request *q,struct pw_d3d9_native_texture **child)
{
 struct pw_d3d9_texture_request decoded;struct pw_d3d9_texture_reply reply,result;
 unsigned char wire[PW_D3D9_TEXTURE_MAX_WIRE];size_t n;
 assert(!pw_d3d9_texture_request_encode(wire,sizeof(wire),&n,q));assert(!pw_d3d9_texture_request_decode(&decoded,wire,n));
 pw_d3d9_native_texture_call(r,&decoded,&reply,child);
 if(*child)reply.object=(struct pw_d3d9_object_ref){7,9};
 assert(!pw_d3d9_texture_reply_encode(wire,sizeof(wire),&n,&reply));assert(!pw_d3d9_texture_reply_decode(&result,wire,n));return result;
}
static struct pw_d3d9_native_texture *create(IDirect3DDevice9 *device,D3DFORMAT format,unsigned width,unsigned height,D3DPOOL pool,int surface)
{
 struct pw_d3d9_texture_request q={.operation=surface?PW_D3D9_TEXTURE_CREATE_SURFACE:PW_D3D9_TEXTURE_CREATE,.width=width,.height=height,.levels=1,.format=format,.pool=pool};
 struct pw_d3d9_texture_reply reply;struct pw_d3d9_native_texture *r=NULL;
 pw_d3d9_native_texture_create(device,&q,&reply,&r);assert(reply.hresult==S_OK&&r&&reply.levels==1&&reply.desc.width==width&&reply.desc.height==height);
 assert(pw_d3d9_native_texture_identity(r));return r;
}
static struct pw_d3d9_texture_reply lock(struct pw_d3d9_native_texture *r,unsigned flags)
{
 struct pw_d3d9_native_texture *child=NULL;struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_LOCK,.flags=flags};
 struct pw_d3d9_texture_reply reply=call(r,&q,&child);assert(reply.hresult==S_OK&&!child&&reply.pitch>0);return reply;
}
static void finish(struct pw_d3d9_native_texture *r,uint64_t generation,int cancel)
{
 struct pw_d3d9_native_texture *child=NULL;struct pw_d3d9_texture_request q={.operation=cancel?PW_D3D9_TEXTURE_CANCEL_LOCK:PW_D3D9_TEXTURE_UNLOCK,.lock_generation=generation};
 struct pw_d3d9_texture_reply reply=call(r,&q,&child);assert(reply.hresult==S_OK&&!child);
 reply=call(r,&q,&child);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);
}
static void write_bytes(struct pw_d3d9_native_texture *r,const struct pw_d3d9_texture_reply *layout,unsigned char *expected,int constant)
{
 struct pw_d3d9_native_texture *child=NULL;struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_WRITE,.lock_generation=layout->lock_generation};uint32_t offset,i;
 for(offset=0;offset<layout->length;offset+=q.count){
  q.offset=offset;q.count=layout->length-offset<4096?layout->length-offset:4096;
  for(i=0;i<q.count;i++){
   q.data[i]=constant>=0?(unsigned char)constant:(unsigned char)((offset+i)*13+7);
   if(expected)expected[offset+i]=(offset+i)%(uint32_t)layout->pitch<layout->row_bytes?q.data[i]:0;
  }
  struct pw_d3d9_texture_reply reply=call(r,&q,&child);assert(reply.hresult==S_OK);memset(q.data,0xee,sizeof(q.data));
 }
}
static void check_bytes(struct pw_d3d9_native_texture *r,const struct pw_d3d9_texture_reply *layout,const unsigned char *expected)
{
 struct pw_d3d9_native_texture *child=NULL;struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_READ,.lock_generation=layout->lock_generation};uint32_t offset;
 for(offset=0;offset<layout->length;offset+=q.count){
  q.offset=offset;q.count=layout->length-offset<4096?layout->length-offset:4096;
  struct pw_d3d9_texture_reply reply=call(r,&q,&child);assert(reply.hresult==S_OK&&reply.count==q.count&&!memcmp(reply.data,expected+offset,q.count));
 }
}
static void texture(IDirect3DDevice9 *device,D3DFORMAT format,unsigned width,unsigned height)
{
 struct pw_d3d9_native_texture *r=create(device,format,width,height,D3DPOOL_MANAGED,0),*surface=NULL,*again=NULL,*none=NULL;
 struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_SURFACE_LEVEL};
 struct pw_d3d9_texture_reply reply=call(r,&q,&surface);assert(reply.hresult==S_OK&&surface);
 reply=call(r,&q,&again);assert(reply.hresult==S_OK&&again&&pw_d3d9_native_texture_identity(surface)==pw_d3d9_native_texture_identity(again));assert(pw_d3d9_native_texture_destroy(again)==S_OK);
 struct pw_d3d9_texture_reply layout=lock(r,0);unsigned char *expected=malloc(layout.length);assert(expected);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_LOCK,.flags=D3DLOCK_READONLY};reply=call(surface,&q,&none);assert(reply.hresult==(uint32_t)E_NOTIMPL);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_READ,.lock_generation=layout.lock_generation+1,.count=1};reply=call(r,&q,&none);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);
 q.lock_generation=layout.lock_generation;q.offset=layout.length;reply=call(r,&q,&none);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);
 write_bytes(r,&layout,expected,-1);finish(r,layout.lock_generation,0);
 struct pw_d3d9_texture_reply read=lock(surface,D3DLOCK_READONLY);assert(read.length==layout.length);check_bytes(surface,&read,expected);finish(surface,read.lock_generation,0);
 if(format==D3DFMT_A8R8G8B8){
  q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_LOCK,.has_rect=1,.left=4,.top=4,.right=20,.bottom=16};
  reply=call(r,&q,&none);assert(reply.hresult==S_OK&&reply.rows==12&&reply.row_bytes==64);
  write_bytes(r,&reply,NULL,0x91);finish(r,reply.lock_generation,0);
  for(unsigned row=4;row<16;row++)memset(expected+row*(uint32_t)layout.pitch+16,0x91,64);
  read=lock(r,D3DLOCK_READONLY);check_bytes(r,&read,expected);finish(r,read.lock_generation,0);
 }
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_DIRTY};reply=call(r,&q,&none);assert(reply.hresult==S_OK);
 read=lock(r,D3DLOCK_READONLY);finish(r,read.lock_generation,1);
 read=lock(r,D3DLOCK_READONLY);assert(pw_d3d9_native_texture_destroy(r)==S_OK);
 assert(pw_d3d9_native_texture_destroy(surface)==S_OK);free(expected);
 printf("PW_NATIVE_TEXTURE format=%u rows=%u row_bytes=%u length=%u pass=1\n",(unsigned)format,layout.rows,layout.row_bytes,layout.length);fflush(stdout);
}
static void copies(IDirect3DDevice9 *device,int surface)
{
 struct pw_d3d9_native_texture *src=create(device,D3DFMT_A8R8G8B8,67,41,D3DPOOL_SYSTEMMEM,surface),*dst=create(device,D3DFMT_A8R8G8B8,67,41,D3DPOOL_DEFAULT,surface);
 struct pw_d3d9_texture_reply layout=lock(src,0),reply;
 unsigned char *expected=malloc(layout.length);assert(expected);write_bytes(src,&layout,expected,-1);finish(src,layout.lock_generation,0);
 struct pw_d3d9_texture_request q={.operation=surface?PW_D3D9_TEXTURE_UPDATE_SURFACE:PW_D3D9_TEXTURE_UPDATE};
 pw_d3d9_native_texture_copy(device,src,dst,&q,&reply);assert(reply.hresult==S_OK);
 if(surface){struct pw_d3d9_texture_reply read=lock(dst,D3DLOCK_READONLY);assert(read.length==layout.length);check_bytes(dst,&read,expected);finish(dst,read.lock_generation,0);}
 assert(pw_d3d9_native_texture_destroy(dst)==S_OK&&pw_d3d9_native_texture_destroy(src)==S_OK);free(expected);
 printf("PW_NATIVE_TEXTURE_COPY surface=%d pass=1\n",surface);fflush(stdout);
}
static void capacity(IDirect3DDevice9 *device)
{
 struct pw_d3d9_native_texture *items[129],*none=NULL;struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_LOCK,.flags=D3DLOCK_READONLY};
 for(unsigned i=0;i<129;i++){
  items[i]=create(device,D3DFMT_A8R8G8B8,1,1,D3DPOOL_SYSTEMMEM,1);
  struct pw_d3d9_texture_reply reply=call(items[i],&q,&none);
  assert(reply.hresult==(i<128?(uint32_t)S_OK:(uint32_t)E_OUTOFMEMORY));
 }
 for(unsigned i=0;i<129;i++)assert(pw_d3d9_native_texture_destroy(items[i])==S_OK);
 puts("PW_NATIVE_TEXTURE_CAPACITY pass=1");
}
static void adopt_existing(IDirect3DDevice9 *device)
{
 IDirect3DSurface9 *surface=NULL;struct pw_d3d9_native_texture *a=NULL,*b=NULL;struct pw_d3d9_texture_reply reply;struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_DESC};
 assert(IDirect3DDevice9_GetBackBuffer(device,0,0,D3DBACKBUFFER_TYPE_MONO,&surface)==S_OK&&surface);
 assert(pw_d3d9_native_texture_adopt(device,PW_D3D9_KIND_SURFACE,surface,&a)==S_OK&&a);
 assert(IDirect3DDevice9_GetRenderTarget(device,0,&surface)==S_OK&&surface);
 assert(pw_d3d9_native_texture_adopt(device,PW_D3D9_KIND_SURFACE,surface,&b)==S_OK&&b);
 assert(pw_d3d9_native_texture_identity(a)==pw_d3d9_native_texture_identity(b));
 assert(pw_d3d9_native_texture_destroy(b)==S_OK);
 assert(IDirect3DDevice9_GetBackBuffer(device,0,0,D3DBACKBUFFER_TYPE_MONO,&surface)==S_OK&&surface);
 assert(pw_d3d9_native_texture_adopt(device,PW_D3D9_KIND_TEXTURE_2D,surface,&b)==(uint32_t)E_NOINTERFACE&&!b);
 reply=call(a,&q,&b);assert(reply.hresult==S_OK&&reply.desc.width==64&&reply.desc.height==64&&!b);
 assert(pw_d3d9_native_texture_destroy(a)==S_OK);
 assert(IDirect3DDevice9_GetDepthStencilSurface(device,&surface)==S_OK&&surface);
 assert(pw_d3d9_native_texture_adopt(device,PW_D3D9_KIND_SURFACE,surface,&a)==S_OK&&a);
 assert(pw_d3d9_native_texture_destroy(a)==S_OK);
 a=create(device,D3DFMT_A8R8G8B8,16,16,D3DPOOL_MANAGED,0);
 uintptr_t identity=pw_d3d9_native_texture_identity(a);
 assert(IDirect3DDevice9_SetTexture(device,0,pw_d3d9_native_texture_backend(a))==S_OK);
 assert(pw_d3d9_native_texture_destroy(a)==S_OK);
 IDirect3DBaseTexture9 *texture=NULL;assert(IDirect3DDevice9_GetTexture(device,0,&texture)==S_OK&&texture);
 assert(pw_d3d9_native_texture_adopt(device,PW_D3D9_KIND_TEXTURE_2D,texture,&a)==S_OK&&a);
 assert(pw_d3d9_native_texture_identity(a)==identity);
 assert(IDirect3DDevice9_SetTexture(device,0,NULL)==S_OK);
 assert(pw_d3d9_native_texture_destroy(a)==S_OK);
 puts("PW_NATIVE_TEXTURE_ADOPT pass=1");
}
static void negative_pitch(void)
{
 unsigned char native[32],copy[28];memset(native,0xcc,sizeof(native));
 struct pw_d3d9_native_texture r={.mapping=native+16,.pitch=-16,.rows=2,.row_bytes=12,.length=28};
 for(unsigned i=0;i<12;i++){native[i]=(unsigned char)i;native[16+i]=(unsigned char)(20+i);}
 transfer(&r,0,copy,sizeof(copy),0);
 assert(!memcmp(copy,native,12)&&!memcmp(copy+16,native+16,12));
 for(unsigned i=12;i<16;i++)assert(!copy[i]);
 memset(copy,0x91,sizeof(copy));transfer(&r,0,copy,sizeof(copy),1);
 for(unsigned i=0;i<32;i++)assert(native[i]==((i%16)<12?0x91:0xcc));
 puts("PW_NATIVE_TEXTURE_NEGATIVE_PITCH pass=1");
}
int wmain(int argc,WCHAR **argv)
{
 HMODULE module;IDirect3D9 *(WINAPI *factory)(UINT);IDirect3D9 *d3d;IDirect3DDevice9 *device=NULL;HWND window;D3DPRESENT_PARAMETERS pp={0};WNDCLASSW cls={0};
 if(argc!=2)return 2;
 negative_pitch();
 cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW_NATIVE_TEXTURE";assert(RegisterClassW(&cls));
 window=CreateWindowW(cls.lpszClassName,L"texture",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 module=LoadLibraryW(argv[1]);assert(module);factory=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);d3d=factory(D3D_SDK_VERSION);assert(d3d);
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.hDeviceWindow=window;pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D16;
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 texture(device,D3DFMT_A8R8G8B8,67,41);texture(device,D3DFMT_DXT1,256,128);texture(device,D3DFMT_DXT3,256,128);texture(device,D3DFMT_DXT5,256,128);
 copies(device,0);copies(device,1);capacity(device);adopt_existing(device);
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);
 puts("PW_NATIVE_TEXTURE PASS");return 0;
}
