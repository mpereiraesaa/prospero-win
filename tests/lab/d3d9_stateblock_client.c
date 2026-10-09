/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_stateblock_client.h"
#include <assert.h>
#include <stdio.h>
static IDirect3DDevice9 parent;static LONG parent_refs=1,remote_refs,calls,releases,failures;
static int blocked,reject_defer,bad_reply,backend_fail;
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
static int backend_false;
#endif
static struct pw_d3d9_deferred *pending;
static ULONG WINAPI parent_addref(IDirect3DDevice9 *d){assert(d==&parent);return InterlockedIncrement(&parent_refs);}
static ULONG WINAPI parent_release(IDirect3DDevice9 *d){assert(d==&parent);return InterlockedDecrement(&parent_refs);}
static HRESULT call(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref,const struct pw_d3d9_stateblock_request *q,struct pw_d3d9_stateblock_reply *r)
{
 assert(d==&parent && !pw_d3d9_stateblock_validate(q));InterlockedIncrement(&calls);
 r->hresult=backend_fail?(uint32_t)D3DERR_INVALIDCALL:S_OK;r->object=(struct pw_d3d9_object_ref){0};
 if(backend_fail)return D3DERR_INVALIDCALL;
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
 if(backend_false){r->hresult=S_FALSE;return S_FALSE;}
#endif
 if(q->method>=59){assert(!ref.id && !ref.generation);if(q->method!=60){InterlockedIncrement(&remote_refs);r->object=(struct pw_d3d9_object_ref){7,9};}}
 else assert(ref.id==7 && ref.generation==9);
 if(bad_reply)r->hresult=1;
 return S_OK;
}
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
static LONG observed_calls,evidence_updates,invalidations;
static SRWLOCK evidence_gate=SRWLOCK_INIT;
static DWORD evidence_owner;
static int fail_after_commit,alias_race;
static HANDLE committed_event,publish_event;
static DWORD held_thread;
static uintptr_t held_address;
static void invalidate_evidence(struct pw_d3d9_stateblock_evidence *);
static DWORD main_thread;
static uintptr_t evidence_address;
static struct pw_d3d9_draw_shadow device_evidence;
static void fail(IDirect3DDevice9 *,HRESULT);
static HRESULT observed_call_locked(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_stateblock_request *q,struct pw_d3d9_stateblock_reply *r,
 struct pw_d3d9_stateblock_evidence *evidence)
{
 unsigned char wire[16];HRESULT hr;
 assert((q->method==PW_D3D9_SB_BEGIN)==(evidence==NULL));
 if(q->method==PW_D3D9_SB_CREATE || q->method==PW_D3D9_SB_END)
  assert(!evidence->draw.captures_decl && evidence->draw.declaration==PW_D3D9_DECL_UNKNOWN);
 InterlockedIncrement(&observed_calls);hr=call(d,ref,q,r);
 /* Model the future admitted observer: malformed or failed replies never
  * publish evidence. Real session code must hold its gate around this block. */
 if(hr!=S_OK){if(SUCCEEDED(hr)){fail(d,E_FAIL);return E_FAIL;}return hr;}
 if(r->hresult!=(uint32_t)hr || pw_d3d9_stateblock_reply_encode(wire,sizeof(wire),q,r)){
  fail(d,E_FAIL);return E_FAIL;
 }
 if(GetCurrentThreadId()!=main_thread){
  if(q->method==PW_D3D9_SB_CREATE || q->method==PW_D3D9_SB_END){
   evidence->draw.captures_decl=1;evidence->draw.declaration=PW_D3D9_DECL_PRESENT;
   if(GetCurrentThreadId()==held_thread)held_address=(uintptr_t)evidence;
   return pw_d3d9_stateblock_client_commit(evidence,r->object,invalidate_evidence);
  }
  return hr;
 }
 switch(q->method){
 case PW_D3D9_SB_CREATE:
  pw_d3d9_draw_create_block(&device_evidence,&evidence->draw,q->type,hr);
  if(!alias_race)evidence_address=(uintptr_t)evidence;
  break;
 case PW_D3D9_SB_BEGIN:pw_d3d9_draw_begin(&device_evidence,hr);break;
 case PW_D3D9_SB_END:pw_d3d9_draw_end(&device_evidence,&evidence->draw,hr);break;
 case PW_D3D9_SB_CAPTURE:
  assert((uintptr_t)evidence==evidence_address);
  pw_d3d9_draw_capture(&device_evidence,&evidence->draw,hr);break;
 case PW_D3D9_SB_APPLY:
  assert((uintptr_t)evidence==evidence_address);
  pw_d3d9_draw_apply(&device_evidence,&evidence->draw,hr);break;
 }
 InterlockedIncrement(&evidence_updates);
 if(q->method==PW_D3D9_SB_CREATE || q->method==PW_D3D9_SB_END){
  hr=pw_d3d9_stateblock_client_commit(evidence,r->object,invalidate_evidence);
  if(FAILED(hr))return hr;
  if(fail_after_commit)return E_FAIL; /* Exercise proxy rollback after commit. */
 }
 return hr;
}
static HRESULT observed_call(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_stateblock_request *q,struct pw_d3d9_stateblock_reply *r,
 struct pw_d3d9_stateblock_evidence *evidence)
{
 AcquireSRWLockExclusive(&evidence_gate);evidence_owner=GetCurrentThreadId();
 HRESULT hr=observed_call_locked(d,ref,q,r,evidence);
 evidence_owner=0;ReleaseSRWLockExclusive(&evidence_gate);
 if(GetCurrentThreadId()==held_thread && q->method==PW_D3D9_SB_CREATE){
  SetEvent(committed_event);assert(WaitForSingleObject(publish_event,10000)==WAIT_OBJECT_0);
 }
 return hr;
}
static void invalidate_evidence(struct pw_d3d9_stateblock_evidence *evidence)
{
 assert(evidence && evidence_owner==GetCurrentThreadId());
 evidence->draw.captures_decl=1;evidence->draw.declaration=PW_D3D9_DECL_UNKNOWN;
 InterlockedIncrement(&invalidations);
}
#endif
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
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
static DWORD WINAPI held_create(void *unused)
{
 IDirect3DStateBlock9 *block;(void)unused;held_thread=GetCurrentThreadId();
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&block)==S_OK);
 IDirect3DStateBlock9_Release(block);return 0;
}
#endif
static DWORD WINAPI thread(void *unused)
{
 (void)unused;for(unsigned i=0;i<128;i++){IDirect3DStateBlock9 *b;assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&b)==S_OK);assert(IDirect3DStateBlock9_Capture(b)==S_OK);IDirect3DStateBlock9_Release(b);}return 0;
}
int main(void)
{
 IDirect3DDevice9Vtbl table={0};struct pw_d3d9_stateblock_client_ops ops={call,remote_release,defer,fail
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
 ,observed_call
#endif
 };IDirect3DStateBlock9 *a,*b;IDirect3DDevice9 *d;IUnknown *unknown;
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
 main_thread=GetCurrentThreadId();pw_d3d9_draw_init(&device_evidence);
 pw_d3d9_draw_observe(&device_evidence,1,S_OK);
#endif
 table.AddRef=parent_addref;table.Release=parent_release;parent.lpVtbl=&table;pw_d3d9_stateblock_client_install(&table,&ops);
 assert(!table.Present && !table.GetRenderState);
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,NULL)==D3DERR_INVALIDCALL && !calls);
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==S_OK && parent_refs==2 && remote_refs==1);
 assert(IDirect3DDevice9_EndStateBlock(&parent,&b)==S_OK && a==b && parent_refs==2 && remote_refs==1);
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
 assert(invalidations==1); /* Duplicate End result used the original pinned owner. */
