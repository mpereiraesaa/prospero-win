/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_service_program.h"
#include "pw_d3d9_native_program.h"
#include "pw_d3d9_session.h"
#include "pw_d3d9_kinds.h"
#include <d3d9.h>
#include <string.h>
struct program_owner {struct pw_d3d9_native_program *native;struct pw_d3d9_object_ref parent;};
struct upload_owner {struct upload_owner *next;struct pw_d3d9_object_ref parent;struct pw_d3d9_program_upload upload;unsigned pinned;};
static struct upload_owner *uploads;
static unsigned active_uploads;
static int same(struct pw_d3d9_object_ref a,struct pw_d3d9_object_ref b){return a.id==b.id&&a.generation==b.generation;}
static const struct pw_d3d9_object_slot *lookup(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref)
{return pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);}
static void finish(struct pw_d3d9_objects *objects,struct upload_owner *owner)
{
    pw_d3d9_program_upload_finish(&owner->upload);
    if(owner->upload.storage){HeapFree(GetProcessHeap(),0,owner->upload.storage);owner->upload.storage=NULL;owner->upload.capacity=0;}
    if(owner->pinned){pw_d3d9_object_complete(objects,owner->parent);owner->pinned=0;active_uploads--;}
}
void pw_d3d9_service_program_retire(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent)
{
    struct upload_owner **link=&uploads;
    while(*link){struct upload_owner *owner=*link;if(same(owner->parent,parent)){*link=owner->next;finish(objects,owner);HeapFree(GetProcessHeap(),0,owner);return;}link=&owner->next;}
}
void pw_d3d9_service_program_shutdown(struct pw_d3d9_objects *objects)
{while(uploads)pw_d3d9_service_program_retire(objects,uploads->parent);}
static HRESULT publish(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,struct pw_d3d9_native_program *native,struct pw_d3d9_object_ref *out)
{
    uintptr_t identity=pw_d3d9_native_program_identity(native);uint32_t kind=pw_d3d9_native_program_kind(native);struct pw_d3d9_object_ref ref;
    if(identity&&pw_d3d9_object_find(objects,identity,&ref)){
        const struct pw_d3d9_object_slot *slot=lookup(objects,ref);HRESULT hr=slot&&slot->kind==kind&&pw_d3d9_object_addref(objects,ref,1)?S_OK:E_FAIL;
        pw_d3d9_native_program_destroy(native);if(SUCCEEDED(hr))*out=ref;return hr;
    }
    struct program_owner *owner=HeapAlloc(GetProcessHeap(),0,sizeof(*owner));
    if(!owner){pw_d3d9_native_program_destroy(native);return E_OUTOFMEMORY;}
    *owner=(struct program_owner){native,parent};HRESULT hr=E_OUTOFMEMORY;
    if(identity&&pw_d3d9_object_queue(objects,parent)){
        if(pw_d3d9_object_reserve(objects,&ref)){
            if(pw_d3d9_object_commit(objects,ref,identity,(uintptr_t)owner,kind)){*out=ref;return S_OK;}
            pw_d3d9_object_abort(objects,ref);hr=E_FAIL;
        }
        pw_d3d9_object_complete(objects,parent);
    }
    pw_d3d9_native_program_destroy(native);HeapFree(GetProcessHeap(),0,owner);return hr;
}
void pw_d3d9_service_program_call(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref target,const struct pw_d3d9_program_request *q,struct pw_d3d9_program_reply *r)
{
    memset(r,0,sizeof(*r));r->operation=q->operation;r->hresult=D3DERR_INVALIDCALL;
    const struct pw_d3d9_object_slot *slot=lookup(objects,target);
    if(!slot||slot->kind!=PW_D3D9_KIND_DEVICE||!pw_d3d9_object_queue(objects,target))return;
    struct upload_owner *owner;for(owner=uploads;owner&&!same(owner->parent,target);owner=owner->next){}
    if(!owner&&q->operation==PW_D3D9_PROGRAM_BEGIN){
        owner=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*owner));
        if(!owner){r->hresult=E_OUTOFMEMORY;goto done;}
        owner->parent=target;owner->next=uploads;uploads=owner;
    }
    if(!owner)goto done;
    if(q->operation==PW_D3D9_PROGRAM_BEGIN&&!owner->upload.transfer){
        if(active_uploads>=64){r->hresult=E_OUTOFMEMORY;goto done;}
        owner->upload.storage=HeapAlloc(GetProcessHeap(),0,q->total);owner->upload.capacity=q->total;
        if(!owner->upload.storage){r->hresult=E_OUTOFMEMORY;goto done;}
        if(!pw_d3d9_object_queue(objects,target)){finish(objects,owner);goto done;}
        owner->pinned=1;active_uploads++;
    }
    int status=pw_d3d9_program_upload_apply(&owner->upload,q,&r->transfer);
    if(status!=PW_D3D9_PROGRAM_OK){if(!owner->upload.transfer)finish(objects,owner);goto done;}
    r->hresult=S_OK;
    if(q->operation==PW_D3D9_PROGRAM_ABORT){finish(objects,owner);}
    else if(q->operation==PW_D3D9_PROGRAM_COMMIT){
        struct pw_d3d9_native_program *native=NULL;struct pw_d3d9_object_ref created={0};
        r->hresult=pw_d3d9_native_program_create(pw_d3d9_native_device_backend((void *)slot->context),owner->upload.kind,owner->upload.storage,owner->upload.total,&native);
        if(native){HRESULT hr=publish(objects,target,native,&created);if(FAILED(hr))r->hresult=hr;}
        if(SUCCEEDED((HRESULT)r->hresult)){r->id=created.id;r->generation=created.generation;}
        finish(objects,owner);
    }
 done:
    if(!pw_d3d9_object_complete(objects,target))r->hresult=E_FAIL;
    if(FAILED((HRESULT)r->hresult))r->transfer=r->id=r->generation=0;
}
HRESULT pw_d3d9_service_program_destroy(struct pw_d3d9_objects *objects,uintptr_t context)
{
    struct program_owner *owner=(void *)context;pw_d3d9_native_program_destroy(owner->native);
    HRESULT hr=pw_d3d9_object_complete(objects,owner->parent)?S_OK:E_FAIL;HeapFree(GetProcessHeap(),0,owner);return hr;
}
HRESULT pw_d3d9_service_program_acquire(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,uint32_t kind,void *native_device,void **out)
{
    if(!out)return E_POINTER;
    *out=NULL;const struct pw_d3d9_object_slot *slot=lookup(objects,ref);
    if(kind<PW_D3D9_KIND_VERTEX_DECLARATION||kind>PW_D3D9_KIND_PIXEL_SHADER||!slot||slot->kind!=kind)return D3DERR_INVALIDCALL;
    struct program_owner *owner=(void *)slot->context;const struct pw_d3d9_object_slot *parent=lookup(objects,owner->parent);
    if(!parent||parent->kind!=PW_D3D9_KIND_DEVICE||pw_d3d9_native_device_backend((void *)parent->context)!=native_device)return D3DERR_INVALIDCALL;
    IUnknown *backend=pw_d3d9_native_program_backend(owner->native);IUnknown_AddRef(backend);*out=backend;return S_OK;
}
