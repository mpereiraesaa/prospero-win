/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_service_up.h"
#include "pw_d3d9_native_up.h"
#include "pw_d3d9_session.h"
#include "pw_d3d9_kinds.h"
#include <d3d9.h>
#include <string.h>
struct up_owner {struct up_owner *next;struct pw_d3d9_object_ref parent;struct pw_d3d9_up_upload upload;size_t allocated;unsigned pinned;};
static struct up_owner *uploads;
static size_t allocated;
static int same(struct pw_d3d9_object_ref a,struct pw_d3d9_object_ref b){return a.id==b.id&&a.generation==b.generation;}
static void finish(struct pw_d3d9_objects *objects,struct up_owner *owner)
{
    pw_d3d9_up_upload_finish(&owner->upload);
    if(owner->upload.storage){HeapFree(GetProcessHeap(),0,owner->upload.storage);owner->upload.storage=NULL;}
    owner->upload.capacity=0;allocated-=owner->allocated;owner->allocated=0;
    if(owner->pinned){pw_d3d9_object_complete(objects,owner->parent);owner->pinned=0;}
}
void pw_d3d9_service_up_retire(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref parent)
{
    struct up_owner **link=&uploads;
    while(*link){struct up_owner *owner=*link;if(same(owner->parent,parent)){*link=owner->next;finish(objects,owner);HeapFree(GetProcessHeap(),0,owner);return;}link=&owner->next;}
}
void pw_d3d9_service_up_shutdown(struct pw_d3d9_objects *objects)
{while(uploads)pw_d3d9_service_up_retire(objects,uploads->parent);}
void pw_d3d9_service_up_call(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref target,const struct pw_d3d9_up_request *q,struct pw_d3d9_up_reply *r)
{
    unsigned char wire[PW_D3D9_UP_WIRE_MAX];size_t bytes;
    memset(r,0,sizeof(*r));r->operation=q->operation;r->hresult=D3DERR_INVALIDCALL;
    if(pw_d3d9_up_encode(wire,sizeof(wire),&bytes,q)!=PW_D3D9_UP_OK)return;
    const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,target);
    if(!slot||slot->kind!=PW_D3D9_KIND_DEVICE||!pw_d3d9_object_queue(objects,target))return;
    struct up_owner *owner;for(owner=uploads;owner&&!same(owner->parent,target);owner=owner->next){}
    if(!owner&&q->operation==PW_D3D9_UP_BEGIN){
        owner=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*owner));
        if(!owner){r->hresult=E_OUTOFMEMORY;goto done;}
        owner->parent=target;owner->next=uploads;uploads=owner;
    }
    if(!owner)goto done;
    if(q->operation==PW_D3D9_UP_BEGIN&&!owner->upload.transfer){
        size_t total=(size_t)q->draw.vertex_bytes+q->draw.index_bytes;
        if(total>PW_D3D9_UP_LIMIT-allocated){r->hresult=E_OUTOFMEMORY;goto done;}
        owner->upload.storage=HeapAlloc(GetProcessHeap(),0,total?total:1);
        if(!owner->upload.storage){r->hresult=E_OUTOFMEMORY;goto done;}
        owner->allocated=total;allocated+=total;owner->upload.capacity=total;
        if(!pw_d3d9_object_queue(objects,target)){finish(objects,owner);goto done;}
        owner->pinned=1;
    }
    int matched_commit=q->operation==PW_D3D9_UP_COMMIT&&owner->upload.transfer&&q->transfer==owner->upload.transfer;
    if(pw_d3d9_up_upload_apply(&owner->upload,q,&r->transfer)!=PW_D3D9_UP_OK){
        if(!owner->upload.transfer||matched_commit)finish(objects,owner);
        goto done;
    }
    r->hresult=S_OK;
    if(q->operation==PW_D3D9_UP_ABORT)finish(objects,owner);
    else if(q->operation==PW_D3D9_UP_COMMIT){
        r->hresult=pw_d3d9_native_up_dispatch(pw_d3d9_native_device_backend((void *)slot->context),&owner->upload);
        finish(objects,owner);
    }
 done:
    if(!pw_d3d9_object_complete(objects,target))r->hresult=E_FAIL;
    if(FAILED((HRESULT)r->hresult))r->transfer=0;
}
