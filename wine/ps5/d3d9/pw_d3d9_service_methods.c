#ifdef PW_D3D9_ENABLE_TEXTURE
#include "pw_d3d9_service_texture.h"
#endif
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_service_methods.h"
#ifdef PW_D3D9_ENABLE_PROGRAM
#include "pw_d3d9_service_program.h"
#endif
#include "pw_d3d9_service_resource.h"
#include "pw_d3d9_session.h"
#include "pw_d3d9_native_command.h"
#include "pw_d3d9_native_getter.h"
#ifdef PW_D3D9_ENABLE_GAMMA
#include "pw_d3d9_native_gamma.h"
#endif
#ifdef PW_D3D9_ENABLE_CURSOR
#include "pw_d3d9_cursor.h"
#endif
#include "../pw_d3d9_bridge_wire.h"
static HRESULT acquire(void *context,uint32_t id,uint32_t generation,uint32_t kind,IDirect3DDevice9 *device,void **out)
{
#ifdef PW_D3D9_ENABLE_PROGRAM
 if(kind>=7&&kind<=9)return pw_d3d9_service_program_acquire(context,(struct pw_d3d9_object_ref){id,generation},kind,device,out);
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
 if(kind==5||kind==6)return pw_d3d9_service_texture_acquire(context,(struct pw_d3d9_object_ref){id,generation},kind,device,out);
#endif
 return pw_d3d9_service_resource_acquire(context,(struct pw_d3d9_object_ref){id,generation},kind,device,out);
}
int pw_d3d9_service_methods(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,
 uint32_t opcode,const void *input,size_t input_bytes,void *output,size_t capacity,size_t *bytes,HRESULT *hr)
{
    struct pw_d3d9_command command;struct pw_d3d9_getter_request getter;
#ifdef PW_D3D9_ENABLE_CURSOR
    struct pw_d3d9_cursor_request cursor;
#endif
#ifdef PW_D3D9_ENABLE_GAMMA
    struct pw_d3d9_gamma_request gamma;
#endif
    *bytes=0;*hr=D3DERR_INVALIDCALL;
    if(opcode==PW_D3D9_COMMAND_CALL){if(pw_d3d9_command_decode(&command,input,input_bytes)!=PW_D3D9_COMMAND_OK)return 0;}
    else if(opcode==PW_D3D9_GETTER_CALL){if(pw_d3d9_getter_decode(&getter,input,input_bytes)!=PW_D3D9_GETTER_OK)return 0;}
#ifdef PW_D3D9_ENABLE_CURSOR
    else if(opcode==PW_D3D9_CURSOR_CALL){if(pw_d3d9_cursor_decode(&cursor,input,input_bytes))return 0;}
#endif
#ifdef PW_D3D9_ENABLE_GAMMA
    else if(opcode==PW_D3D9_GAMMA_CALL){if(pw_d3d9_gamma_request_decode(&gamma,input,input_bytes))return 0;}
#endif
    else return 0;
    const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);
    if(!slot||slot->kind!=PW_D3D9_KIND_DEVICE||!pw_d3d9_object_queue(objects,ref))return 1;
    IDirect3DDevice9 *device=pw_d3d9_native_device_backend((void *)slot->context);int valid;
    if(opcode==PW_D3D9_COMMAND_CALL){
        *hr=pw_d3d9_native_command_dispatch(device,&command,acquire,objects);
        valid=pw_d3d9_command_reply_encode(output,capacity,bytes,command.method,(uint32_t)*hr)==PW_D3D9_COMMAND_OK;
#ifdef PW_D3D9_ENABLE_CURSOR
    }else if(opcode==PW_D3D9_CURSOR_CALL){
        struct pw_d3d9_cursor_reply reply={0};
        *hr=pw_d3d9_native_cursor(device,&cursor,&reply,acquire,objects);
        valid=pw_d3d9_cursor_reply_encode(output,capacity,bytes,&reply)==0;
#endif
#ifdef PW_D3D9_ENABLE_GAMMA
    }else if(opcode==PW_D3D9_GAMMA_CALL){
        struct pw_d3d9_gamma_reply reply;
        pw_d3d9_native_gamma_call(device,&gamma,&reply);*hr=(HRESULT)reply.hresult;
        /* The gamma codec takes an exact record length, not scratch capacity. */
        valid=capacity>=PW_D3D9_GAMMA_REPLY_BYTES &&
              pw_d3d9_gamma_reply_encode(output,PW_D3D9_GAMMA_REPLY_BYTES,&gamma,&reply)==0;
        if(valid)*bytes=PW_D3D9_GAMMA_REPLY_BYTES;
#endif
    }else{
        struct pw_d3d9_getter_reply reply;
        valid=pw_d3d9_native_getter_dispatch(device,&getter,&reply)==PW_D3D9_GETTER_OK;
        if(valid){*hr=(HRESULT)reply.hresult;valid=pw_d3d9_getter_reply_encode(output,capacity,bytes,&getter,&reply)==PW_D3D9_GETTER_OK;}
    }
    if(!pw_d3d9_object_complete(objects,ref))return 0;
    return valid;
}
