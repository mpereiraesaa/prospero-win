/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_service_stateblock.h"
#include "pw_d3d9_native_stateblock.h"
#include "pw_d3d9_session.h"
#include "pw_d3d9_kinds.h"
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
#include "pw_d3d9_native_draw_state.h"
#endif
struct block_owner {IDirect3DStateBlock9 *native;struct pw_d3d9_object_ref parent;};
static HRESULT publish(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent,IDirect3DStateBlock9 *block,struct pw_d3d9_object_ref *out)
{
    IUnknown *identity=NULL;struct pw_d3d9_object_ref ref;HRESULT hr=IDirect3DStateBlock9_QueryInterface(block,&IID_IUnknown,(void **)&identity);
    if(FAILED(hr)||!identity){IDirect3DStateBlock9_Release(block);return E_NOINTERFACE;}
    if(pw_d3d9_object_find(objects,(uintptr_t)identity,&ref)){
        const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);
        hr=slot&&slot->kind==PW_D3D9_KIND_STATE_BLOCK&&pw_d3d9_object_addref(objects,ref,1)?S_OK:E_FAIL;
        IUnknown_Release(identity);IDirect3DStateBlock9_Release(block);if(SUCCEEDED(hr))*out=ref;return hr;
    }
    struct block_owner *owner=HeapAlloc(GetProcessHeap(),0,sizeof(*owner));hr=E_OUTOFMEMORY;
    if(owner){
        *owner=(struct block_owner){block,parent};
        if(pw_d3d9_object_queue(objects,parent)){
            if(pw_d3d9_object_reserve(objects,&ref)){
                if(pw_d3d9_object_commit(objects,ref,(uintptr_t)identity,(uintptr_t)owner,PW_D3D9_KIND_STATE_BLOCK)){IUnknown_Release(identity);*out=ref;return S_OK;}
                pw_d3d9_object_abort(objects,ref);hr=E_FAIL;
            }
            pw_d3d9_object_complete(objects,parent);
        }
        HeapFree(GetProcessHeap(),0,owner);
    }
    IUnknown_Release(identity);IDirect3DStateBlock9_Release(block);return hr;
}
void pw_d3d9_service_stateblock_call(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,const struct pw_d3d9_stateblock_request *q,struct pw_d3d9_stateblock_reply *r)
{
    *r=(struct pw_d3d9_stateblock_reply){.hresult=D3DERR_INVALIDCALL};
    if(pw_d3d9_stateblock_validate(q)!=PW_D3D9_SB_OK)return;
    unsigned kind=q->method>=59?PW_D3D9_KIND_DEVICE:PW_D3D9_KIND_STATE_BLOCK;
    const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);
    if(!slot||slot->kind!=kind||!pw_d3d9_object_queue(objects,ref))return;
    IDirect3DDevice9 *device=kind==PW_D3D9_KIND_DEVICE?pw_d3d9_native_device_backend((void *)slot->context):NULL;
    IDirect3DStateBlock9 *block=kind==PW_D3D9_KIND_STATE_BLOCK?((struct block_owner *)slot->context)->native:NULL,*created=NULL;
    r->hresult=pw_d3d9_native_stateblock_dispatch(device,block,q,&created);
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
    if(kind==PW_D3D9_KIND_DEVICE&&(q->method==60u||q->method==61u)){
        /* Capture the backend transition before publication can fail. Reset
         * deliberately leaves this state unchanged on the pinned backend. */
        pw_d3d9_native_device_recording_outcome((void *)slot->context,q->method,r->hresult);
        if(r->hresult!=S_OK&&SUCCEEDED((HRESULT)r->hresult))r->hresult=E_FAIL;
    }
#endif
    if(SUCCEEDED((HRESULT)r->hresult)&&(q->method==59||q->method==61)){
        if(!created)r->hresult=E_FAIL;
        else {HRESULT hr=publish(objects,ref,created,&r->object);created=NULL;if(FAILED(hr))r->hresult=hr;}
    }
    if(created)IDirect3DStateBlock9_Release(created);
    if(!pw_d3d9_object_complete(objects,ref))r->hresult=E_FAIL;
    if(FAILED((HRESULT)r->hresult))r->object=(struct pw_d3d9_object_ref){0};
}
HRESULT pw_d3d9_service_stateblock_destroy(struct pw_d3d9_objects *objects,uintptr_t context)
{
    struct block_owner *owner=(void *)context;IDirect3DStateBlock9_Release(owner->native);
    HRESULT hr=pw_d3d9_object_complete(objects,owner->parent)?S_OK:E_FAIL;HeapFree(GetProcessHeap(),0,owner);return hr;
}

int pw_d3d9_service_stateblock_reply(void *output,size_t capacity,size_t *bytes,const struct pw_d3d9_stateblock_request *q,const struct pw_d3d9_stateblock_reply *r)
{
    if(!bytes||capacity<16)return PW_D3D9_SB_INVALID;
    int status=pw_d3d9_stateblock_reply_encode(output,16,q,r);
    if(status==PW_D3D9_SB_OK)*bytes=16;
    return status;
}
