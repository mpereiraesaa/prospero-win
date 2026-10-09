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
static void surface_ops(IDirect3DDevice9 *device)
{
 struct pw_d3d9_native_texture *rt[2]={NULL,NULL},*depth=NULL,*unused=NULL,*mips=NULL;
 struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_CREATE_RT,.width=32,.height=32,.format=D3DFMT_A8R8G8B8,.lockable=TRUE},decoded;
 struct pw_d3d9_texture_reply reply;unsigned char wire[PW_D3D9_TEXTURE_MAX_WIRE];size_t n;
 for(unsigned i=0;i<2;i++){
  assert(!pw_d3d9_texture_request_encode(wire,sizeof(wire),&n,&q));assert(!pw_d3d9_texture_request_decode(&decoded,wire,n));
  pw_d3d9_native_texture_create(device,&decoded,&reply,&rt[i]);assert(reply.hresult==S_OK&&rt[i]&&reply.desc.usage==D3DUSAGE_RENDERTARGET);
 }
 q.operation=PW_D3D9_TEXTURE_CREATE_DEPTH;q.format=D3DFMT_D16;q.discard=TRUE;
 pw_d3d9_native_texture_create(device,&q,&reply,&depth);assert(reply.hresult==S_OK&&depth&&reply.desc.usage==D3DUSAGE_DEPTHSTENCIL);
 IDirect3DSurface9 *source=pw_d3d9_native_texture_backend(rt[0]),*destination=pw_d3d9_native_texture_backend(rt[1]);
 assert(IDirect3DDevice9_ColorFill(device,source,NULL,0xff123456)==S_OK);
 assert(IDirect3DDevice9_ColorFill(device,destination,NULL,0xff000000)==S_OK);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_STRETCH,.source={1,1},.destination={2,1},.has_rect=1,.left=2,.top=2,.right=10,.bottom=10,.has_destination_rect=1,.destination_left=4,.destination_top=4,.destination_right=20,.destination_bottom=20,.filter=D3DTEXF_POINT};
 assert(!pw_d3d9_texture_request_encode(wire,sizeof(wire),&n,&q));assert(!pw_d3d9_texture_request_decode(&decoded,wire,n));
 pw_d3d9_native_texture_copy(device,rt[0],rt[1],&decoded,&reply);assert(reply.hresult==S_OK);
 D3DLOCKED_RECT locked;assert(IDirect3DSurface9_LockRect(destination,&locked,NULL,D3DLOCK_READONLY)==S_OK);
 assert(*(DWORD *)((unsigned char *)locked.pBits+4*locked.Pitch+4*4)==0xff123456);
 assert(*(DWORD *)locked.pBits==0xff000000);assert(IDirect3DSurface9_UnlockRect(destination)==S_OK);
 q.has_rect=q.has_destination_rect=0;q.left=q.top=q.right=q.bottom=q.destination_left=q.destination_top=q.destination_right=q.destination_bottom=0;q.filter=D3DTEXF_NONE;
 pw_d3d9_native_texture_copy(device,rt[0],rt[1],&q,&reply);assert(reply.hresult==(uint32_t)IDirect3DDevice9_StretchRect(device,source,NULL,destination,NULL,D3DTEXF_NONE));
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_CREATE,.width=32,.height=16,.levels=0,.format=D3DFMT_A8R8G8B8,.pool=D3DPOOL_MANAGED};
 pw_d3d9_native_texture_create(device,&q,&reply,&mips);assert(reply.hresult==S_OK&&mips);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_DESC};reply=call(mips,&q,&unused);
 assert(reply.hresult==S_OK&&reply.levels==IDirect3DTexture9_GetLevelCount((IDirect3DTexture9 *)pw_d3d9_native_texture_backend(mips))&&reply.levels==6);
 reply=call(depth,&q,&unused);assert(reply.hresult==S_OK&&reply.levels==1);
 pw_d3d9_native_texture_destroy(mips);pw_d3d9_native_texture_destroy(depth);pw_d3d9_native_texture_destroy(rt[0]);pw_d3d9_native_texture_destroy(rt[1]);
 puts("PW_NATIVE_SURFACE_OPS pass=1");
}
static void surface_readback(IDirect3DDevice9 *device)
{
 struct pw_d3d9_native_texture *rt=NULL,*system=NULL,*unused=NULL;
 struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_CREATE_RT,.width=16,.height=16,.format=D3DFMT_A8R8G8B8};
 struct pw_d3d9_texture_reply reply;
 pw_d3d9_native_texture_create(device,&q,&reply,&rt);assert(reply.hresult==S_OK&&rt);
 system=create(device,D3DFMT_A8R8G8B8,16,16,D3DPOOL_SYSTEMMEM,1);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_COLOR_FILL,.color=0xff010203};
 reply=call(rt,&q,&unused);assert(reply.hresult==S_OK);
 q.has_rect=1;q.left=2;q.top=3;q.right=7;q.bottom=8;q.color=0xfffedcba;
 reply=call(rt,&q,&unused);assert(reply.hresult==S_OK);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_RT_DATA,.source={1,1},.destination={2,1}};
 struct pw_d3d9_texture_request decoded;unsigned char wire[PW_D3D9_TEXTURE_MAX_WIRE];size_t n;
 assert(!pw_d3d9_texture_request_encode(wire,sizeof(wire),&n,&q));assert(!pw_d3d9_texture_request_decode(&decoded,wire,n));
 pw_d3d9_native_texture_copy(device,rt,system,&decoded,&reply);assert(reply.hresult==S_OK);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_LOCK,.flags=D3DLOCK_READONLY};reply=call(system,&q,&unused);assert(reply.hresult==S_OK);
 uint64_t generation=reply.lock_generation;unsigned stride=(unsigned)reply.pitch;
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_READ,.lock_generation=generation,.count=reply.length};reply=call(system,&q,&unused);assert(reply.hresult==S_OK);
 DWORD pixel;memcpy(&pixel,reply.data,sizeof(pixel));assert(pixel==0xff010203);
 memcpy(&pixel,reply.data+3*stride+2*4,sizeof(pixel));assert(pixel==0xfffedcba);
 finish(system,generation,0);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_COLOR_FILL,.has_rect=1,.left=-1,.right=999,.bottom=999,.color=0};RECT invalid={-1,0,999,999};
 HRESULT direct=IDirect3DDevice9_ColorFill(device,pw_d3d9_native_texture_backend(rt),&invalid,0);reply=call(rt,&q,&unused);assert(reply.hresult==(uint32_t)direct);
 pw_d3d9_native_texture_destroy(rt);pw_d3d9_native_texture_destroy(system);
 puts("PW_NATIVE_SURFACE_READBACK pass=1");
}
static void hints(IDirect3DDevice9 *device)
{
 struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_CREATE,.width=64,.height=64,.levels=0,.format=D3DFMT_A8R8G8B8,.pool=D3DPOOL_MANAGED};
 struct pw_d3d9_native_texture *r=NULL,*surface=NULL,*none=NULL;struct pw_d3d9_texture_reply reply;
 pw_d3d9_native_texture_create(device,&q,&reply,&r);assert(reply.hresult==S_OK&&r);
 IDirect3DTexture9 *native=(IDirect3DTexture9 *)pw_d3d9_native_texture_backend(r);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_SET_PRIORITY,.value=0xfedcba98};
 DWORD before=IDirect3DTexture9_GetPriority(native);reply=call(r,&q,&none);assert(reply.hresult==S_OK&&reply.value==before);
 q.operation=PW_D3D9_TEXTURE_GET_PRIORITY;reply=call(r,&q,&none);assert(reply.hresult==S_OK&&reply.value==IDirect3DTexture9_GetPriority(native));
 q.operation=PW_D3D9_TEXTURE_SET_LOD;q.value=3;before=IDirect3DTexture9_GetLOD(native);reply=call(r,&q,&none);assert(reply.hresult==S_OK&&reply.value==before);
 q.operation=PW_D3D9_TEXTURE_GET_LOD;reply=call(r,&q,&none);assert(reply.hresult==S_OK&&reply.value==IDirect3DTexture9_GetLOD(native)&&reply.value==3);
 q.operation=PW_D3D9_TEXTURE_PRELOAD;reply=call(r,&q,&none);assert(reply.hresult==S_OK);
 q.operation=PW_D3D9_TEXTURE_SURFACE_LEVEL;q.level=0;reply=call(r,&q,&surface);assert(reply.hresult==S_OK&&surface);
 IDirect3DSurface9 *native_surface=(IDirect3DSurface9 *)pw_d3d9_native_texture_backend(surface);
 q.operation=PW_D3D9_TEXTURE_SET_PRIORITY;q.value=17;before=IDirect3DSurface9_GetPriority(native_surface);reply=call(surface,&q,&none);assert(reply.hresult==S_OK&&reply.value==before);
 q.operation=PW_D3D9_TEXTURE_GET_PRIORITY;reply=call(surface,&q,&none);assert(reply.hresult==S_OK&&reply.value==IDirect3DSurface9_GetPriority(native_surface));
 q.operation=PW_D3D9_TEXTURE_PRELOAD;reply=call(surface,&q,&none);assert(reply.hresult==S_OK);
 for(unsigned op=PW_D3D9_TEXTURE_GET_LOD;op<=PW_D3D9_TEXTURE_GENERATE_MIPS;op++){q.operation=op;reply=call(surface,&q,&none);assert(reply.hresult==(uint32_t)D3DERR_INVALIDCALL);}
 assert(pw_d3d9_native_texture_destroy(surface)==S_OK&&pw_d3d9_native_texture_destroy(r)==S_OK);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_CREATE,.width=64,.height=64,.levels=0,.usage=D3DUSAGE_AUTOGENMIPMAP,.format=D3DFMT_A8R8G8B8,.pool=D3DPOOL_MANAGED};
 pw_d3d9_native_texture_create(device,&q,&reply,&r);assert(reply.hresult==S_OK&&r);
 native=(IDirect3DTexture9 *)pw_d3d9_native_texture_backend(r);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_SET_AUTOGEN_FILTER,.value=D3DTEXF_POINT};reply=call(r,&q,&none);assert(reply.hresult==S_OK);
 q.operation=PW_D3D9_TEXTURE_GET_AUTOGEN_FILTER;reply=call(r,&q,&none);assert(reply.hresult==S_OK&&reply.value==D3DTEXF_POINT&&reply.value==IDirect3DTexture9_GetAutoGenFilterType(native));
 q.operation=PW_D3D9_TEXTURE_SET_AUTOGEN_FILTER;q.value=0;HRESULT expected=IDirect3DTexture9_SetAutoGenFilterType(native,0);reply=call(r,&q,&none);assert(reply.hresult==(uint32_t)expected);
 q.operation=PW_D3D9_TEXTURE_GENERATE_MIPS;reply=call(r,&q,&none);assert(reply.hresult==S_OK);
 assert(pw_d3d9_native_texture_destroy(r)==S_OK);puts("PW_NATIVE_TEXTURE_HINTS pass=1");
}
static void containers(IDirect3DDevice9 *device)
{
 struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_CREATE,.width=64,.height=64,.levels=0,.format=D3DFMT_A8R8G8B8,.pool=D3DPOOL_MANAGED};
 struct pw_d3d9_native_texture *texture=NULL,*surface=NULL,*adopted=NULL,*none=NULL;struct pw_d3d9_texture_reply reply;void *owned=NULL;
 pw_d3d9_native_texture_create(device,&q,&reply,&texture);assert(reply.hresult==S_OK&&texture);
 uintptr_t identity=pw_d3d9_native_texture_identity(texture);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_SURFACE_LEVEL};reply=call(texture,&q,&surface);assert(reply.hresult==S_OK&&surface);
 assert(pw_d3d9_native_texture_destroy(texture)==S_OK); /* child owns texture */
 for(unsigned selector=PW_D3D9_CONTAINER_UNKNOWN;selector<=PW_D3D9_CONTAINER_SWAPCHAIN;selector++){
  q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_CONTAINER,.value=selector};
  pw_d3d9_native_texture_container(surface,&q,&reply,&owned);
  if(selector==PW_D3D9_CONTAINER_DEVICE||selector==PW_D3D9_CONTAINER_SWAPCHAIN){assert(reply.hresult==(uint32_t)E_NOINTERFACE&&!owned);continue;}
  assert(reply.hresult==S_OK&&owned&&reply.container_kind==PW_D3D9_KIND_TEXTURE_2D&&reply.levels==7);
  assert(pw_d3d9_native_texture_adopt(device,reply.container_kind,owned,&adopted)==S_OK&&pw_d3d9_native_texture_identity(adopted)==identity);
  assert(pw_d3d9_native_texture_destroy(adopted)==S_OK);
 }
 q.value=PW_D3D9_CONTAINER_TEXTURE_2D;pw_d3d9_native_texture_container(surface,&q,&reply,&owned);assert(reply.hresult==S_OK&&owned);
 assert(pw_d3d9_native_texture_adopt(device,reply.container_kind,owned,&adopted)==S_OK);
 assert(pw_d3d9_native_texture_destroy(surface)==S_OK);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_DESC};reply=call(adopted,&q,&none);assert(reply.hresult==S_OK&&reply.desc.width==64&&reply.levels==7);
 assert(pw_d3d9_native_texture_destroy(adopted)==S_OK);
 surface=create(device,D3DFMT_A8R8G8B8,16,16,D3DPOOL_SYSTEMMEM,1);
 for(unsigned selector=PW_D3D9_CONTAINER_UNKNOWN;selector<=PW_D3D9_CONTAINER_DEVICE;selector++){
  q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_CONTAINER,.value=selector};pw_d3d9_native_texture_container(surface,&q,&reply,&owned);
  assert(reply.hresult==S_OK&&reply.container_kind==PW_D3D9_KIND_DEVICE&&!reply.levels&&owned==device);IDirect3DDevice9_Release((IDirect3DDevice9 *)owned);
 }
 q.value=PW_D3D9_CONTAINER_TEXTURE_2D;pw_d3d9_native_texture_container(surface,&q,&reply,&owned);assert(reply.hresult==(uint32_t)E_NOINTERFACE&&!owned);
 assert(pw_d3d9_native_texture_destroy(surface)==S_OK);
 IDirect3DSurface9 *back=NULL;IDirect3DSwapChain9 *swap=NULL;
 assert(IDirect3DDevice9_GetBackBuffer(device,0,0,D3DBACKBUFFER_TYPE_MONO,&back)==S_OK);
 assert(IDirect3DSurface9_GetContainer(back,&IID_IDirect3DSwapChain9,(void **)&swap)==S_OK&&swap);IDirect3DSwapChain9_Release(swap);
 assert(pw_d3d9_native_texture_adopt(device,PW_D3D9_KIND_SURFACE,back,&surface)==S_OK);
 q.value=PW_D3D9_CONTAINER_UNKNOWN;pw_d3d9_native_texture_container(surface,&q,&reply,&owned);assert(reply.hresult==(uint32_t)E_NOINTERFACE&&!owned&&!reply.container_kind);
 assert(pw_d3d9_native_texture_destroy(surface)==S_OK);puts("PW_NATIVE_CONTAINER pass=1");
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
 copies(device,0);copies(device,1);capacity(device);adopt_existing(device);surface_ops(device);surface_readback(device);hints(device);containers(device);
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);
 puts("PW_NATIVE_TEXTURE PASS");return 0;
}
