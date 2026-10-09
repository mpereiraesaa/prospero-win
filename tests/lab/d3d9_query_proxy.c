/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_query_proxy.h"
static IDirect3DDevice9 parent;static LONG parent_refs=1;static unsigned remote_refs,calls,failures;static int blocked,defer_failed,pending_result=1;static struct pw_d3d9_deferred *pending;
static ULONG WINAPI parent_addref(IDirect3DDevice9 *p){assert(p==&parent);return InterlockedIncrement(&parent_refs);}
static ULONG WINAPI parent_release(IDirect3DDevice9 *p){assert(p==&parent);return InterlockedDecrement(&parent_refs);}
static HRESULT exchange(IDirect3DDevice9 *p,struct pw_d3d9_object_ref ref,const struct pw_d3d9_query_request *request,struct pw_d3d9_query_reply *r)
{
 unsigned char bytes[PW_D3D9_QUERY_WIRE_MAX];size_t n;struct pw_d3d9_query_request q;struct pw_d3d9_query_reply decoded;
 assert(p==&parent);assert(!pw_d3d9_query_request_encode(bytes,sizeof(bytes),&n,request));assert(!pw_d3d9_query_request_decode(&q,bytes,n));calls++;memset(r,0,sizeof(*r));r->method=q.method;
 if(q.method==PW_D3D9_QUERY_CREATE){assert(!ref.id && !ref.generation);if(q.type!=D3DQUERYTYPE_EVENT)r->hresult=D3DERR_NOTAVAILABLE;else if(q.want_object){remote_refs++;r->object=(struct pw_d3d9_object_ref){7,9};r->type=q.type;r->size=4;}}
 else {assert(ref.id==7 && ref.generation==9 && remote_refs);
  if(q.method==PW_D3D9_QUERY_ISSUE)assert(q.flags==0xfedcba98u);
  else {assert(q.method==PW_D3D9_QUERY_DATA && q.flags==D3DGETDATA_FLUSH);r->count=q.has_data?q.size:0;memcpy(r->data,q.data,r->count);r->hresult=pending_result?S_FALSE:S_OK;if(q.has_data && q.size)r->data[0]=pending_result?0:1;}
 }
 assert(!pw_d3d9_query_reply_encode(bytes,sizeof(bytes),&n,&q,r));assert(!pw_d3d9_query_reply_decode(&decoded,&q,bytes,n));*r=decoded;return r->hresult;
}
static HRESULT remote_release(IDirect3DDevice9 *p,struct pw_d3d9_object_ref ref)
{assert(p==&parent && ref.id==7 && ref.generation==9 && remote_refs);if(blocked)return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;remote_refs--;return S_OK;}
static HRESULT defer(IDirect3DDevice9 *p,struct pw_d3d9_deferred *node)
{assert(p==&parent && !pending);pending=node;return defer_failed?E_OUTOFMEMORY:S_OK;}
static void fail(IDirect3DDevice9 *p,HRESULT hr){assert(p==&parent && FAILED(hr));failures++;}
int main(void)
{
 IDirect3DDevice9Vtbl table={0};struct pw_d3d9_query_proxy_ops ops={exchange,remote_release,defer,fail};IDirect3DQuery9 *q;IUnknown *u;IDirect3DDevice9 *owner;unsigned char bytes[8];unsigned before;
 table.AddRef=parent_addref;table.Release=parent_release;parent.lpVtbl=&table;pw_d3d9_query_proxy_install(&table,&ops);assert(!table.Present);
 assert(IDirect3DDevice9_CreateQuery(&parent,D3DQUERYTYPE_EVENT,NULL)==S_OK && !remote_refs && parent_refs==1);
 assert(IDirect3DDevice9_CreateQuery(&parent,D3DQUERYTYPE_RESOURCEMANAGER,&q)==D3DERR_NOTAVAILABLE && !q && parent_refs==1);
 assert(IDirect3DDevice9_CreateQuery(&parent,D3DQUERYTYPE_EVENT,&q)==S_OK && q && remote_refs==1 && parent_refs==2);
 assert(IDirect3DQuery9_GetType(q)==D3DQUERYTYPE_EVENT && IDirect3DQuery9_GetDataSize(q)==4);
 assert(IDirect3DQuery9_QueryInterface(q,&IID_IUnknown,(void **)&u)==S_OK && u==(IUnknown *)q);IUnknown_Release(u);
 assert(IDirect3DQuery9_QueryInterface(q,&IID_IDirect3DQuery9,(void **)&u)==S_OK && u==(IUnknown *)q);IUnknown_Release(u);
 assert(IDirect3DQuery9_QueryInterface(q,&IID_IDirect3DTexture9,(void **)&u)==E_NOINTERFACE && !u);
 assert(IDirect3DQuery9_GetDevice(q,&owner)==S_OK && owner==&parent);IDirect3DDevice9_Release(owner);
 assert(IDirect3DQuery9_Issue(q,0xfedcba98u)==S_OK);
 memset(bytes,0xa5,sizeof(bytes));assert(IDirect3DQuery9_GetData(q,bytes,4,D3DGETDATA_FLUSH)==S_FALSE && !bytes[0] && bytes[1]==0xa5 && bytes[4]==0xa5);
 assert(IDirect3DQuery9_GetData(q,NULL,0,D3DGETDATA_FLUSH)==S_FALSE);
 assert(IDirect3DQuery9_GetData(q,bytes,0,D3DGETDATA_FLUSH)==S_FALSE);
 pending_result=0;assert(IDirect3DQuery9_GetData(q,bytes,4,D3DGETDATA_FLUSH)==S_OK && bytes[0]==1 && bytes[1]==0xa5);
 before=calls;assert(IDirect3DQuery9_GetData(q,bytes,8,0)==D3DERR_INVALIDCALL && calls==before);
 blocked=1;assert(IDirect3DQuery9_Release(q)==0 && pending && parent_refs==2 && remote_refs==1);
 {struct pw_d3d9_deferred *node=pending;pending=NULL;node->function(node->context);}assert(pending && parent_refs==2 && remote_refs==1);
 {struct pw_d3d9_deferred *node=pending;pending=NULL;blocked=0;node->function(node->context);}assert(!remote_refs && parent_refs==1);
 assert(IDirect3DDevice9_CreateQuery(&parent,D3DQUERYTYPE_EVENT,&q)==S_OK);blocked=1;defer_failed=1;assert(!IDirect3DQuery9_Release(q) && pending && failures==1 && parent_refs==2 && remote_refs==1);
 /* Harness retains failed node solely to recover the deliberately retained shell. */
 {struct pw_d3d9_deferred *node=pending;pending=NULL;blocked=0;node->function(node->context);}assert(!remote_refs && parent_refs==1);
 puts("PW_QUERY_PROXY PASS");return 0;
}
