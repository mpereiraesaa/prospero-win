/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include "../../wine/ps5/d3d9/pw_d3d9_cursor.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {HRESULT actual=(x);if(FAILED(actual)){fprintf(stderr,"FAIL line=%d hr=%08lx\n",__LINE__,actual);return 1;}}while(0)
static struct pw_d3d9_session *session;static struct pw_d3d9_object_ref device_ref,surface_ref;static ULONG refs=1;static unsigned failures;static IDirect3DSurface9 local_surface;
static ULONG WINAPI addref(IDirect3DDevice9 *d){(void)d;return ++refs;}
static ULONG WINAPI release(IDirect3DDevice9 *d){(void)d;return --refs;}
static HRESULT call(IDirect3DDevice9 *d,const struct pw_d3d9_cursor_request *q,struct pw_d3d9_cursor_reply *r){(void)d;return pw_d3d9_session_cursor(session,device_ref,q,r);}
static HRESULT resolve(IDirect3DDevice9 *d,IUnknown *surface,uint32_t kind,struct pw_d3d9_object_ref *out){(void)d;if(surface!=(IUnknown *)&local_surface||kind!=6)return D3DERR_INVALIDCALL;*out=surface_ref;return S_OK;}
static void fail(IDirect3DDevice9 *d,HRESULT hr){(void)d;(void)hr;failures++;pw_d3d9_session_cancel(session);}
int wmain(int argc,WCHAR **argv)
{
 struct pw_d3d9_object_ref factory,other;struct pw_d3d9_device_reply dr;struct pw_d3d9_texture_request tq;struct pw_d3d9_texture_reply tr;struct pw_d3d9_cursor_request q;struct pw_d3d9_cursor_reply r;
 IDirect3DDevice9Vtbl table={.AddRef=addref,.Release=release};IDirect3DDevice9 guest={&table};struct pw_d3d9_cursor_ops ops={call,resolve,fail};POINT old_position;
 if(argc!=3)return 2;
 pw_d3d9_cursor_install(&table,&ops);GetCursorPos(&old_position);
 for(unsigned cycle=0;cycle<3;cycle++){
  CHECK(pw_d3d9_session_open(argv[1],argv[2],&session));CHECK(pw_d3d9_session_create(session,D3D_SDK_VERSION,&factory));
  struct pw_d3d9_device_request dq={.operation=1,.device_type=D3DDEVTYPE_HAL,.behavior_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING,.focus_window={1,1,1},.parameters={.width=64,.height=64,.format=D3DFMT_A8R8G8B8,.count=1,.swap_effect=D3DSWAPEFFECT_DISCARD,.windowed=1,.interval=D3DPRESENT_INTERVAL_IMMEDIATE,.window={1,1,1}}};
  CHECK(pw_d3d9_session_device(session,factory,&dq,&dr));device_ref=dr.object;CHECK(pw_d3d9_session_device(session,factory,&dq,&dr));other=dr.object;
  tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_CREATE_SURFACE,.width=32,.height=32,.format=D3DFMT_A8R8G8B8,.pool=D3DPOOL_SYSTEMMEM};CHECK(pw_d3d9_session_texture(session,device_ref,&tq,&tr));surface_ref=tr.object;
  tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_LOCK};CHECK(pw_d3d9_session_texture(session,surface_ref,&tq,&tr));uint64_t generation=tr.lock_generation;if(tr.length!=4096)return 3;
  tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_WRITE,.lock_generation=generation,.count=4096};memset(tq.data,0xff,4096);CHECK(pw_d3d9_session_texture(session,surface_ref,&tq,&tr));
  tq=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_UNLOCK,.lock_generation=generation};CHECK(pw_d3d9_session_texture(session,surface_ref,&tq,&tr));
  CHECK(table.SetCursorProperties(&guest,7,9,&local_surface));if(table.SetCursorProperties(&guest,0,0,NULL)!=D3DERR_INVALIDCALL)return 4;
  table.SetCursorPosition(&guest,-17,-9,D3DCURSOR_IMMEDIATE_UPDATE);table.ShowCursor(&guest,FALSE);
  if(table.ShowCursor(&guest,(BOOL)0xffffffff)!=FALSE||(uint32_t)table.ShowCursor(&guest,FALSE)!=0xffffffffu)return 5;
  q=(struct pw_d3d9_cursor_request){10,7,9,0,surface_ref.id,surface_ref.generation};
  if(pw_d3d9_session_cursor(session,other,&q,&r)!=D3DERR_INVALIDCALL)return 6;
  q.id=factory.id;q.generation=factory.generation;if(pw_d3d9_session_cursor(session,device_ref,&q,&r)!=D3DERR_INVALIDCALL)return 7;
  q.id=surface_ref.id;q.generation=surface_ref.generation+1;if(pw_d3d9_session_cursor(session,device_ref,&q,&r)!=D3DERR_INVALIDCALL)return 8;
  CHECK(pw_d3d9_session_release(session,surface_ref));q.generation=surface_ref.generation;if(pw_d3d9_session_cursor(session,device_ref,&q,&r)!=D3DERR_INVALIDCALL)return 9;
  table.ShowCursor(&guest,FALSE);if(refs!=1||failures)return 10;
  CHECK(pw_d3d9_session_release(session,other));CHECK(pw_d3d9_session_release(session,device_ref));CHECK(pw_d3d9_session_release(session,factory));CHECK(pw_d3d9_session_close(session));session=NULL;
  printf("PW_CURSOR_TRANSPORT cycle=%u status=0 exact_bool=1 negatives=4\n",cycle);fflush(stdout);
 }
 SetCursorPos(old_position.x,old_position.y);return 0;
}
