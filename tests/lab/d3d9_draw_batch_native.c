/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../wine/ps5/d3d9/pw_d3d9_service_methods.h"
#include "../../wine/ps5/d3d9/pw_d3d9_service_stateblock.h"
#include "../../wine/ps5/d3d9/pw_d3d9_native_command.h"
#include "../../wine/ps5/d3d9/pw_d3d9_native_draw_state.h"
#include "../../wine/ps5/d3d9/pw_d3d9_service_resource.h"
#include "../../wine/ps5/pw_d3d9_command_batch.h"
#include "../../wine/ps5/pw_d3d9_bridge_wire.h"
/* Test-only local context and typed registry acquisition. The production batch,
 * lease, command, stateblock, registry and codec implementations are linked. */
struct pw_d3d9_native_device {IDirect3DDevice9 *backend;unsigned recording;};
static struct pw_d3d9_native_device context;
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *p){return p->backend;}
unsigned pw_d3d9_native_device_recording(struct pw_d3d9_native_device *p){return p->recording;}
void pw_d3d9_native_device_recording_outcome(struct pw_d3d9_native_device *p,uint32_t method,uint32_t hr)
{pw_d3d9_native_recording_outcome(&p->recording,method,hr);}
HRESULT pw_d3d9_service_resource_acquire(struct pw_d3d9_objects *o,struct pw_d3d9_object_ref ref,uint32_t kind,void *device,void **out)
{
 const struct pw_d3d9_object_slot *s=pw_d3d9_object_lookup(o,o->device,o->epoch,ref);*out=NULL;
 if(!s||s->kind!=kind||device!=context.backend)return D3DERR_INVALIDCALL;
 IUnknown_AddRef((IUnknown *)s->context);*out=(void *)s->context;return S_OK;
}
static struct pw_d3d9_objects objects;static struct pw_d3d9_object_slot slots[16];
static struct pw_d3d9_object_ref parent,refs[6];
static struct pw_d3d9_service_batch_state stream;
static unsigned transactions,commands;
static struct pw_d3d9_object_ref publish(void *p,uint32_t kind)
{
 struct pw_d3d9_object_ref r;assert(pw_d3d9_object_reserve(&objects,&r));
 assert(pw_d3d9_object_commit(&objects,r,(uintptr_t)p,(uintptr_t)p,kind));return r;
}
static void execute(int batched,struct pw_d3d9_command *c,unsigned count)
{
 unsigned char wire[PW_D3D9_BATCH_MAX],out[PW_D3D9_BATCH_REPLY];size_t bytes,n;HRESULT hr;
 if(batched){
  struct pw_d3d9_command_batch b;struct pw_d3d9_batch_reply reply;
  assert(!pw_d3d9_batch_init(&b,stream.next_sequence));
  for(unsigned i=0;i<count;i++)assert(!pw_d3d9_batch_append(&b,c+i));
  assert(!pw_d3d9_batch_encode(wire,sizeof(wire),&n,&b));
  assert(pw_d3d9_service_batch(&stream,&objects,parent,wire,n,out,sizeof(out),&bytes,&hr));
  assert(hr==S_OK&&!pw_d3d9_batch_reply_decode(&reply,out,bytes,b.first_sequence,count));
  assert(reply.attempted==count&&reply.failed_index==UINT32_MAX);transactions++;
 }else for(unsigned i=0;i<count;i++){
  assert(!pw_d3d9_command_encode(wire,sizeof(wire),&n,c+i));
  assert(pw_d3d9_service_methods(&objects,parent,PW_D3D9_COMMAND_CALL,wire,n,out,sizeof(out),&bytes,&hr)&&hr==S_OK);
  transactions++;
 }
 commands+=count;
 for(unsigned i=0;i<16;i++)assert(!slots[i].queued_refs); /* No stateblock alive at these boundaries. */
}
static struct pw_d3d9_object_ref stateblock(unsigned method,struct pw_d3d9_object_ref target)
{
 struct pw_d3d9_stateblock_request q={method,0};struct pw_d3d9_stateblock_reply r;
 pw_d3d9_service_stateblock_call(&objects,target,&q,&r);assert(r.hresult==S_OK);return r.object;
}
static void destroy(struct pw_d3d9_object_ref r,int block)
{
 uintptr_t p;assert(pw_d3d9_object_release(&objects,r));assert(pw_d3d9_object_take_destroy(&objects,r,&p));
 if(block)assert(pw_d3d9_service_stateblock_destroy(&objects,p)==S_OK);
 else IUnknown_Release((IUnknown *)p);
 assert(pw_d3d9_object_finish_destroy(&objects,r));
}
static void bind_resources(int batched)
{
 struct pw_d3d9_command c[8]={
 {.method=65,.args={0,refs[0].id,refs[0].generation}},
 {.method=87,.args={refs[1].id,refs[1].generation}},
 {.method=92,.args={refs[2].id,refs[2].generation}},
 {.method=100,.args={0,refs[3].id,refs[3].generation,0,20}},
 {.method=104,.args={refs[4].id,refs[4].generation}},
 {.method=107,.args={refs[5].id,refs[5].generation}},
 {.method=92},{.method=107}};
 execute(batched,c,8);
}
static uint64_t pixels(IDirect3DDevice9 *d)
{
 IDirect3DSurface9 *rt=NULL,*copy=NULL;D3DLOCKED_RECT lock;uint64_t hash=1469598103934665603ULL;
 assert(IDirect3DDevice9_GetRenderTarget(d,0,&rt)==S_OK);
 assert(IDirect3DDevice9_CreateOffscreenPlainSurface(d,64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&copy,NULL)==S_OK);
 assert(IDirect3DDevice9_GetRenderTargetData(d,rt,copy)==S_OK);
 assert(IDirect3DSurface9_LockRect(copy,&lock,NULL,D3DLOCK_READONLY)==S_OK);
 assert((*(DWORD *)((char *)lock.pBits+16*lock.Pitch+16*4)&0xffffff)==0xff0000);
 assert((*(DWORD *)((char *)lock.pBits+60*lock.Pitch+60*4)&0xffffff)==0);
 for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
  hash^=(*(DWORD *)((char *)lock.pBits+y*lock.Pitch+4*x)&0xffffff);hash*=1099511628211ULL;
 }
 assert(IDirect3DSurface9_UnlockRect(copy)==S_OK);IDirect3DSurface9_Release(copy);IDirect3DSurface9_Release(rt);return hash;
}
static uint64_t draw(int batched,int indexed,int record)
{
 IDirect3DDevice9 *d=context.backend;
 assert(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,0xff000000,1,0)==S_OK);
 assert(IDirect3DDevice9_BeginScene(d)==S_OK);
 if(record){stateblock(PW_D3D9_SB_BEGIN,parent);assert(context.recording==PW_D3D9_RECORDING_ACTIVE);}
 struct pw_d3d9_command c[2]={ {.method=87}, {.method=indexed?82:81,.args={D3DPT_TRIANGLELIST,0,0}} };
 /* Recorded NULL declaration must not erase the currently active declaration. */
 if(!record){c[0].args[0]=refs[1].id;c[0].args[1]=refs[1].generation;}
 if(indexed){c[1].args[3]=3;c[1].args[5]=1;}else c[1].args[2]=1;
 execute(batched,c,2);
 if(record){
  struct pw_d3d9_object_ref block=stateblock(PW_D3D9_SB_END,parent);
  assert(context.recording==PW_D3D9_RECORDING_LIVE);
  stateblock(PW_D3D9_SB_APPLY,block);IDirect3DVertexDeclaration9 *decl=(void *)1;
  assert(IDirect3DDevice9_GetVertexDeclaration(d,&decl)==S_OK&&decl);
  assert((void *)decl==(void *)slots[refs[1].id-1].context);
  IDirect3DVertexDeclaration9_Release(decl);destroy(block,1);
 }
 assert(IDirect3DDevice9_EndScene(d)==S_OK);return pixels(d);
}
static void cycle(IDirect3D9 *factory,HWND window,unsigned cycle)
{
 IDirect3DDevice9 *d=NULL;D3DPRESENT_PARAMETERS pp={.Windowed=TRUE,.SwapEffect=D3DSWAPEFFECT_DISCARD,
 .BackBufferFormat=D3DFMT_X8R8G8B8,.BackBufferWidth=64,.BackBufferHeight=64,.hDeviceWindow=window};
 assert(IDirect3D9_CreateDevice(factory,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d)==S_OK);
 context=(struct pw_d3d9_native_device){d,PW_D3D9_RECORDING_LIVE};
 assert(pw_d3d9_objects_init(&objects,slots,16,1,cycle+1));parent=publish(&context,PW_D3D9_KIND_DEVICE);
 IDirect3DTexture9 *texture;IDirect3DVertexDeclaration9 *decl;IDirect3DVertexShader9 *vs;
 IDirect3DPixelShader9 *ps;IDirect3DVertexBuffer9 *vb;IDirect3DIndexBuffer9 *ib;void *data;
 const DWORD vertex[]={0xfffe0101,1,0xc00f0000,0x90e40000,0xffff};
 const DWORD pixel[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0xffff};
 const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITIONT,0},{0,16,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
 struct vertex {float x,y,z,w;DWORD color;} vertices[3]={{4,4,.5f,1,0xffff0000},{60,4,.5f,1,0xffff0000},{4,60,.5f,1,0xffff0000}};
 WORD indices[]={0,1,2};
 assert(IDirect3DDevice9_CreateTexture(d,4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,NULL)==S_OK);
 assert(IDirect3DDevice9_CreateVertexDeclaration(d,elements,&decl)==S_OK);
 assert(IDirect3DDevice9_CreateVertexShader(d,vertex,&vs)==S_OK);
 assert(IDirect3DDevice9_CreatePixelShader(d,pixel,&ps)==S_OK);
 assert(IDirect3DDevice9_CreateVertexBuffer(d,sizeof(vertices),0,0,D3DPOOL_MANAGED,&vb,NULL)==S_OK);
 assert(IDirect3DDevice9_CreateIndexBuffer(d,sizeof(indices),0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib,NULL)==S_OK);
 assert(IDirect3DVertexBuffer9_Lock(vb,0,0,&data,0)==S_OK);memcpy(data,vertices,sizeof(vertices));assert(IDirect3DVertexBuffer9_Unlock(vb)==S_OK);
 assert(IDirect3DIndexBuffer9_Lock(ib,0,0,&data,0)==S_OK);memcpy(data,indices,sizeof(indices));assert(IDirect3DIndexBuffer9_Unlock(ib)==S_OK);
 refs[0]=publish(texture,5);refs[1]=publish(decl,7);refs[2]=publish(vs,8);refs[3]=publish(vb,3);refs[4]=publish(ib,4);refs[5]=publish(ps,9);
 uint64_t expected[4]={0};unsigned totals[2];
 for(unsigned mode=0;mode<2;mode++){
  transactions=commands=0;pw_d3d9_service_batch_init(&stream);
  assert(pw_d3d9_service_batch_bindings(&stream,1)&&pw_d3d9_service_batch_draws(&stream,1));
  for(unsigned reset=0;reset<2;reset++){
   if(reset){assert(IDirect3DDevice9_Reset(d,&pp)==S_OK);context.recording=PW_D3D9_RECORDING_LIVE;}
   struct pw_d3d9_command setup[]={ {.method=57,.args={D3DRS_ZENABLE,0}}, {.method=57,.args={D3DRS_CULLMODE,D3DCULL_NONE}},
    {.method=57,.args={D3DRS_LIGHTING,0}}, {.method=67,.args={0,D3DTSS_COLOROP,D3DTOP_SELECTARG1}}, {.method=67,.args={0,D3DTSS_COLORARG1,D3DTA_DIFFUSE}} };
   execute(mode,setup,5);
   for(unsigned indexed=0;indexed<2;indexed++){
    bind_resources(mode);uint64_t hash=draw(mode,indexed,indexed);unsigned index=reset*2+indexed;
    if(!mode)expected[index]=hash;else assert(hash==expected[index]);
   }
  }
  totals[mode]=transactions;assert(commands==50);
 }
 assert(totals[0]==50&&totals[1]==10);
 assert(IDirect3DDevice9_SetTexture(d,0,NULL)==S_OK);assert(IDirect3DDevice9_SetVertexDeclaration(d,NULL)==S_OK);
 assert(IDirect3DDevice9_SetVertexShader(d,NULL)==S_OK);assert(IDirect3DDevice9_SetPixelShader(d,NULL)==S_OK);
 assert(IDirect3DDevice9_SetStreamSource(d,0,NULL,0,0)==S_OK);assert(IDirect3DDevice9_SetIndices(d,NULL)==S_OK);
 for(unsigned i=0;i<6;i++)destroy(refs[i],0);
 uintptr_t dead;assert(pw_d3d9_object_release(&objects,parent)&&pw_d3d9_object_take_destroy(&objects,parent,&dead));
 assert(pw_d3d9_object_finish_destroy(&objects,parent));
 for(unsigned i=0;i<16;i++)assert(slots[i].state==PW_D3D9_FREE&&!slots[i].queued_refs);
 assert(IDirect3DDevice9_Release(d)==0);
 printf("PW_DRAW_BATCH_NATIVE cycle=%u modes=2 six_bindings=1 draws=8 recording_apply=4 reset=2 direct_transactions=%u batch_transactions=%u device_final=0 status=0\n",cycle,totals[0],totals[1]);fflush(stdout);
}
int wmain(int argc,WCHAR **argv)
{
 assert(argc==2);WNDCLASSW cls={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW_DRAW_BATCH_NATIVE"};
 assert(RegisterClassW(&cls));HWND window=CreateWindowW(cls.lpszClassName,L"draw batch",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 HMODULE module=LoadLibraryW(argv[1]);assert(module);IDirect3D9 *(WINAPI *create)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");assert(create);
 IDirect3D9 *factory=create(D3D_SDK_VERSION);assert(factory);
 for(unsigned i=0;i<3;i++)cycle(factory,window,i);
 assert(IDirect3D9_Release(factory)==0);assert(DestroyWindow(window));assert(UnregisterClassW(cls.lpszClassName,cls.hInstance));FreeLibrary(module);return 0;
}
