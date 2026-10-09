/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../wine/ps5/d3d9/pw_d3d9_service_methods.h"
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include "../../wine/ps5/d3d9/pw_d3d9_service_resource.h"
#include "../../wine/ps5/d3d9/pw_d3d9_native_command.h"
#include "../../wine/ps5/d3d9/pw_d3d9_native_getter.h"
#include "../../wine/ps5/pw_d3d9_command_batch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static IDirect3DDevice9 device;
static struct pw_d3d9_objects objects;
static struct pw_d3d9_object_slot slots[8];
static struct pw_d3d9_object_ref ref;
static unsigned calls,fail_at,release_at,seen[128];
static unsigned char *mutate_input;
static size_t mutate_bytes;
static HRESULT failure;
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *p)
{assert((void *)p==&device);return &device;}
HRESULT pw_d3d9_service_resource_acquire(struct pw_d3d9_objects *o,struct pw_d3d9_object_ref r,uint32_t kind,void *d,void **out)
{(void)o;(void)r;(void)kind;(void)d;(void)out;assert(0);return E_FAIL;}
int pw_d3d9_native_getter_dispatch(IDirect3DDevice9 *d,const struct pw_d3d9_getter_request *q,struct pw_d3d9_getter_reply *r)
{(void)d;(void)q;(void)r;assert(0);return PW_D3D9_GETTER_INVALID;}
HRESULT pw_d3d9_native_command_dispatch(IDirect3DDevice9 *d,const struct pw_d3d9_command *q,pw_d3d9_command_acquire_fn acquire,void *context)
{
    (void)acquire;assert(context==&objects&&d==&device&&q->method==57);
    assert(slots[ref.id-1].queued_refs==1);seen[calls++]=q->args[1];
    if(calls==1&&mutate_input)memset(mutate_input,0xa7,mutate_bytes);
    if(calls==release_at){uintptr_t dead;assert(pw_d3d9_object_release(&objects,ref));assert(!pw_d3d9_object_take_destroy(&objects,ref,&dead));}
    return calls==fail_at?failure:S_OK;
}
static void setup(struct pw_d3d9_service_batch_state *state,uint32_t kind)
{
    calls=fail_at=release_at=0;mutate_input=NULL;mutate_bytes=0;failure=S_OK;memset(seen,0,sizeof(seen));
    assert(pw_d3d9_objects_init(&objects,slots,8,1,7));assert(pw_d3d9_object_reserve(&objects,&ref));
    assert(pw_d3d9_object_commit(&objects,ref,(uintptr_t)&device,(uintptr_t)&device,kind));
    pw_d3d9_service_batch_init(state);
}
static size_t make(unsigned char *wire,uint64_t first,unsigned count,int ineligible)
{
    struct pw_d3d9_command_batch b;size_t bytes;assert(!pw_d3d9_batch_init(&b,first));
    for(unsigned n=0;n<count;n++){
        struct pw_d3d9_command q={.method=57,.args={7,n+1}};
        if(ineligible&&n==count-1)q=(struct pw_d3d9_command){.method=41};
        assert(!pw_d3d9_batch_append(&b,&q));
    }
    assert(!pw_d3d9_batch_encode(wire,PW_D3D9_BATCH_MAX,&bytes,&b));return bytes;
}
int main(void)
{
    unsigned char wire[PW_D3D9_BATCH_MAX],output[32],before[32];size_t bytes,n;
    struct pw_d3d9_service_batch_state state;struct pw_d3d9_batch_reply reply;HRESULT hr;
    setup(&state,PW_D3D9_KIND_DEVICE);n=make(wire,1,3,0);
    assert(pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,sizeof(output),&bytes,&hr));
    assert(bytes==32&&hr==S_OK&&calls==3&&state.next_sequence==4&&!state.failed_result);
    assert(!pw_d3d9_batch_reply_decode(&reply,output,bytes,1,3)&&reply.attempted==3);
    assert(seen[0]==1&&seen[1]==2&&seen[2]==3&&!slots[ref.id-1].queued_refs);
    n=make(wire,4,2,0);assert(pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr)&&calls==5&&state.next_sequence==6);
    assert(!pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr)&&calls==5&&state.failed_result==(uint32_t)E_FAIL);
    setup(&state,PW_D3D9_KIND_DEVICE);n=make(wire,2,3,0);
    assert(!pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr)&&!calls&&state.failed_sequence==1);
    setup(&state,PW_D3D9_KIND_DEVICE);n=make(wire,1,3,0);mutate_input=wire;mutate_bytes=n;
    assert(pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr)&&calls==3&&hr==S_OK);
    assert(seen[0]==1&&seen[1]==2&&seen[2]==3);
    /* Every failure prefix is exact, sticky, and unpins the device. */
    for(unsigned index=1;index<=3;index++){
        setup(&state,PW_D3D9_KIND_DEVICE);n=make(wire,1,3,0);fail_at=index;failure=D3DERR_DEVICELOST;
        assert(pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr));
        assert(hr==failure&&calls==index&&state.failed_sequence==index&&state.failed_result==(uint32_t)failure);
        assert(!pw_d3d9_batch_reply_decode(&reply,output,bytes,1,3)&&reply.attempted==index&&reply.failed_index==index-1);
        assert(!slots[ref.id-1].queued_refs);
        assert(!pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr)&&calls==index);
    }
    /* All policy and byte validation precedes execution, including last record. */
    for(unsigned mode=0;mode<4;mode++){
        setup(&state,PW_D3D9_KIND_DEVICE);n=make(wire,1,3,mode==0);
        if(mode==1){struct pw_d3d9_command_batch b;assert(!pw_d3d9_batch_decode(&b,wire,n));wire[PW_D3D9_BATCH_HEADER+b.offsets[2]+4]=0xff;}
        if(mode==2)n--;
        memset(output,0xac,sizeof(output));memcpy(before,output,sizeof(output));
        assert(!pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,mode==3?31:32,&bytes,&hr));
        assert(!calls&&!bytes&&state.failed_result&&!memcmp(output,before,sizeof(output))&&!slots[ref.id-1].queued_refs);
    }
    setup(&state,PW_D3D9_KIND_DEVICE);n=make(wire,1,3,0);fail_at=2;failure=S_FALSE;
    assert(!pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr));
    assert(calls==2&&!bytes&&state.failed_sequence==2&&state.unexpected_result==S_FALSE&&!slots[ref.id-1].queued_refs);
    setup(&state,PW_D3D9_KIND_DEVICE);n=make(wire,1,3,0);release_at=1;
    assert(pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr)&&hr==S_OK&&calls==3);
    uintptr_t dead;assert(pw_d3d9_object_take_destroy(&objects,ref,&dead)&&dead==(uintptr_t)&device);assert(pw_d3d9_object_finish_destroy(&objects,ref));
    for(unsigned wrong=0;wrong<2;wrong++){
        setup(&state,wrong?PW_D3D9_KIND_DEVICE:PW_D3D9_KIND_FACTORY);n=make(wire,1,3,0);
        struct pw_d3d9_object_ref bad=ref;if(wrong)bad.generation++;
        assert(pw_d3d9_service_batch(&state,&objects,bad,wire,n,output,32,&bytes,&hr)&&hr==D3DERR_INVALIDCALL&&!calls);
        assert(!pw_d3d9_batch_reply_decode(&reply,output,bytes,1,3)&&reply.attempted==1&&reply.failed_index==0);
    }
    setup(&state,PW_D3D9_KIND_DEVICE);state.next_sequence=UINT64_MAX;n=make(wire,UINT64_MAX,1,0);
    assert(pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr)&&state.exhausted&&calls==1);
    assert(!pw_d3d9_service_batch(&state,&objects,ref,wire,n,output,32,&bytes,&hr)&&calls==1);
    puts("BATCH_SERVICE PASS ordered=1 first_failure=1 sticky=1 prevalidated=1 lifetime=1 exhaustion=1");return 0;
}
