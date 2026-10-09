/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_service_texture.h"
#include "pw_d3d9_native_texture.h"
#include "pw_d3d9_session.h"
#include <d3d9.h>
#include <string.h>
struct texture_owner { struct pw_d3d9_native_texture *native; struct pw_d3d9_object_ref parent; unsigned implicit,prepared,zero,parked; };
static const struct pw_d3d9_object_slot *lookup(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref)
{return pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);}
static HRESULT publish(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,
 struct pw_d3d9_native_texture *native,struct pw_d3d9_object_ref *out)
{
    uintptr_t identity=pw_d3d9_native_texture_identity(native);uint32_t kind=pw_d3d9_native_texture_kind(native);
    struct pw_d3d9_object_ref existing;
    if(identity&&pw_d3d9_object_find(objects,identity,&existing)){
        const struct pw_d3d9_object_slot *slot=lookup(objects,existing);
        HRESULT hr=slot&&slot->kind==kind&&pw_d3d9_object_addref(objects,existing,1)?S_OK:E_FAIL;
        pw_d3d9_native_texture_destroy(native);if(SUCCEEDED(hr))*out=existing;return hr;
    }
    struct texture_owner *owner=HeapAlloc(GetProcessHeap(),0,sizeof(*owner));
    if(!owner){pw_d3d9_native_texture_destroy(native);return E_OUTOFMEMORY;}
    *owner=(struct texture_owner){.native=native,.parent=parent};
    struct pw_d3d9_object_ref ref={0};HRESULT hr=E_OUTOFMEMORY;
    if(identity&&pw_d3d9_object_queue(objects,parent)){
        if(pw_d3d9_object_reserve(objects,&ref)){
            if(pw_d3d9_object_commit(objects,ref,identity,(uintptr_t)owner,kind)){*out=ref;return S_OK;}
            pw_d3d9_object_abort(objects,ref);hr=E_FAIL;
        }
        pw_d3d9_object_complete(objects,parent);
    }
    pw_d3d9_native_texture_destroy(native);HeapFree(GetProcessHeap(),0,owner);return hr;
}
HRESULT pw_d3d9_service_texture_adopt(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,
 uint32_t kind,void *owned,struct pw_d3d9_object_ref *out)
{
    const struct pw_d3d9_object_slot *slot=lookup(objects,parent);
    if(!slot||slot->kind!=PW_D3D9_KIND_DEVICE){if(owned)IUnknown_Release((IUnknown *)owned);return D3DERR_INVALIDCALL;}
    struct pw_d3d9_native_texture *native=NULL;
    HRESULT hr=pw_d3d9_native_texture_adopt(pw_d3d9_native_device_backend((void *)slot->context),kind,owned,&native);
    if(SUCCEEDED(hr)){HRESULT published=publish(objects,parent,native,out);if(FAILED(published))hr=published;}
    return hr;
}
static struct texture_owner *owned_texture(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,
 struct pw_d3d9_object_ref parent)
{
    const struct pw_d3d9_object_slot *slot=lookup(objects,ref);
    if(!slot||(slot->kind!=PW_D3D9_KIND_TEXTURE_2D&&slot->kind!=PW_D3D9_KIND_SURFACE))return NULL;
    struct texture_owner *owner=(void *)slot->context;
    return owner->parent.id==parent.id&&owner->parent.generation==parent.generation?owner:NULL;
}
void pw_d3d9_service_texture_call(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref target,
 const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *r)
{
    memset(r,0,sizeof(*r));r->operation=q->operation;r->hresult=D3DERR_INVALIDCALL;
    const struct pw_d3d9_object_slot *slot=lookup(objects,target);
    int create=q->operation==PW_D3D9_TEXTURE_CREATE||q->operation==PW_D3D9_TEXTURE_CREATE_SURFACE||
      q->operation==PW_D3D9_TEXTURE_CREATE_RT||q->operation==PW_D3D9_TEXTURE_CREATE_DEPTH;
    int copy=q->operation==PW_D3D9_TEXTURE_UPDATE||q->operation==PW_D3D9_TEXTURE_UPDATE_SURFACE||
      q->operation==PW_D3D9_TEXTURE_STRETCH||q->operation==PW_D3D9_TEXTURE_RT_DATA;
    if(!slot||((create||copy)?slot->kind!=PW_D3D9_KIND_DEVICE:
       (slot->kind!=PW_D3D9_KIND_TEXTURE_2D&&slot->kind!=PW_D3D9_KIND_SURFACE))||!pw_d3d9_object_queue(objects,target))return;
    struct pw_d3d9_native_texture *native=NULL;
    if(create){
        pw_d3d9_native_texture_create(pw_d3d9_native_device_backend((void *)slot->context),q,r,&native);
        if(native){HRESULT hr=publish(objects,target,native,&r->object);if(FAILED(hr))r->hresult=hr;}
    }else if(copy){
        struct texture_owner *source=owned_texture(objects,q->source,target),*destination=owned_texture(objects,q->destination,target);
        if(source&&destination&&pw_d3d9_object_queue(objects,q->source)){
            if(pw_d3d9_object_queue(objects,q->destination)){
                pw_d3d9_native_texture_copy(pw_d3d9_native_device_backend((void *)slot->context),source->native,destination->native,q,r);
                if(!pw_d3d9_object_complete(objects,q->destination))r->hresult=E_FAIL;
            }
            if(!pw_d3d9_object_complete(objects,q->source))r->hresult=E_FAIL;
        }
    }else if(q->operation==PW_D3D9_TEXTURE_CONTAINER){
        struct texture_owner *owner=(void *)slot->context;void *owned=NULL;
        pw_d3d9_native_texture_container(owner->native,q,r,&owned);
        if(owned){
            HRESULT hr=E_NOINTERFACE;
            if(r->container_kind==PW_D3D9_KIND_DEVICE){
                const struct pw_d3d9_object_slot *parent=lookup(objects,owner->parent);
                if(parent&&parent->kind==PW_D3D9_KIND_DEVICE&&pw_d3d9_native_device_backend((void *)parent->context)==owned&&pw_d3d9_object_addref(objects,owner->parent,1)){
                    r->object=owner->parent;hr=S_OK;
                }
                IUnknown_Release((IUnknown *)owned);
            }else if(r->container_kind==PW_D3D9_KIND_TEXTURE_2D)
                hr=pw_d3d9_service_texture_adopt(objects,owner->parent,r->container_kind,owned,&r->object);
            else IUnknown_Release((IUnknown *)owned);
            if(FAILED(hr)){r->hresult=hr;r->object=(struct pw_d3d9_object_ref){0};r->container_kind=r->levels=0;}
        }
    }else{
        struct texture_owner *owner=(void *)slot->context;
        pw_d3d9_native_texture_call(owner->native,q,r,&native);
        if(native){HRESULT hr=publish(objects,owner->parent,native,&r->object);if(FAILED(hr))r->hresult=hr;}
    }
    if(!pw_d3d9_object_complete(objects,target))r->hresult=E_FAIL;
}
HRESULT pw_d3d9_service_texture_destroy(struct pw_d3d9_objects *objects,uintptr_t context)
{
    struct texture_owner *owner=(void *)context;
    HRESULT hr=pw_d3d9_native_texture_destroy(owner->native);
    if(!pw_d3d9_object_complete(objects,owner->parent))hr=E_FAIL;
    HeapFree(GetProcessHeap(),0,owner);return hr;
}
HRESULT pw_d3d9_service_texture_acquire(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,
 uint32_t kind,void *native_device,void **out)
{
    if(!out)return E_POINTER;
    *out=NULL;const struct pw_d3d9_object_slot *slot=lookup(objects,ref);
    if((kind!=PW_D3D9_KIND_TEXTURE_2D&&kind!=PW_D3D9_KIND_SURFACE)||!slot||slot->kind!=kind)return D3DERR_INVALIDCALL;
    struct texture_owner *owner=(void *)slot->context;const struct pw_d3d9_object_slot *parent=lookup(objects,owner->parent);
    if(!parent||parent->kind!=PW_D3D9_KIND_DEVICE||pw_d3d9_native_device_backend((void *)parent->context)!=native_device)return D3DERR_INVALIDCALL;
    IUnknown *backend=pw_d3d9_native_texture_backend(owner->native);if(!backend)return D3DERR_INVALIDCALL;IUnknown_AddRef(backend);*out=backend;return S_OK;
}

