/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_binding_leases.h"
#include "../pw_d3d9_command_policy.h"
#include <string.h>
int pw_d3d9_binding_leases_prepare(struct pw_d3d9_binding_leases *leases,
    struct pw_d3d9_objects *objects,IDirect3DDevice9 *device,const void *input,size_t bytes,
    pw_d3d9_command_acquire_fn acquire,void *context)
{
    struct pw_d3d9_command command;
    if(!leases||leases->prepared||!objects||!device||objects->cancelled)return 0;
    memset(leases,0,sizeof(*leases));
    if(pw_d3d9_batch_decode(&leases->batch,input,bytes))return 0;
    /* An ineligible suffix cannot leave partial pins or execute a prefix. */
    for(uint32_t n=0;n<leases->batch.count;n++){
        struct pw_d3d9_binding_lease *l=leases->records+n;
        if(pw_d3d9_batch_command(&command,&leases->batch,n))return 0;
        int status=pw_d3d9_binding_plan(&command,&l->binding);
        if(status!=PW_D3D9_BINDING_READY&&
           (status!=PW_D3D9_BINDING_OTHER||!pw_d3d9_command_can_queue(&command)))return 0;
        l->device=device;l->result=S_OK;
    }
    leases->objects=objects;leases->prepared=1;
    for(uint32_t n=0;n<leases->batch.count;n++){
        struct pw_d3d9_binding_lease *l=leases->records+n;
        struct pw_d3d9_object_ref ref={l->binding.id,l->binding.generation};
        if(!ref.id)continue;
        const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);
        if(!slot||slot->kind!=l->binding.kind||!pw_d3d9_object_queue(objects,ref)){
            l->result=D3DERR_INVALIDCALL;continue;
        }
        l->queued=1;
    }
    /* Pin every registry generation before any native AddRef/callback. */
    for(uint32_t n=0;n<leases->batch.count;n++){
        struct pw_d3d9_binding_lease *l=leases->records+n;
        struct pw_d3d9_object_ref ref={l->binding.id,l->binding.generation};
        if(!l->queued)continue;
        void *native=NULL;
        HRESULT hr=acquire?acquire(context,ref.id,ref.generation,l->binding.kind,device,&native):D3DERR_INVALIDCALL;
        if(hr!=S_OK||!native){
            if(native)IUnknown_Release((IUnknown *)native);
            if(FAILED(hr))l->result=hr;
            else if(hr!=S_OK){l->unexpected_result=(uint32_t)hr;l->result=E_FAIL;}
            else l->result=D3DERR_INVALIDCALL;
        }else l->native=native;
    }
    return 1;
}
HRESULT pw_d3d9_binding_lease_acquire(void *context,uint32_t id,uint32_t generation,
    uint32_t kind,IDirect3DDevice9 *device,void **out)
{
    struct pw_d3d9_binding_lease *l=context;
    if(!out)return E_POINTER;
    *out=NULL;
    if(!l||!id||l->binding.id!=id||l->binding.generation!=generation||
       l->binding.kind!=kind||l->device!=device)return D3DERR_INVALIDCALL;
    if(l->result!=S_OK)return l->result;
    if(!l->queued||!l->native)return D3DERR_INVALIDCALL;
    IUnknown_AddRef(l->native);*out=l->native;return S_OK;
}
int pw_d3d9_binding_leases_release(struct pw_d3d9_binding_leases *leases)
{
    int ok=1;
    if(!leases||!leases->prepared)return 0;
    for(uint32_t n=0;n<leases->batch.count;n++){
        struct pw_d3d9_binding_lease *l=leases->records+n;
        if(l->native){IUnknown_Release(l->native);l->native=NULL;}
        if(l->queued){
            struct pw_d3d9_object_ref ref={l->binding.id,l->binding.generation};
            if(!pw_d3d9_object_complete(leases->objects,ref))ok=0;
            l->queued=0;
        }
    }
    memset(leases,0,sizeof(*leases));return ok;
}
