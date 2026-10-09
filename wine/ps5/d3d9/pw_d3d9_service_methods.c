#define COBJMACROS
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
#ifdef PW_D3D9_ENABLE_BATCH
#include "../pw_d3d9_command_batch.h"
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
#include "pw_d3d9_binding_leases.h"
#endif
#include "../pw_d3d9_command_policy.h"
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
#ifndef PW_D3D9_ENABLE_BINDING_TICKETS
#error Draw batches require prepared binding leases
#endif
#include "pw_d3d9_native_draw_state.h"
#include "../pw_d3d9_draw_shadow.h"
#endif
#include <string.h>
#endif
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
#ifdef PW_D3D9_ENABLE_BATCH
void pw_d3d9_service_batch_init(struct pw_d3d9_service_batch_state *state)
{
    memset(state,0,sizeof(*state));state->next_sequence=1;
}
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
int pw_d3d9_service_batch_bindings(struct pw_d3d9_service_batch_state *s,int enabled)
{
    if(!s||s->next_sequence!=1||s->failed_result||s->exhausted)return 0;
    s->bindings=!!enabled;return 1;
}
#endif
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
int pw_d3d9_service_batch_draws(struct pw_d3d9_service_batch_state *s,int enabled)
{
    if(!s||!s->bindings||s->next_sequence!=1||s->failed_result||s->exhausted)return 0;
    s->draws=!!enabled;return 1;
}
static int batch_draw_state(struct pw_d3d9_native_device *context,IDirect3DDevice9 *device,
 const struct pw_d3d9_command_batch *batch,const struct pw_d3d9_binding_leases *leases)
{
    struct pw_d3d9_draw_shadow shadow;pw_d3d9_draw_init(&shadow);
    unsigned recording=pw_d3d9_native_device_recording(context);
    if(recording!=PW_D3D9_RECORDING_LIVE&&recording!=PW_D3D9_RECORDING_ACTIVE)return 0;
    shadow.recording=recording==PW_D3D9_RECORDING_ACTIVE;
    IDirect3DVertexDeclaration9 *declaration=NULL;
    HRESULT hr=IDirect3DDevice9_GetVertexDeclaration(device,&declaration);
    if(hr==S_OK)pw_d3d9_draw_observe(&shadow,declaration!=NULL,0);
    if(declaration)IDirect3DVertexDeclaration9_Release(declaration);
    if(hr!=S_OK)return 0;
    for(uint32_t n=0;n<batch->count;n++){
        struct pw_d3d9_command command;
        if(pw_d3d9_batch_command(&command,batch,n))return 0;
        /* A saved binding failure stops execution at this position. Never
         * manufacture a new declaration from an unresolved object. */
        if(leases->records[n].result!=S_OK)break;
        if(command.method==87)pw_d3d9_draw_declaration(&shadow,command.args[0]!=0,0);
        else if(command.method==89)pw_d3d9_draw_fvf(&shadow,command.args[0],0);
        else if((command.method==81||command.method==82)&&!pw_d3d9_draw_can_queue(&shadow,&command))return 0;
    }
    return 1;
}
#endif
static void batch_fail(struct pw_d3d9_service_batch_state *s,uint64_t sequence,uint32_t hr)
{
    if(!s->failed_result){s->failed_sequence=sequence;s->failed_result=hr;}
}
int pw_d3d9_service_batch(struct pw_d3d9_service_batch_state *state,
 struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,
 const void *input,size_t input_bytes,void *output,size_t capacity,size_t *bytes,HRESULT *hr)
{
    struct pw_d3d9_command_batch batch;struct pw_d3d9_command command;
    struct pw_d3d9_batch_reply reply;IDirect3DDevice9 *device=NULL;int pinned=0,valid=0;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    struct pw_d3d9_binding_leases leases;int leased=0;
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
    int has_draw=0;
#endif
#endif
    if(!bytes||!hr)return 0;
    *bytes=0;*hr=E_FAIL;
    if(!state||!objects||state->failed_result||state->exhausted)return 0;
    if(!output||capacity<PW_D3D9_BATCH_REPLY||
       pw_d3d9_batch_decode(&batch,input,input_bytes)||batch.first_sequence!=state->next_sequence)
        goto protocol_failure;
    /* Validate eligibility for every record before the first native call. */
    for(uint32_t n=0;n<batch.count;n++){
        if(pw_d3d9_batch_command(&command,&batch,n))goto protocol_failure;
        int eligible=pw_d3d9_command_can_queue(&command);
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
        if(!eligible&&state->bindings){struct pw_d3d9_binding binding;
            eligible=pw_d3d9_binding_plan(&command,&binding)==PW_D3D9_BINDING_READY;}
#endif
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
        if(!eligible&&state->draws){
            const struct pw_d3d9_draw_shadow shape={.active=PW_D3D9_DECL_PRESENT};
            eligible=pw_d3d9_draw_can_queue(&shape,&command);
            if(eligible)has_draw=1;
        }
#endif
        if(!eligible)goto protocol_failure;
    }
    reply=(struct pw_d3d9_batch_reply){batch.first_sequence,batch.count,0,UINT32_MAX,0};
    const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);
    if(slot&&slot->kind==PW_D3D9_KIND_DEVICE&&pw_d3d9_object_queue(objects,ref)){
        pinned=1;device=pw_d3d9_native_device_backend((void *)slot->context);
    }
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(state->bindings&&device){
        /* Re-encode only our immutable decoded snapshot. Do not reread shared
         * input after native accessor/acquire callbacks can reenter. */
        unsigned char owned[PW_D3D9_BATCH_MAX];size_t owned_bytes;
        memset(&leases,0,sizeof(leases));
        if(pw_d3d9_batch_encode(owned,sizeof(owned),&owned_bytes,&batch)||
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
           !pw_d3d9_binding_leases_prepare_draws(&leases,objects,device,owned,owned_bytes,acquire,objects,state->draws)
#else
           !pw_d3d9_binding_leases_prepare(&leases,objects,device,owned,owned_bytes,acquire,objects)
#endif
           )goto protocol_failure;
        leased=1;
    }
