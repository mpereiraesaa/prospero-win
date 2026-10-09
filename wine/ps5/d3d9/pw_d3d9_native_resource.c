/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_resource.h"
#include <d3d9.h>
#include <string.h>
struct pw_d3d9_native_resource {
 union { IDirect3DVertexBuffer9 *vb; IDirect3DIndexBuffer9 *ib; IUnknown *unknown; } object;
 IDirect3DDevice9 *device;
 unsigned char *mapping;
 uint64_t generation;
 uint32_t kind,span,written,readonly;
};
static HRESULT describe(struct pw_d3d9_native_resource *r,struct pw_d3d9_buffer_desc *out)
{
 HRESULT hr;
 if(r->kind==PW_D3D9_KIND_VERTEX_BUFFER){
  D3DVERTEXBUFFER_DESC d;hr=IDirect3DVertexBuffer9_GetDesc(r->object.vb,&d);
  if(SUCCEEDED(hr))*out=(struct pw_d3d9_buffer_desc){d.Format,d.Type,d.Usage,d.Pool,d.Size,d.FVF};
 }else{
  D3DINDEXBUFFER_DESC d;hr=IDirect3DIndexBuffer9_GetDesc(r->object.ib,&d);
  if(SUCCEEDED(hr))*out=(struct pw_d3d9_buffer_desc){d.Format,d.Type,d.Usage,d.Pool,d.Size,0};
 }return hr;
}
static HRESULT unlock(struct pw_d3d9_native_resource *r)
{
 HRESULT hr=r->kind==PW_D3D9_KIND_VERTEX_BUFFER?IDirect3DVertexBuffer9_Unlock(r->object.vb):IDirect3DIndexBuffer9_Unlock(r->object.ib);
 /* The backend call has consumed the lock even when it reports a failure.
  * Never retain a mapping which may have been invalidated by that call. */
 r->mapping=NULL;r->span=r->written=r->readonly=0;return hr;
}
uint32_t pw_d3d9_native_resource_destroy(struct pw_d3d9_native_resource *r)
{
 HRESULT hr=S_OK;
 if(!r)return S_OK;
 if(r->mapping)hr=unlock(r);
 if(r->object.unknown)IUnknown_Release(r->object.unknown);
 if(r->device)IDirect3DDevice9_Release(r->device);
 HeapFree(GetProcessHeap(),0,r);return (uint32_t)hr;
}
uintptr_t pw_d3d9_native_resource_identity(struct pw_d3d9_native_resource *r)
{
 IUnknown *identity=NULL;uintptr_t value;
 if(!r||FAILED(IUnknown_QueryInterface(r->object.unknown,&IID_IUnknown,(void **)&identity)))return 0;
 value=(uintptr_t)identity;IUnknown_Release(identity);return value;
}
void *pw_d3d9_native_resource_backend(struct pw_d3d9_native_resource *r)
{return r?r->object.unknown:NULL;}
uint32_t pw_d3d9_native_resource_kind(struct pw_d3d9_native_resource *r)
{return r?r->kind:0;}
uint32_t pw_d3d9_native_resource_adopt(void *device,uint32_t kind,void *owned,struct pw_d3d9_native_resource **out)
{
 struct pw_d3d9_native_resource *r;IUnknown *object=owned;HRESULT hr;
 if(out)*out=NULL;
 if(!out||!device||!object){if(object)IUnknown_Release(object);return E_INVALIDARG;}
 if(kind!=PW_D3D9_KIND_VERTEX_BUFFER&&kind!=PW_D3D9_KIND_INDEX_BUFFER){IUnknown_Release(object);return E_NOTIMPL;}
 r=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*r));if(!r){IUnknown_Release(object);return E_OUTOFMEMORY;}
 r->kind=kind;
 hr=IUnknown_QueryInterface(object,kind==PW_D3D9_KIND_VERTEX_BUFFER?&IID_IDirect3DVertexBuffer9:&IID_IDirect3DIndexBuffer9,(void **)&r->object.unknown);
 IUnknown_Release(object);
 if(SUCCEEDED(hr)&&!r->object.unknown)hr=E_FAIL;
 if(SUCCEEDED(hr)){
  hr=kind==PW_D3D9_KIND_VERTEX_BUFFER?IDirect3DVertexBuffer9_GetDevice(r->object.vb,&r->device):IDirect3DIndexBuffer9_GetDevice(r->object.ib,&r->device);
  if(SUCCEEDED(hr)&&r->device!=device)hr=D3DERR_INVALIDCALL;
 }
 if(FAILED(hr))pw_d3d9_native_resource_destroy(r);else *out=r;
 return (uint32_t)hr;
}
void pw_d3d9_native_resource_create(void *native_device,const struct pw_d3d9_resource_request *q,
 struct pw_d3d9_resource_reply *reply,struct pw_d3d9_native_resource **out)
{
 struct pw_d3d9_native_resource *r;IDirect3DDevice9 *device=native_device;HRESULT hr;
 memset(reply,0,sizeof(*reply));reply->operation=q->operation;reply->hresult=D3DERR_INVALIDCALL;*out=NULL;
 if(!device)return;
 if(q->operation!=PW_D3D9_RESOURCE_CREATE_VB&&q->operation!=PW_D3D9_RESOURCE_CREATE_IB){reply->hresult=E_NOTIMPL;return;}
 r=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*r));if(!r){reply->hresult=E_OUTOFMEMORY;return;}
 r->kind=q->operation==PW_D3D9_RESOURCE_CREATE_VB?PW_D3D9_KIND_VERTEX_BUFFER:PW_D3D9_KIND_INDEX_BUFFER;
 if(r->kind==PW_D3D9_KIND_VERTEX_BUFFER)hr=IDirect3DDevice9_CreateVertexBuffer(device,q->length,q->usage,q->format_fvf,q->pool,&r->object.vb,NULL);
 else hr=IDirect3DDevice9_CreateIndexBuffer(device,q->length,q->usage,q->format_fvf,q->pool,&r->object.ib,NULL);
 if(SUCCEEDED(hr)&&!r->object.unknown)hr=E_FAIL;
 if(SUCCEEDED(hr)){
  r->device=device;IDirect3DDevice9_AddRef(device);
  hr=describe(r,&reply->desc);
 }
 if(FAILED(hr)){pw_d3d9_native_resource_destroy(r);memset(&reply->desc,0,sizeof(reply->desc));}
 else *out=r;
 reply->hresult=(uint32_t)hr;
}
void pw_d3d9_native_resource_call(struct pw_d3d9_native_resource *r,const struct pw_d3d9_resource_request *q,
 struct pw_d3d9_resource_reply *reply)
{
 HRESULT hr=D3DERR_INVALIDCALL;struct pw_d3d9_buffer_desc desc={0};uint32_t span;
 memset(reply,0,sizeof(*reply));reply->operation=q->operation;reply->hresult=(uint32_t)hr;
 if(!r)return;
 switch(q->operation){
 case PW_D3D9_RESOURCE_DESC:hr=describe(r,&reply->desc);break;
 case PW_D3D9_RESOURCE_GET_PRIORITY:
  reply->priority=r->kind==PW_D3D9_KIND_VERTEX_BUFFER?IDirect3DVertexBuffer9_GetPriority(r->object.vb):IDirect3DIndexBuffer9_GetPriority(r->object.ib);hr=S_OK;break;
 case PW_D3D9_RESOURCE_SET_PRIORITY:
  reply->priority=r->kind==PW_D3D9_KIND_VERTEX_BUFFER?IDirect3DVertexBuffer9_SetPriority(r->object.vb,q->priority):IDirect3DIndexBuffer9_SetPriority(r->object.ib,q->priority);hr=S_OK;break;
 case PW_D3D9_RESOURCE_PRELOAD:
  if(r->kind==PW_D3D9_KIND_VERTEX_BUFFER)IDirect3DVertexBuffer9_PreLoad(r->object.vb);else IDirect3DIndexBuffer9_PreLoad(r->object.ib);
  hr=S_OK;break;
 case PW_D3D9_RESOURCE_LOCK:
  if(r->mapping){hr=E_NOTIMPL;break;}
  if(r->generation==UINT64_MAX){hr=E_FAIL;break;}
  hr=describe(r,&desc);if(FAILED(hr))break;
  hr=D3DERR_INVALIDCALL;
  if(q->offset>=desc.size||q->length>desc.size-q->offset)break;
  span=q->length?q->length:desc.size-q->offset;
  if(span>PW_D3D9_RESOURCE_MAX_LOCK){hr=E_NOTIMPL;break;}
  if(r->kind==PW_D3D9_KIND_VERTEX_BUFFER)hr=IDirect3DVertexBuffer9_Lock(r->object.vb,q->offset,q->length,(void **)&r->mapping,q->flags);
  else hr=IDirect3DIndexBuffer9_Lock(r->object.ib,q->offset,q->length,(void **)&r->mapping,q->flags);
  if(SUCCEEDED(hr)){
   if(!r->mapping){unlock(r);hr=E_FAIL;break;}
   r->generation++;r->span=span;r->written=0;r->readonly=!!(q->flags&D3DLOCK_READONLY);
   reply->lock_generation=r->generation;reply->length=span;
  }else r->mapping=NULL;
  break;
 case PW_D3D9_RESOURCE_READ:case PW_D3D9_RESOURCE_WRITE:
  if(!r->mapping||q->lock_generation!=r->generation||!q->count||q->count>PW_D3D9_RESOURCE_CHUNK||q->offset>r->span||q->count>r->span-q->offset)break;
  if(q->operation==PW_D3D9_RESOURCE_WRITE){
   if(r->readonly||q->offset!=r->written)break;
   memcpy(r->mapping+q->offset,q->data,q->count);r->written+=q->count;
  }else{
   memcpy(reply->data,r->mapping+q->offset,q->count);reply->count=q->count;
   reply->offset=q->offset;reply->lock_generation=r->generation;
  }hr=S_OK;break;
 case PW_D3D9_RESOURCE_UNLOCK:case PW_D3D9_RESOURCE_CANCEL_LOCK:
  if(!r->mapping||q->lock_generation!=r->generation)break;
  if(q->operation==PW_D3D9_RESOURCE_UNLOCK&&!r->readonly&&r->written!=r->span)break;
  hr=unlock(r);break;
 default:hr=E_NOTIMPL;break;
 }
 reply->hresult=(uint32_t)hr;
 if(FAILED(hr)){memset(&reply->desc,0,sizeof(reply->desc));reply->lock_generation=0;reply->length=reply->count=reply->offset=0;}
}