#endif
 assert(IDirect3DStateBlock9_QueryInterface(a,&IID_IUnknown,(void **)&unknown)==S_OK && unknown==(IUnknown *)a);IUnknown_Release(unknown);
 assert(IDirect3DStateBlock9_QueryInterface(a,&IID_IDirect3DStateBlock9,(void **)&unknown)==S_OK && unknown==(IUnknown *)a);IUnknown_Release(unknown);
 assert(IDirect3DStateBlock9_QueryInterface(a,&IID_IDirect3DDevice9,(void **)&unknown)==E_NOINTERFACE && !unknown);
 assert(IDirect3DStateBlock9_GetDevice(a,&d)==S_OK && d==&parent && parent_refs==3);IDirect3DDevice9_Release(d);
 assert(IDirect3DStateBlock9_GetDevice(a,NULL)==D3DERR_INVALIDCALL);
 assert(IDirect3DDevice9_BeginStateBlock(&parent)==S_OK);assert(IDirect3DStateBlock9_Capture(a)==S_OK);assert(IDirect3DStateBlock9_Apply(a)==S_OK);
 assert(IDirect3DStateBlock9_Release(a)==1);assert(IDirect3DStateBlock9_Release(b)==0 && remote_refs==0 && parent_refs==1);
 {HANDLE handles[4];for(unsigned i=0;i<4;i++){handles[i]=CreateThread(NULL,0,thread,NULL,0,NULL);assert(handles[i]);}assert(WaitForMultipleObjects(4,handles,TRUE,INFINITE)==WAIT_OBJECT_0);for(unsigned i=0;i<4;i++)CloseHandle(handles[i]);assert(remote_refs==0 && parent_refs==1);}
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
 /* Pause the first creator after original-gate commit but before its public
  * method returns. A second creator must see/invalidate that unpublished alias. */
 committed_event=CreateEventW(NULL,TRUE,FALSE,NULL);publish_event=CreateEventW(NULL,TRUE,FALSE,NULL);
 assert(committed_event && publish_event);alias_race=1;
 HANDLE held=CreateThread(NULL,0,held_create,NULL,0,NULL);assert(held);
 assert(WaitForSingleObject(committed_event,10000)==WAIT_OBJECT_0);evidence_address=held_address;
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==S_OK);
 assert(IDirect3DStateBlock9_Apply(a)==S_OK && device_evidence.active==PW_D3D9_DECL_UNKNOWN);
 IDirect3DStateBlock9_Release(a);SetEvent(publish_event);
 assert(WaitForSingleObject(held,10000)==WAIT_OBJECT_0);
 CloseHandle(held);CloseHandle(committed_event);CloseHandle(publish_event);held_thread=0;alias_race=0;
 assert(parent_refs==1 && remote_refs==0);
 /* Both newly committed and canonical-alias ownership roll back after a later
  * observer failure, without publishing an output or leaking a parent pin. */
 fail_after_commit=1;
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==E_FAIL && !a && parent_refs==1 && remote_refs==0);
 fail_after_commit=0;assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==S_OK);
 fail_after_commit=1;
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&b)==E_FAIL && !b && parent_refs==2 && remote_refs==1);
 fail_after_commit=0;IDirect3DStateBlock9_Release(a);assert(parent_refs==1 && remote_refs==0);
