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
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
#include "../../wine/ps5/d3d9/pw_d3d9_native_draw_state.h"
static unsigned recording,getter_calls;static int live_decl,getter_release;static HRESULT getter_result;
static unsigned char *getter_mutate;static size_t getter_mutate_bytes;
unsigned pw_d3d9_native_device_recording(struct pw_d3d9_native_device *p)
{assert((void *)p==&device);return recording;}
static HRESULT WINAPI get_declaration(IDirect3DDevice9 *d,IDirect3DVertexDeclaration9 **out)
{
 assert(d==&device&&slots[device_ref.id-1].queued_refs==1);++getter_calls;
 if(getter_mutate)memset(getter_mutate,0x91,getter_mutate_bytes);
 if(getter_release){
  assert(slots[refs[1].id-1].queued_refs);
  for(unsigned i=0;i<6;i++)assert(pw_d3d9_object_release(&objects,refs[i]));
 }
 *out=NULL;if(live_decl){IUnknown_AddRef(&resources[1].iface);*out=(void *)&resources[1];}
 return getter_result;
}
static HRESULT WINAPI set_declaration(IDirect3DDevice9 *d,IDirect3DVertexDeclaration9 *p)
{
 HRESULT hr=declaration(d,p);
 if(hr==S_OK&&recording==PW_D3D9_RECORDING_LIVE)live_decl=p!=NULL;
 return hr;
}
static HRESULT WINAPI draw_primitive(IDirect3DDevice9 *d,D3DPRIMITIVETYPE type,UINT first,UINT count)
{
 assert(d==&device&&type==D3DPT_TRIANGLELIST&&!first&&!count&&live_decl);
 return bound(0,NULL);
}
static IDirect3DDevice9Vtbl draw_table;
static size_t make_draw(unsigned char *wire,int null_decl,int stale,int malformed)
{
 struct pw_d3d9_command_batch b;size_t bytes;assert(!pw_d3d9_batch_init(&b,1));
 struct pw_d3d9_command c={.method=57,.args={7,1}};assert(!pw_d3d9_batch_append(&b,&c));
 c=(struct pw_d3d9_command){.method=87};
 if(!null_decl){c.args[0]=refs[1].id;c.args[1]=refs[1].generation+stale;}
 assert(!pw_d3d9_batch_append(&b,&c));
 c=(struct pw_d3d9_command){.method=81,.args={D3DPT_TRIANGLELIST,0,0}};
 if(malformed)c.args[0]=0;
 assert(!pw_d3d9_batch_append(&b,&c));assert(!pw_d3d9_batch_encode(wire,PW_D3D9_BATCH_MAX,&bytes,&b));return bytes;
}
#endif
static void start(struct pw_d3d9_service_batch_state *s,int enabled)
{setup();assert(pw_d3d9_object_complete(&objects,device_ref));pw_d3d9_service_batch_init(s);assert(pw_d3d9_service_batch_bindings(s,enabled));mutate_input=NULL;mutate_bytes=0;release_all=0;}
static void end(void)
{assert(!slots[device_ref.id-1].queued_refs);assert(pw_d3d9_object_queue(&objects,device_ref));finish();}
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
static void draw_controls(void)
{
 struct pw_d3d9_service_batch_state state;unsigned char wire[PW_D3D9_BATCH_MAX],out[PW_D3D9_BATCH_REPLY];size_t n,bytes;HRESULT hr;
 for(unsigned mode=0;mode<9;mode++){
  start(&state,1);assert(pw_d3d9_service_batch_draws(&state,mode!=8));
  recording=mode==1||mode==2?PW_D3D9_RECORDING_ACTIVE:PW_D3D9_RECORDING_LIVE;
  if(mode==7)recording=PW_D3D9_RECORDING_UNKNOWN;
  live_decl=mode==2;getter_calls=getter_release=0;getter_result=mode==5?S_FALSE:S_OK;
  getter_mutate=NULL;getter_mutate_bytes=0;
  draw_table=device_table;draw_table.GetVertexDeclaration=get_declaration;
  draw_table.SetVertexDeclaration=set_declaration;draw_table.DrawPrimitive=draw_primitive;device.lpVtbl=&draw_table;
  n=make_draw(wire,mode==2,mode==3,mode==6);
  if(mode==4){getter_mutate=wire;getter_mutate_bytes=n;getter_release=1;}
  int valid=pw_d3d9_service_batch(&state,&objects,device_ref,wire,n,out,sizeof(out),&bytes,&hr);
  if(mode==0||mode==2||mode==4)assert(valid&&hr==S_OK&&calls==3&&getter_calls==1);
  else if(mode==3){struct pw_d3d9_batch_reply reply;assert(valid&&hr==D3DERR_INVALIDCALL&&calls==1);
   assert(!pw_d3d9_batch_reply_decode(&reply,out,bytes,1,3)&&reply.attempted==2&&reply.failed_index==1);}
  else assert(!valid&&!bytes&&!calls);
  if(mode==6||mode==8)assert(!getter_calls&&!acquires);
  if(mode==7)assert(!getter_calls);
  end();
 }
 puts("PW_DRAW_SERVICE live=1 recording=1 stale_prefix=1 owned_input=1 pins=1 getter_sfalse=1 unknown=1 malformed=1 negotiation=1 status=0");
}
#endif
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
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
 draw_controls();
#endif
 puts("PW_BINDING_SERVICE negotiated=1 owned_input=1 six_kinds=1 prefix=1 sticky=1 cleanup=1 exhausted=1 status=0");return 0;
}
