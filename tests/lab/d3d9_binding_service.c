/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define main leases_baseline_main
#include "d3d9_binding_leases.c"
#undef main
#include "../../wine/ps5/d3d9/pw_d3d9_service_methods.h"
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include "../../wine/ps5/d3d9/pw_d3d9_service_resource.h"
#include "../../wine/ps5/d3d9/pw_d3d9_native_getter.h"
static unsigned char *mutate_input;static size_t mutate_bytes;static int release_all;
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *p)
{assert((void *)p==&device);return &device;}
HRESULT pw_d3d9_service_resource_acquire(struct pw_d3d9_objects *o,struct pw_d3d9_object_ref ref,uint32_t kind,void *d,void **out)
{
 HRESULT hr=acquire(o,ref.id,ref.generation,kind,d,out);
 if(acquires==1){
  if(mutate_input)memset(mutate_input,0xa7,mutate_bytes);
  if(release_all)for(unsigned i=0;i<6;i++){uintptr_t dead;assert(pw_d3d9_object_release(o,refs[i]));assert(!pw_d3d9_object_take_destroy(o,refs[i],&dead));}
 }
 return hr;
}
int pw_d3d9_native_getter_dispatch(IDirect3DDevice9 *d,const struct pw_d3d9_getter_request *q,struct pw_d3d9_getter_reply *r)
{(void)d;(void)q;(void)r;assert(0);return PW_D3D9_GETTER_INVALID;}
static void start(struct pw_d3d9_service_batch_state *s,int enabled)
{setup();assert(pw_d3d9_object_complete(&objects,device_ref));pw_d3d9_service_batch_init(s);assert(pw_d3d9_service_batch_bindings(s,enabled));mutate_input=NULL;mutate_bytes=0;release_all=0;}
static void end(void)
{assert(!slots[device_ref.id-1].queued_refs);assert(pw_d3d9_object_queue(&objects,device_ref));finish();}
int main(void)
{
 assert(!leases_baseline_main());
 struct pw_d3d9_service_batch_state state;struct pw_d3d9_batch_reply reply;
 unsigned char wire[PW_D3D9_BATCH_MAX],output[PW_D3D9_BATCH_REPLY];size_t n,bytes;HRESULT hr;
 start(&state,0);n=make(wire,0,-1,0,0);
 assert(!pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,output,sizeof(output),&bytes,&hr)&&!bytes&&!calls&&!acquires);end();
 start(&state,1);n=make(wire,0,-1,0,1);
 assert(!pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,output,sizeof(output),&bytes,&hr)&&!calls&&!acquires);end();
 start(&state,1);n=make(wire,0,-1,0,0);mutate_input=wire;mutate_bytes=n;release_all=1;
 assert(pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,output,sizeof(output),&bytes,&hr)&&hr==S_OK&&calls==8);
 assert(!pw_d3d9_batch_reply_decode(&reply,output,bytes,1,8)&&reply.attempted==8&&state.next_sequence==9);
 assert(!pw_d3d9_service_batch_bindings(&state,0));end();
 for(unsigned failure=1;failure<=8;failure++){
  start(&state,1);n=make(wire,0,-1,0,0);native_fail=failure;
  assert(pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,output,sizeof(output),&bytes,&hr)&&hr==D3DERR_DEVICELOST&&calls==failure);
  assert(!pw_d3d9_batch_reply_decode(&reply,output,bytes,1,8)&&reply.attempted==failure&&reply.failed_index==failure-1&&state.failed_sequence==failure);
  assert(!pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,output,sizeof(output),&bytes,&hr)&&calls==failure);end();
 }
 for(unsigned k=0;k<6;k++){
  start(&state,1);n=make(wire,0,(int)k,1,0);
  assert(pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,output,sizeof(output),&bytes,&hr)&&hr==D3DERR_INVALIDCALL&&calls==k+1);
  assert(!pw_d3d9_batch_reply_decode(&reply,output,bytes,1,8)&&reply.attempted==k+2&&reply.failed_index==k+1);end();
 }
 start(&state,1);n=make(wire,0,-1,0,0);special_id=refs[0].id;special_hr=S_FALSE;
 assert(!pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,output,sizeof(output),&bytes,&hr)&&state.unexpected_result==S_FALSE&&state.failed_sequence==2&&calls==1);end();
 start(&state,1);n=make(wire,1,-1,0,0);
 assert(pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,output,sizeof(output),&bytes,&hr)&&hr==S_OK&&calls==8&&!acquires);end();
 start(&state,1);n=make(wire,0,-1,0,0);struct pw_d3d9_command_batch b;assert(!pw_d3d9_batch_decode(&b,wire,n));b.first_sequence=UINT64_MAX-7;state.next_sequence=b.first_sequence;
 assert(!pw_d3d9_batch_encode(wire,sizeof(wire),&n,&b));
 assert(pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,output,sizeof(output),&bytes,&hr)&&hr==S_OK&&state.exhausted&&calls==8);end();
 puts("PW_BINDING_SERVICE negotiated=1 owned_input=1 six_kinds=1 prefix=1 sticky=1 cleanup=1 exhausted=1 status=0");return 0;
}
