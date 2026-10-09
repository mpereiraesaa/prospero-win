/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_service_object_getter.h"
#include "pw_d3d9_native_object_getter.h"
#include "pw_d3d9_service_resource.h"
#include "pw_d3d9_service_texture.h"
#include "pw_d3d9_service_program.h"
#include "pw_d3d9_session.h"
#include <d3d9.h>
#include <string.h>
void pw_d3d9_service_object_getter(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref target,
 const struct pw_d3d9_object_getter_request *q,struct pw_d3d9_object_getter_reply *r)
{
    memset(r,0,sizeof(*r));r->method=q->method;r->hresult=D3DERR_INVALIDCALL;
    const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,target);
    if(!slot||slot->kind!=PW_D3D9_KIND_DEVICE||!pw_d3d9_object_queue(objects,target))return;
    struct pw_d3d9_native_object_result result={0};
    HRESULT hr=pw_d3d9_native_object_getter(pw_d3d9_native_device_backend((void *)slot->context),q,&result);
    if(SUCCEEDED(hr)){
        r->offset=result.offset;r->stride=result.stride;
        if(result.object){
            struct pw_d3d9_object_ref ref={0};HRESULT published=E_NOINTERFACE;
            if(result.kind==3||result.kind==4)published=pw_d3d9_service_resource_adopt(objects,target,result.kind,result.object,&ref);
            else if(result.kind==5||result.kind==6)published=pw_d3d9_service_texture_adopt(objects,target,result.kind,result.object,&ref);
            else if(result.kind>=7&&result.kind<=9)published=pw_d3d9_service_program_adopt(objects,target,result.kind,result.object,&ref);
            else pw_d3d9_native_object_result_release(&result);
            result.object=NULL; /* Every typed adopt consumes the backend reference. */
            if(FAILED(published))hr=published;
            else{r->kind=result.kind;r->id=ref.id;r->generation=ref.generation;}
        }
    }
    pw_d3d9_native_object_result_release(&result);
    if(!pw_d3d9_object_complete(objects,target)){pw_d3d9_objects_cancel(objects);hr=E_FAIL;}
    r->hresult=hr;
    if(FAILED(hr)){r->kind=r->id=r->generation=r->offset=r->stride=0;}
}
