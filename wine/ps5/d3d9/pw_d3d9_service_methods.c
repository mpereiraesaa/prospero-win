/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_service_methods.h"
#include "pw_d3d9_service_resource.h"
#include "pw_d3d9_session.h"
#include "pw_d3d9_native_command.h"
#include "pw_d3d9_native_getter.h"
#include "../pw_d3d9_bridge_wire.h"
static HRESULT acquire(void *context,uint32_t id,uint32_t generation,uint32_t kind,IDirect3DDevice9 *device,void **out)
{return pw_d3d9_service_resource_acquire(context,(struct pw_d3d9_object_ref){id,generation},kind,device,out);}
int pw_d3d9_service_methods(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,
 uint32_t opcode,const void *input,size_t input_bytes,void *output,size_t capacity,size_t *bytes,HRESULT *hr)
{
    struct pw_d3d9_command command;struct pw_d3d9_getter_request getter;
    *bytes=0;*hr=D3DERR_INVALIDCALL;
    if(opcode==PW_D3D9_COMMAND_CALL){if(pw_d3d9_command_decode(&command,input,input_bytes)!=PW_D3D9_COMMAND_OK)return 0;}
    else if(opcode==PW_D3D9_GETTER_CALL){if(pw_d3d9_getter_decode(&getter,input,input_bytes)!=PW_D3D9_GETTER_OK)return 0;}
    else return 0;
    const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);
    if(!slot||slot->kind!=PW_D3D9_KIND_DEVICE||!pw_d3d9_object_queue(objects,ref))return 1;
    IDirect3DDevice9 *device=pw_d3d9_native_device_backend((void *)slot->context);int valid;
    if(opcode==PW_D3D9_COMMAND_CALL){
        *hr=pw_d3d9_native_command_dispatch(device,&command,acquire,objects);
        valid=pw_d3d9_command_reply_encode(output,capacity,bytes,command.method,(uint32_t)*hr)==PW_D3D9_COMMAND_OK;
    }else{
        struct pw_d3d9_getter_reply reply;
        valid=pw_d3d9_native_getter_dispatch(device,&getter,&reply)==PW_D3D9_GETTER_OK;
        if(valid){*hr=(HRESULT)reply.hresult;valid=pw_d3d9_getter_reply_encode(output,capacity,bytes,&getter,&reply)==PW_D3D9_GETTER_OK;}
    }
    if(!pw_d3d9_object_complete(objects,ref))return 0;
    return valid;
}