#endif
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
    if(has_draw&&(!leased||!device||!batch_draw_state((void *)slot->context,device,&batch,&leases)))goto protocol_failure;
#endif
    for(uint32_t n=0;n<batch.count;n++){
        uint64_t sequence=batch.first_sequence+n;
        /* The owned batch was fully checked above; this cannot inspect caller
         * or shared memory again while native execution is in progress. */
        if(pw_d3d9_batch_command(&command,&batch,n))goto protocol_failure;
        HRESULT result;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
        if(leased)result=pw_d3d9_native_command_dispatch(device,&command,pw_d3d9_binding_lease_acquire,leases.records+n);
        else
#endif
        result=device?pw_d3d9_native_command_dispatch(device,&command,acquire,objects):D3DERR_INVALIDCALL;
        reply.attempted=n+1;
        if(sequence==UINT64_MAX)state->exhausted=1;
        else state->next_sequence=sequence+1;
        if(result!=S_OK){
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
            if(leased&&leases.records[n].unexpected_result){
                state->unexpected_result=leases.records[n].unexpected_result;
                batch_fail(state,sequence,(uint32_t)E_FAIL);goto protocol_failure;
            }
#endif
            if(SUCCEEDED(result)){
                state->unexpected_result=(uint32_t)result;
                batch_fail(state,sequence,(uint32_t)E_FAIL);goto protocol_failure;
            }
            reply.failed_index=n;reply.hresult=(uint32_t)result;
            batch_fail(state,sequence,(uint32_t)result);break;
        }
    }
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(leased){leased=0;if(!pw_d3d9_binding_leases_release(&leases))goto protocol_failure;}
#endif
    if(pinned){pinned=0;if(!pw_d3d9_object_complete(objects,ref))goto protocol_failure;}
    if(pw_d3d9_batch_reply_encode(output,capacity,&reply))goto protocol_failure;
    *bytes=PW_D3D9_BATCH_REPLY;*hr=(HRESULT)reply.hresult;valid=1;
    return valid;
 protocol_failure:
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(leased)pw_d3d9_binding_leases_release(&leases);
#endif
    if(pinned)pw_d3d9_object_complete(objects,ref);
    batch_fail(state,state->next_sequence,(uint32_t)E_FAIL);
    return 0;
}
#endif
