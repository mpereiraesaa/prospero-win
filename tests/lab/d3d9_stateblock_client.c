/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_stateblock_client.h"
#include <assert.h>
#include <stdio.h>
static IDirect3DDevice9 parent;static LONG parent_refs=1,remote_refs,calls,releases,failures;
static int blocked,reject_defer,bad_reply,backend_fail;static struct pw_d3d9_deferred *pending;
static ULONG WINAPI parent_addref(IDirect3DDevice9 *d){assert(d==&parent);return InterlockedIncrement(&parent_refs);}
static ULONG WINAPI parent_release(IDirect3DDevice9 *d){assert(d==&parent);return InterlockedDecrement(&parent_refs);}
static HRESULT call(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref,const struct pw_d3d9_stateblock_request *q,struct pw_d3d9_stateblock_reply *r)
{
 assert(d==&parent && !pw_d3d9_stateblock_validate(q));InterlockedIncrement(&calls);
 r->hresult=backend_fail?(uint32_t)D3DERR_INVALIDCALL:S_OK;r->object=(struct pw_d3d9_object_ref){0};
 if(backend_fail)return D3DERR_INVALIDCALL;
 if(q->method>=59){assert(!ref.id && !ref.generation);if(q->method!=60){InterlockedIncrement(&remote_refs);r->object=(struct pw_d3d9_object_ref){7,9};}}
 else assert(ref.id==7 && ref.generation==9);
 if(bad_reply)r->hresult=1;
 return S_OK;
}
static HRESULT remote_release(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref)
{
 assert(d==&parent && ref.id==7 && ref.generation==9);InterlockedIncrement(&releases);
 if(blocked)return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
 assert(InterlockedDecrement(&remote_refs)>=0);return S_OK;
}
static HRESULT defer(IDirect3DDevice9 *d,struct pw_d3d9_deferred *item)
{assert(d==&parent && !pending && item->function && item->context);pending=item;return reject_defer?E_FAIL:S_OK;}
static void fail(IDirect3DDevice9 *d,HRESULT hr)
{assert(d==&parent && FAILED(hr));InterlockedIncrement(&failures);InterlockedExchange(&remote_refs,0);}
static DWORD WINAPI thread(void *unused)
{
 (void)unused;for(unsigned i=0;i<128;i++){IDirect3DStateBlock9 *b;assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&b)==S_OK);assert(IDirect3DStateBlock9_Capture(b)==S_OK);IDirect3DStateBlock9_Release(b);}return 0;
}
int main(void)
{
 IDirect3DDevice9Vtbl table={0};struct pw_d3d9_stateblock_client_ops ops={call,remote_release,defer,fail};IDirect3DStateBlock9 *a,*b;IDirect3DDevice9 *d;IUnknown *unknown;
 table.AddRef=parent_addref;table.Release=parent_release;parent.lpVtbl=&table;pw_d3d9_stateblock_client_install(&table,&ops);
 assert(!table.Present && !table.GetRenderState);
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,NULL)==D3DERR_INVALIDCALL && !calls);
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==S_OK && parent_refs==2 && remote_refs==1);
 assert(IDirect3DDevice9_EndStateBlock(&parent,&b)==S_OK && a==b && parent_refs==2 && remote_refs==1);
 assert(IDirect3DStateBlock9_QueryInterface(a,&IID_IUnknown,(void **)&unknown)==S_OK && unknown==(IUnknown *)a);IUnknown_Release(unknown);
 assert(IDirect3DStateBlock9_QueryInterface(a,&IID_IDirect3DStateBlock9,(void **)&unknown)==S_OK && unknown==(IUnknown *)a);IUnknown_Release(unknown);
 assert(IDirect3DStateBlock9_QueryInterface(a,&IID_IDirect3DDevice9,(void **)&unknown)==E_NOINTERFACE && !unknown);
 assert(IDirect3DStateBlock9_GetDevice(a,&d)==S_OK && d==&parent && parent_refs==3);IDirect3DDevice9_Release(d);
 assert(IDirect3DStateBlock9_GetDevice(a,NULL)==D3DERR_INVALIDCALL);
 assert(IDirect3DDevice9_BeginStateBlock(&parent)==S_OK);assert(IDirect3DStateBlock9_Capture(a)==S_OK);assert(IDirect3DStateBlock9_Apply(a)==S_OK);
 assert(IDirect3DStateBlock9_Release(a)==1);assert(IDirect3DStateBlock9_Release(b)==0 && remote_refs==0 && parent_refs==1);
 {HANDLE handles[4];for(unsigned i=0;i<4;i++){handles[i]=CreateThread(NULL,0,thread,NULL,0,NULL);assert(handles[i]);}assert(WaitForMultipleObjects(4,handles,TRUE,INFINITE)==WAIT_OBJECT_0);for(unsigned i=0;i<4;i++)CloseHandle(handles[i]);assert(remote_refs==0 && parent_refs==1);}
 backend_fail=1;a=(void *)(uintptr_t)1;assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==D3DERR_INVALIDCALL && !a && parent_refs==1);backend_fail=0;
 bad_reply=1;assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==E_FAIL && !a && failures==1 && parent_refs==1);bad_reply=0;
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==S_OK);blocked=1;
 assert(IDirect3DStateBlock9_Release(a)==0 && pending && parent_refs==2 && remote_refs==1);
 {struct pw_d3d9_deferred *item=pending;pending=NULL;blocked=0;item->function(item->context);}assert(parent_refs==1 && !remote_refs);
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==S_OK);blocked=reject_defer=1;
 assert(IDirect3DStateBlock9_Release(a)==0 && pending && failures==2 && parent_refs==2 && !remote_refs);
 /* Failed enqueue intentionally retains the parent/shell; no unsafe reentrant join. */
 printf("PASS stateblock proxy: canonical identity, strong parent, 512 threaded cycles, deferred cleanup, calls=%ld\n",calls);return 0;
}