static struct texture_owner *implicit_owner(struct pw_d3d9_objects *objects,UINT index,struct pw_d3d9_object_ref parent)
{
 struct pw_d3d9_object_slot *slot=&objects->slots[index];
 if(slot->state!=PW_D3D9_LIVE||slot->kind!=PW_D3D9_KIND_SURFACE)return NULL;
 struct texture_owner *owner=(void *)slot->context;
 return owner->implicit&&owner->parent.id==parent.id&&owner->parent.generation==parent.generation?owner:NULL;
}
static HRESULT destroy_retired(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref)
{
 uintptr_t context;
 if(!pw_d3d9_object_take_destroy(objects,ref,&context))return E_FAIL;
 HRESULT hr=pw_d3d9_service_texture_destroy(objects,context);
 if(!pw_d3d9_object_finish_destroy(objects,ref))return E_FAIL;
 return hr;
}
HRESULT pw_d3d9_service_texture_owners_list(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,
 struct pw_d3d9_object_ref *out,UINT capacity,UINT *count)
{
 if(!count||(!out&&capacity))return E_POINTER;
 *count=0;
 for(UINT i=0;i<objects->capacity;i++)if(implicit_owner(objects,i,parent)){
  if(*count>=capacity)return E_OUTOFMEMORY;
  out[(*count)++]=(struct pw_d3d9_object_ref){i+1,objects->slots[i].generation};
 }
 return S_OK;
}
HRESULT pw_d3d9_service_texture_owners_capture(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,
 UINT backbuffers,BOOL auto_depth)
{
 const struct pw_d3d9_object_slot *device=lookup(objects,parent);
 if(!device||device->kind!=PW_D3D9_KIND_DEVICE||backbuffers>PW_D3D9_SURFACE_OWNER_MAX||
    (auto_depth&&backbuffers==PW_D3D9_SURFACE_OWNER_MAX))return D3DERR_INVALIDCALL;
 for(UINT i=0;i<objects->capacity;i++)if(implicit_owner(objects,i,parent))return D3DERR_INVALIDCALL;
 IDirect3DDevice9 *backend=pw_d3d9_native_device_backend((void *)device->context);
 HRESULT hr=S_OK;
 for(UINT i=0;i<backbuffers+(auto_depth?1u:0u);i++){
  IDirect3DSurface9 *surface=NULL;struct pw_d3d9_object_ref ref={0};
  hr=i<backbuffers?IDirect3DDevice9_GetBackBuffer(backend,0,i,D3DBACKBUFFER_TYPE_MONO,&surface):
                   IDirect3DDevice9_GetDepthStencilSurface(backend,&surface);
  if(FAILED(hr)||!surface){if(surface)IDirect3DSurface9_Release(surface);if(SUCCEEDED(hr))hr=E_FAIL;break;}
  hr=pw_d3d9_service_texture_adopt(objects,parent,PW_D3D9_KIND_SURFACE,surface,&ref);
  if(FAILED(hr))break;
  struct texture_owner *owner=owned_texture(objects,ref,parent);
  if(!owner||!pw_d3d9_object_owner_hold(objects,ref)){pw_d3d9_object_release(objects,ref);hr=E_FAIL;break;}
  owner->implicit=1;
  if(!pw_d3d9_object_release(objects,ref)){hr=E_FAIL;break;}
 }
 if(FAILED(hr)){HRESULT cleanup=pw_d3d9_service_texture_owners_drain(objects,parent);if(FAILED(cleanup))return cleanup;}
 return hr;
}
HRESULT pw_d3d9_service_texture_owners_prepare(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,
 const struct pw_d3d9_object_ref *zero,UINT count)
{
 if(count>PW_D3D9_SURFACE_OWNER_MAX||(!zero&&count))return D3DERR_INVALIDCALL;
 for(UINT i=0;i<objects->capacity;i++){
  struct texture_owner *owner=implicit_owner(objects,i,parent);
  if(owner&&(owner->prepared||owner->parked))return D3DERR_INVALIDCALL;
 }
 for(UINT i=0;i<count;i++){
  const struct pw_d3d9_object_slot *slot=lookup(objects,zero[i]);
  struct texture_owner *owner=slot&&zero[i].id?implicit_owner(objects,zero[i].id-1,parent):NULL;
  if(!owner||slot->guest_refs>1||slot->queued_refs||!pw_d3d9_native_texture_can_park(owner->native))return D3DERR_INVALIDCALL;
  for(UINT j=0;j<i;j++)if(zero[i].id==zero[j].id)return D3DERR_INVALIDCALL;
 }
 for(UINT i=0;i<objects->capacity;i++){
  struct texture_owner *owner=implicit_owner(objects,i,parent);if(!owner)continue;
  if(!objects->slots[i].guest_refs&&(objects->slots[i].queued_refs||!pw_d3d9_native_texture_can_park(owner->native)))return D3DERR_INVALIDCALL;
 }
 for(UINT i=0;i<objects->capacity;i++){
  struct texture_owner *owner=implicit_owner(objects,i,parent);if(!owner)continue;
  owner->prepared=1;owner->zero=0;
  for(UINT j=0;j<count;j++)if(zero[j].id==i+1)owner->zero=1;
 }
 return S_OK;
}
HRESULT pw_d3d9_service_texture_owners_begin_reset(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent)
{
 for(UINT i=0;i<objects->capacity;i++){
  struct texture_owner *owner=implicit_owner(objects,i,parent);if(!owner)continue;
  if(!owner->prepared||owner->parked||((owner->zero||!objects->slots[i].guest_refs)&&!pw_d3d9_native_texture_can_park(owner->native)))return D3DERR_INVALIDCALL;
 }
 /* A getter between PREPARE and Reset can expose a previously unseen owner.
  * Only classify those unsnapshotted entries using their current guest count. */
 for(UINT i=0;i<objects->capacity;i++){
  struct texture_owner *owner=implicit_owner(objects,i,parent);if(!owner)continue;
  owner->zero|=!objects->slots[i].guest_refs;
  if(!owner->zero)continue;
  HRESULT hr=pw_d3d9_native_texture_park(owner->native);if(FAILED(hr))return hr;
  owner->parked=1;
 }
 return S_OK;
}
HRESULT pw_d3d9_service_texture_owners_finish_reset(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,BOOL restore)
{
 HRESULT result=S_OK;
 for(UINT i=0;i<objects->capacity;i++){
  struct texture_owner *owner=implicit_owner(objects,i,parent);if(!owner)continue;
  if(!owner->prepared)return E_FAIL;
  struct pw_d3d9_object_ref ref={i+1,objects->slots[i].generation};
  if(owner->parked)pw_d3d9_native_texture_unpark(owner->native,restore);
  owner->parked=0;
  if(restore){owner->prepared=owner->zero=0;continue;}
  unsigned zero=owner->zero;owner->implicit=owner->prepared=owner->zero=0;
  if(zero&&objects->slots[i].guest_refs&&!pw_d3d9_object_release(objects,ref))result=E_FAIL;
  if(!pw_d3d9_object_owner_drop(objects,ref))result=E_FAIL;
  if(!objects->slots[i].guest_refs){HRESULT hr=destroy_retired(objects,ref);if(FAILED(hr))result=hr;}
 }
 return result;
}
HRESULT pw_d3d9_service_texture_owners_drain(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent)
{
 for(UINT i=0;i<objects->capacity;i++){
  struct texture_owner *owner=implicit_owner(objects,i,parent);
  if(owner&&(objects->slots[i].guest_refs||objects->slots[i].queued_refs||owner->parked||owner->prepared))return D3DERR_INVALIDCALL;
 }
 HRESULT result=S_OK;
 for(UINT i=0;i<objects->capacity;i++){
  struct texture_owner *owner=implicit_owner(objects,i,parent);if(!owner)continue;
  struct pw_d3d9_object_ref ref={i+1,objects->slots[i].generation};owner->implicit=0;
  if(!pw_d3d9_object_owner_drop(objects,ref))return E_FAIL;
  HRESULT hr=destroy_retired(objects,ref);if(FAILED(hr))result=hr;
 }
 return result;
}
