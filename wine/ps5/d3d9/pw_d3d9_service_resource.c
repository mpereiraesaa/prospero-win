/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_service_resource.h"
#include "pw_d3d9_native_resource.h"
#include "pw_d3d9_session.h"
#include <d3d9.h>
#include <string.h>
struct resource_owner { struct pw_d3d9_native_resource *native; struct pw_d3d9_object_ref parent; };
static const struct pw_d3d9_object_slot *lookup(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref)
{return pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);}
static HRESULT publish(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,
 struct pw_d3d9_native_resource *native,struct pw_d3d9_object_ref *out)
{
    uintptr_t identity=pw_d3d9_native_resource_identity(native);uint32_t kind=pw_d3d9_native_resource_kind(native);
    struct pw_d3d9_object_ref existing;
    if(identity&&pw_d3d9_object_find(objects,identity,&existing)){
        const struct pw_d3d9_object_slot *slot=lookup(objects,existing);
        HRESULT hr=slot&&slot->kind==kind&&pw_d3d9_object_addref(objects,existing,1)?S_OK:E_FAIL;
        pw_d3d9_native_resource_destroy(native);if(SUCCEEDED(hr))*out=existing;return hr;
    }
    struct resource_owner *owner=HeapAlloc(GetProcessHeap(),0,sizeof(*owner));
    if(!owner){pw_d3d9_native_resource_destroy(native);return E_OUTOFMEMORY;}
    *owner=(struct resource_owner){native,parent};
    struct pw_d3d9_object_ref ref={0};HRESULT hr=E_OUTOFMEMORY;
    if(identity&&pw_d3d9_object_queue(objects,parent)){
        if(pw_d3d9_object_reserve(objects,&ref)){
            if(pw_d3d9_object_commit(objects,ref,identity,(uintptr_t)owner,kind)){*out=ref;return S_OK;}
            pw_d3d9_object_abort(objects,ref);hr=E_FAIL;
        }
        pw_d3d9_object_complete(objects,parent);
    }
    pw_d3d9_native_resource_destroy(native);HeapFree(GetProcessHeap(),0,owner);return hr;
}
void pw_d3d9_service_resource_call(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref target,
 const struct pw_d3d9_resource_request *q,struct pw_d3d9_resource_reply *r)
{
    memset(r,0,sizeof(*r));r->operation=q->operation;r->hresult=D3DERR_INVALIDCALL;
    const struct pw_d3d9_object_slot *slot=lookup(objects,target);
    int create=q->operation==PW_D3D9_RESOURCE_CREATE_VB||q->operation==PW_D3D9_RESOURCE_CREATE_IB;
    if(!slot||(create?slot->kind!=PW_D3D9_KIND_DEVICE:(slot->kind!=PW_D3D9_KIND_VERTEX_BUFFER&&slot->kind!=PW_D3D9_KIND_INDEX_BUFFER))||!pw_d3d9_object_queue(objects,target))return;
    if(create){
        struct pw_d3d9_native_resource *native=NULL;
        pw_d3d9_native_resource_create(pw_d3d9_native_device_backend((void *)slot->context),q,r,&native);
        if(native){HRESULT hr=publish(objects,target,native,&r->object);if(FAILED(hr))r->hresult=hr;}
    }else{
        struct resource_owner *owner=(void *)slot->context;pw_d3d9_native_resource_call(owner->native,q,r);
    }
    if(!pw_d3d9_object_complete(objects,target))r->hresult=E_FAIL;
}
HRESULT pw_d3d9_service_resource_destroy(struct pw_d3d9_objects *objects,uintptr_t context)
{
    struct resource_owner *owner=(void *)context;
    HRESULT hr=pw_d3d9_native_resource_destroy(owner->native);
    if(!pw_d3d9_object_complete(objects,owner->parent))hr=E_FAIL;
    HeapFree(GetProcessHeap(),0,owner);return hr;
}
HRESULT pw_d3d9_service_resource_acquire(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,
 uint32_t kind,void *native_device,void **out)
{
    if(!out)return E_POINTER;
    *out=NULL;const struct pw_d3d9_object_slot *slot=lookup(objects,ref);
    if((kind!=PW_D3D9_KIND_VERTEX_BUFFER&&kind!=PW_D3D9_KIND_INDEX_BUFFER)||!slot||slot->kind!=kind)return D3DERR_INVALIDCALL;
    struct resource_owner *owner=(void *)slot->context;const struct pw_d3d9_object_slot *parent=lookup(objects,owner->parent);
    if(!parent||parent->kind!=PW_D3D9_KIND_DEVICE||pw_d3d9_native_device_backend((void *)parent->context)!=native_device)return D3DERR_INVALIDCALL;
    IUnknown *backend=pw_d3d9_native_resource_backend(owner->native);IUnknown_AddRef(backend);*out=backend;return S_OK;
}
