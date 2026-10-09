/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include "../../wine/ps5/d3d9/pw_d3d9_query_proxy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct pw_d3d9_session *session;static struct pw_d3d9_object_ref device;static IDirect3DDevice9 parent;static LONG refs=1;
static ULONG WINAPI addref(IDirect3DDevice9 *p){assert(p==&parent);return InterlockedIncrement(&refs);}
static ULONG WINAPI release(IDirect3DDevice9 *p){assert(p==&parent);return InterlockedDecrement(&refs);}
static HRESULT call(IDirect3DDevice9 *p,struct pw_d3d9_object_ref target,const struct pw_d3d9_query_request *q,struct pw_d3d9_query_reply *r)
{assert(p==&parent);if(!target.id)target=device;return pw_d3d9_session_query(session,target,q,r);}
static HRESULT retire(IDirect3DDevice9 *p,struct pw_d3d9_object_ref target){assert(p==&parent);return pw_d3d9_session_release(session,target);}
static HRESULT defer(IDirect3DDevice9 *p,struct pw_d3d9_deferred *node){(void)p;(void)node;assert(0);return E_FAIL;}
static void fail(IDirect3DDevice9 *p,HRESULT hr){(void)p;fprintf(stderr,"query transport failure %08lx\n",(unsigned long)hr);assert(0);}
int wmain(int argc,WCHAR **argv)
{
 if(argc!=3)return 2;
 IDirect3DDevice9Vtbl table={0};struct pw_d3d9_query_proxy_ops ops={call,retire,defer,fail};table.AddRef=addref;table.Release=release;parent.lpVtbl=&table;pw_d3d9_query_proxy_install(&table,&ops);
 for(unsigned cycle=0;cycle<3;cycle++){
  struct pw_d3d9_object_ref factory;struct pw_d3d9_device_reply reply;IDirect3DQuery9 *query;DWORD output;
  assert(pw_d3d9_session_open(argv[1],argv[2],&session)==S_OK);assert(pw_d3d9_session_create(session,D3D_SDK_VERSION,&factory)==S_OK);
  struct pw_d3d9_device_request q={.operation=1,.device_type=D3DDEVTYPE_HAL,.behavior_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING,.focus_window={1,1,1},.parameters={.width=64,.height=64,.format=D3DFMT_A8R8G8B8,.count=1,.swap_effect=D3DSWAPEFFECT_DISCARD,.windowed=1,.interval=D3DPRESENT_INTERVAL_IMMEDIATE,.window={1,1,1}}};
  assert(pw_d3d9_session_device(session,factory,&q,&reply)==S_OK);device=reply.object;
  assert(IDirect3DDevice9_CreateQuery(&parent,D3DQUERYTYPE_EVENT,NULL)==S_OK);
  assert(IDirect3DDevice9_CreateQuery(&parent,D3DQUERYTYPE_OCCLUSION,&query)==S_OK);assert(IDirect3DQuery9_GetDataSize(query)==4&&IDirect3DQuery9_GetType(query)==D3DQUERYTYPE_OCCLUSION);
  assert(IDirect3DQuery9_Issue(query,D3DISSUE_BEGIN)==S_OK);output=0xa5a5a5a5;assert(IDirect3DQuery9_GetData(query,&output,4,D3DGETDATA_FLUSH)==S_FALSE&&output==0xa5a5a5a5);assert(IDirect3DQuery9_GetData(query,NULL,0,0)==S_FALSE);
  assert(IDirect3DQuery9_Issue(query,D3DISSUE_END)==S_OK);
  for(unsigned i=0;;i++){HRESULT hr=IDirect3DQuery9_GetData(query,NULL,0,D3DGETDATA_FLUSH);if(hr!=S_FALSE){assert(hr==S_OK);break;}assert(i<10000);Sleep(1);}
  assert(IDirect3DQuery9_GetData(query,&output,4,0)==S_OK);IDirect3DQuery9_Release(query);
  assert(IDirect3DDevice9_CreateQuery(&parent,D3DQUERYTYPE_EVENT,&query)==S_OK);assert(IDirect3DQuery9_Issue(query,D3DISSUE_END)==S_OK);
  for(unsigned i=0;;i++){HRESULT hr=IDirect3DQuery9_GetData(query,NULL,0,D3DGETDATA_FLUSH);if(hr!=S_FALSE){assert(hr==S_OK);break;}assert(i<10000);Sleep(1);}
  output=0;assert(IDirect3DQuery9_GetData(query,&output,4,0)==S_OK&&output==1);output=0xa5a5a5a5;assert(IDirect3DQuery9_GetData(query,&output,4,0)==S_OK&&output==0xa5a5a501);
  IDirect3DQuery9_Release(query);assert(refs==1);
  /* Leave a raw owned query in the service registry to prove STOP cleanup. */
  struct pw_d3d9_query_request query_request={.method=PW_D3D9_QUERY_CREATE,.type=D3DQUERYTYPE_EVENT,.want_object=1};struct pw_d3d9_query_reply query_reply;
  assert(pw_d3d9_session_query(session,device,&query_request,&query_reply)==S_OK&&query_reply.object.id);
  assert(pw_d3d9_session_release(session,device)==S_OK);assert(pw_d3d9_session_release(session,factory)==S_OK);assert(pw_d3d9_session_close(session)==S_OK);session=NULL;
  printf("PW_QUERY_TRANSPORT cycle=%u pending=1 partial=1 cleanup=1 status=0\n",cycle);fflush(stdout);
 }
 return 0;
}