#endif
 backend_fail=1;a=(void *)(uintptr_t)1;assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==D3DERR_INVALIDCALL && !a && parent_refs==1);backend_fail=0;
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
 LONG before_updates=evidence_updates;
#endif
 bad_reply=1;assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==E_FAIL && !a && failures==1 && parent_refs==1);bad_reply=0;
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
 assert(evidence_updates==before_updates);
 assert(IDirect3DDevice9_BeginStateBlock(&parent)==S_OK);
 pw_d3d9_draw_declaration(&device_evidence,1,S_OK);
 struct pw_d3d9_draw_shadow before=device_evidence;
 backend_fail=1;before_updates=evidence_updates;
 assert(IDirect3DDevice9_EndStateBlock(&parent,&a)==D3DERR_INVALIDCALL && !a);
 assert(!memcmp(&before,&device_evidence,sizeof(before)) && evidence_updates==before_updates);
 backend_fail=0;
 assert(IDirect3DDevice9_EndStateBlock(&parent,&a)==S_OK);
 assert(!device_evidence.recording);IDirect3DStateBlock9_Release(a);
#endif
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==S_OK);blocked=1;
 assert(IDirect3DStateBlock9_Release(a)==0 && pending && parent_refs==2 && remote_refs==1);
 {struct pw_d3d9_deferred *item=pending;pending=NULL;blocked=0;item->function(item->context);}assert(parent_refs==1 && !remote_refs);
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==S_OK);blocked=reject_defer=1;
 assert(IDirect3DStateBlock9_Release(a)==0 && pending && failures==2 && parent_refs==2 && !remote_refs);
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
 assert(observed_calls==calls);
 LONG previous_failures=failures;LONG previous_parent=parent_refs;
 backend_false=1;before_updates=evidence_updates;
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==E_FAIL && !a);
 assert(evidence_updates==before_updates && failures==previous_failures+1);
 backend_false=0;previous_failures=failures;
 ops.observed_call=NULL;pw_d3d9_stateblock_client_install(&table,&ops);
 assert(IDirect3DDevice9_CreateStateBlock(&parent,D3DSBT_ALL,&a)==E_FAIL && !a);
 assert(failures==previous_failures+1 && parent_refs==previous_parent);
#endif
 /* Failed enqueue intentionally retains the parent/shell; no unsafe reentrant join. */
 printf("PASS stateblock proxy: canonical identity, strong parent, 512 threaded cycles, deferred cleanup, calls=%ld\n",calls);return 0;
}
