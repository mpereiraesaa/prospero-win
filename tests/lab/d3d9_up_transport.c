/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include "../../wine/ps5/d3d9/pw_d3d9_up_client.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {HRESULT actual=(x);if(FAILED(actual)){fprintf(stderr,"FAIL line=%d hr=%08lx\n",__LINE__,actual);return 1;}}while(0)
static struct pw_d3d9_session *session;static struct pw_d3d9_object_ref device_ref;static ULONG refs=1;static unsigned failures;
static ULONG WINAPI addref(IDirect3DDevice9 *d){(void)d;return ++refs;}
static ULONG WINAPI release(IDirect3DDevice9 *d){(void)d;return --refs;}
static HRESULT up(IDirect3DDevice9 *d,const struct pw_d3d9_up_request *q,struct pw_d3d9_up_reply *r){(void)d;return pw_d3d9_session_up(session,device_ref,q,r);}
static void fail(IDirect3DDevice9 *d,HRESULT hr){(void)d;(void)hr;failures++;pw_d3d9_session_cancel(session);}
static HRESULT command(unsigned method,unsigned a,unsigned b){struct pw_d3d9_command q={.method=method,.args={a,b}};return pw_d3d9_session_command(session,device_ref,&q);}
int wmain(int argc,WCHAR **argv)
{
 struct pw_d3d9_object_ref factory,target,readback;struct pw_d3d9_device_reply dr;struct pw_d3d9_texture_request tq;struct pw_d3d9_texture_reply tr;
 IDirect3DDevice9Vtbl table={.AddRef=addref,.Release=release};IDirect3DDevice9 guest={&table};struct pw_d3d9_up_client_ops ops={up,fail};
 struct vertex{float x,y,z,w;DWORD color;} vertices[5]={0};WORD indices[3]={2,3,4};
 if(argc!=3)return 2;
 pw_d3d9_up_client_install(&table,&ops);
 for(unsigned cycle=0;cycle<3;cycle++){
  CHECK(pw_d3d9_session_open(argv[1],argv[2],&session));CHECK(pw_d3d9_session_create(session,D3D_SDK_VERSION,&factory));
  struct pw_d3d9_device_request dq={.operation=1,.device_type=D3DDEVTYPE_HAL,.behavior_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING,.focus_window={1,1,1},.parameters={.width=64,.height=64,.format=D3DFMT_A8R8G8B8,.count=1,.swap_effect=D3DSWAPEFFECT_DISCARD,.windowed=1,.interval=D3DPRESENT_INTERVAL_IMMEDIATE,.window={1,1,1}}};
  CHECK(pw_d3d9_session_device(session,factory,&dq,&dr));device_ref=dr.object;
  /* Backend rejects missing declaration; committed transfer must be consumed. */
  if(table.DrawPrimitiveUP(&guest,D3DPT_TRIANGLELIST,0,NULL,20)!=D3DERR_INVALIDCALL||failures)return 3;
  CHECK(command(89,D3DFVF_XYZRHW|D3DFVF_DIFFUSE,0));CHECK(table.DrawPrimitiveUP(&guest,D3DPT_TRIANGLELIST,0,NULL,20));
  CHECK(command(57,D3DRS_LIGHTING,FALSE));CHECK(command(57,D3DRS_ZENABLE,FALSE));CHECK(command(57,D3DRS_CULLMODE,D3DCULL_NONE));
  tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_CREATE_RT,.width=64,.height=64,.format=D3DFMT_A8R8G8B8};CHECK(pw_d3d9_session_texture(session,device_ref,&tq,&tr));target=tr.object;
  struct pw_d3d9_command bind={.method=37,.args={0,target.id,target.generation}};CHECK(pw_d3d9_session_command(session,device_ref,&bind));
  tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_CREATE_SURFACE,.width=64,.height=64,.format=D3DFMT_A8R8G8B8,.pool=D3DPOOL_SYSTEMMEM};CHECK(pw_d3d9_session_texture(session,device_ref,&tq,&tr));readback=tr.object;
  for(unsigned indexed=0;indexed<2;indexed++){
   DWORD color=indexed?0xff00ff00:0xffff0000;vertices[2]=(struct vertex){0,0,0,1,color};vertices[3]=(struct vertex){63,0,0,1,color};vertices[4]=(struct vertex){0,63,0,1,color};
   struct pw_d3d9_command clear={.method=43,.args={0,D3DCLEAR_TARGET,0xff000000,0x3f800000,0}};CHECK(pw_d3d9_session_command(session,device_ref,&clear));CHECK(command(41,0,0));
   CHECK(indexed?table.DrawIndexedPrimitiveUP(&guest,D3DPT_TRIANGLELIST,2,3,1,indices,D3DFMT_INDEX16,vertices,20):table.DrawPrimitiveUP(&guest,D3DPT_TRIANGLELIST,1,vertices+2,20));CHECK(command(42,0,0));
   tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_RT_DATA,.source=target,.destination=readback};CHECK(pw_d3d9_session_texture(session,device_ref,&tq,&tr));
   tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_LOCK,.flags=D3DLOCK_READONLY};CHECK(pw_d3d9_session_texture(session,readback,&tq,&tr));uint64_t generation=tr.lock_generation;
   unsigned offset=(tr.pitch>=0?16:tr.rows-17)*(unsigned)(tr.pitch>=0?tr.pitch:-tr.pitch)+64;
   tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_READ,.lock_generation=generation,.offset=offset,.count=4};CHECK(pw_d3d9_session_texture(session,readback,&tq,&tr));DWORD pixel;memcpy(&pixel,tr.data,4);if((pixel&0xffffff)!=(color&0xffffff))return 4;
   tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_UNLOCK,.lock_generation=generation};CHECK(pw_d3d9_session_texture(session,readback,&tq,&tr));
  }
  CHECK(pw_d3d9_session_release(session,target));CHECK(pw_d3d9_session_release(session,readback));
  /* Leave a transfer active. Shutdown must drop its persistent device pin. */
  struct pw_d3d9_up_request pending={.operation=PW_D3D9_UP_BEGIN,.draw={83,4,0,0,0,20,0,0,0}};struct pw_d3d9_up_reply reply;CHECK(pw_d3d9_session_up(session,device_ref,&pending,&reply));
  CHECK(pw_d3d9_session_release(session,device_ref));CHECK(pw_d3d9_session_release(session,factory));CHECK(pw_d3d9_session_close(session));session=NULL;
  if(refs!=1||failures)return 5;
  printf("PW_UP_TRANSPORT cycle=%u status=0 pixels=2 failed_commit_consumed=1 abandoned_clean=1\n",cycle);fflush(stdout);
 }
 return 0;
}
