/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_service_query.h"
#include "pw_d3d9_native_query.h"
#include "pw_d3d9_session.h"
#include "pw_d3d9_kinds.h"
#include <string.h>
struct query_owner {struct pw_d3d9_native_query *native;struct pw_d3d9_object_ref parent;};
/* Consumes the native context on every path. */
static HRESULT publish_query(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,struct pw_d3d9_native_query *native,struct pw_d3d9_object_ref *out)
{
 IUnknown *identity=pw_d3d9_native_query_identity(native);struct pw_d3d9_object_ref ref;HRESULT hr=E_FAIL;struct query_owner *owner=NULL;
 if(!identity)goto done;
 if(pw_d3d9_object_find(objects,(uintptr_t)identity,&ref)){
  const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);
  const struct query_owner *existing=slot?(const void *)slot->context:NULL;
  if(slot && slot->kind==PW_D3D9_KIND_QUERY && existing && existing->parent.id==parent.id && existing->parent.generation==parent.generation && pw_d3d9_object_addref(objects,ref,1)){*out=ref;hr=S_OK;}
  goto done;
 }
 owner=HeapAlloc(GetProcessHeap(),0,sizeof(*owner));hr=E_OUTOFMEMORY;if(!owner)goto done;
 *owner=(struct query_owner){native,parent};
 if(!pw_d3d9_object_queue(objects,parent)){hr=E_FAIL;goto done;}
 if(pw_d3d9_object_reserve(objects,&ref)){
  if(pw_d3d9_object_commit(objects,ref,(uintptr_t)identity,(uintptr_t)owner,PW_D3D9_KIND_QUERY)){*out=ref;return S_OK;}
  pw_d3d9_object_abort(objects,ref);hr=E_FAIL;
 }
 if(!pw_d3d9_object_complete(objects,parent)){pw_d3d9_objects_cancel(objects);hr=E_FAIL;}
 done:if(owner)HeapFree(GetProcessHeap(),0,owner);pw_d3d9_native_query_destroy(native);return hr;
}
void pw_d3d9_service_query_call(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,const struct pw_d3d9_query_request *q,struct pw_d3d9_query_reply *r)
{
 unsigned char wire[PW_D3D9_QUERY_WIRE_MAX];size_t bytes;struct pw_d3d9_native_query *created=NULL;
 memset(r,0,sizeof(*r));r->method=q->method;r->hresult=D3DERR_INVALIDCALL;
 if(pw_d3d9_query_request_encode(wire,sizeof(wire),&bytes,q))return;
 if(q->method==PW_D3D9_QUERY_DATA && q->has_data){r->count=q->size;memcpy(r->data,q->data,r->count);}
 unsigned kind=q->method==PW_D3D9_QUERY_CREATE?PW_D3D9_KIND_DEVICE:PW_D3D9_KIND_QUERY;
 const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);
 if(!slot || slot->kind!=kind || !pw_d3d9_object_queue(objects,ref))return;
 if(kind==PW_D3D9_KIND_DEVICE){
  pw_d3d9_native_query_create(pw_d3d9_native_device_backend((void *)slot->context),q,r,&created);
  if(SUCCEEDED((HRESULT)r->hresult) && q->want_object){
   if(!created)r->hresult=E_FAIL;
   else {HRESULT hr=publish_query(objects,ref,created,&r->object);created=NULL;if(FAILED(hr))r->hresult=hr;}
  }
 }else pw_d3d9_native_query_call(((struct query_owner *)slot->context)->native,q,r);
 pw_d3d9_native_query_destroy(created);
 if(!pw_d3d9_object_complete(objects,ref)){pw_d3d9_objects_cancel(objects);r->hresult=E_FAIL;}
 if(q->method==PW_D3D9_QUERY_CREATE && FAILED((HRESULT)r->hresult)){r->object=(struct pw_d3d9_object_ref){0};r->type=0;r->size=0;}
}
HRESULT pw_d3d9_service_query_destroy(struct pw_d3d9_objects *objects,uintptr_t context)
{
 struct query_owner *owner=(void *)context;pw_d3d9_native_query_destroy(owner->native);
 HRESULT hr=pw_d3d9_object_complete(objects,owner->parent)?S_OK:E_FAIL;HeapFree(GetProcessHeap(),0,owner);return hr;
}
