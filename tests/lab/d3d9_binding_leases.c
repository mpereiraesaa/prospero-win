/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_binding_leases.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct resource {IUnknown iface;ULONG refs;uint32_t kind;IDirect3DDevice9 *parent;};
static IDirect3DDevice9 device,foreign_device;
static ULONG device_refs;
static struct resource resources[6];
static struct pw_d3d9_objects objects;
static struct pw_d3d9_object_slot slots[16];
static struct pw_d3d9_object_ref device_ref,refs[6];
static unsigned calls,acquires,native_fail,drop_during_acquire;
static uint32_t special_id;static HRESULT special_hr;
static const unsigned methods[]={65,87,92,100,104,107},kinds[]={5,7,8,3,4,9},words[]={1,0,0,1,0,0};
static HRESULT WINAPI query(IUnknown *p,REFIID iid,void **out){(void)p;(void)iid;if(out)*out=NULL;return E_NOINTERFACE;}
static ULONG WINAPI addref(IUnknown *p){return ++((struct resource *)p)->refs;}
static ULONG WINAPI release(IUnknown *p){struct resource *r=(void *)p;assert(r->refs);return --r->refs;}
static const IUnknownVtbl resource_table={query,addref,release};
static ULONG WINAPI device_addref(IDirect3DDevice9 *p){assert(p==&device);return ++device_refs;}
static ULONG WINAPI device_release(IDirect3DDevice9 *p){assert(p==&device&&device_refs);return --device_refs;}
static HRESULT bound(uint32_t kind,void *p)
{
 assert(slots[device_ref.id-1].queued_refs==1);
 if(p){struct resource *r=p;assert(r->kind==kind&&r->parent==&device&&r->refs>=3);}
 ++calls;return calls==native_fail?D3DERR_DEVICELOST:S_OK;
}
static HRESULT WINAPI texture(IDirect3DDevice9 *d,DWORD stage,IDirect3DBaseTexture9 *p){assert(d==&device&&stage==0);return bound(5,p);}
static HRESULT WINAPI declaration(IDirect3DDevice9 *d,IDirect3DVertexDeclaration9 *p){assert(d==&device);return bound(7,p);}
static HRESULT WINAPI vertex_shader(IDirect3DDevice9 *d,IDirect3DVertexShader9 *p){assert(d==&device);return bound(8,p);}
static HRESULT WINAPI stream(IDirect3DDevice9 *d,UINT index,IDirect3DVertexBuffer9 *p,UINT offset,UINT stride){assert(d==&device&&index==15&&offset==UINT32_MAX&&stride==UINT32_MAX);return bound(3,p);}
static HRESULT WINAPI indices(IDirect3DDevice9 *d,IDirect3DIndexBuffer9 *p){assert(d==&device);return bound(4,p);}
static HRESULT WINAPI pixel_shader(IDirect3DDevice9 *d,IDirect3DPixelShader9 *p){assert(d==&device);return bound(9,p);}
static HRESULT WINAPI render_state(IDirect3DDevice9 *d,D3DRENDERSTATETYPE state,DWORD value){assert(d==&device&&state==7&&value==1);return bound(0,NULL);}
static const IDirect3DDevice9Vtbl device_table={.AddRef=device_addref,.Release=device_release,.SetTexture=texture,.SetVertexDeclaration=declaration,.SetVertexShader=vertex_shader,.SetStreamSource=stream,.SetIndices=indices,.SetPixelShader=pixel_shader,.SetRenderState=render_state};
static void setup(void)
{
 calls=acquires=native_fail=special_id=drop_during_acquire=0;special_hr=S_OK;device_refs=1;device.lpVtbl=(IDirect3DDevice9Vtbl *)&device_table;
 assert(pw_d3d9_objects_init(&objects,slots,16,1,9));assert(pw_d3d9_object_reserve(&objects,&device_ref));
 assert(pw_d3d9_object_commit(&objects,device_ref,(uintptr_t)&device,(uintptr_t)&device,2));assert(pw_d3d9_object_queue(&objects,device_ref));
 for(unsigned i=0;i<6;i++){
  resources[i]=(struct resource){{(IUnknownVtbl *)&resource_table},1,kinds[i],&device};
  assert(pw_d3d9_object_reserve(&objects,refs+i));assert(pw_d3d9_object_commit(&objects,refs[i],(uintptr_t)(resources+i),(uintptr_t)(resources+i),kinds[i]));
 }
}
static void finish(void)
{
 for(unsigned i=0;i<6;i++)assert(resources[i].refs==1&&!slots[refs[i].id-1].queued_refs);
 assert(device_refs==1&&pw_d3d9_object_complete(&objects,device_ref));pw_d3d9_objects_cancel(&objects);
 for(unsigned i=0;i<7;i++){
  struct pw_d3d9_object_ref ref=i==6?device_ref:refs[i];uintptr_t dead;
  assert(pw_d3d9_object_take_destroy(&objects,ref,&dead));assert(IUnknown_Release((IUnknown *)dead)==0);assert(pw_d3d9_object_finish_destroy(&objects,ref));
 }
 assert(!device_refs);
}
static HRESULT acquire(void *context,uint32_t id,uint32_t generation,uint32_t kind,IDirect3DDevice9 *d,void **out)
{
 assert(context==&objects);++acquires;*out=NULL;
 if(drop_during_acquire&&acquires==1){uintptr_t dead;assert(pw_d3d9_object_release(&objects,refs[5]));assert(!pw_d3d9_object_take_destroy(&objects,refs[5],&dead));}
 struct pw_d3d9_object_ref ref={id,generation};const struct pw_d3d9_object_slot *s=pw_d3d9_object_lookup(&objects,objects.device,objects.epoch,ref);
 if(!s||s->kind!=kind)return D3DERR_INVALIDCALL;
 struct resource *r=(void *)s->context;if(r->parent!=d)return D3DERR_INVALIDCALL;
 if(id==special_id&&FAILED(special_hr))return special_hr;
 IUnknown_AddRef(&r->iface);*out=&r->iface;return id==special_id?special_hr:S_OK;
}
static size_t make(unsigned char *wire,int nulls,int bad_index,int stale,int ineligible)
{
 struct pw_d3d9_command_batch b;size_t bytes;assert(!pw_d3d9_batch_init(&b,1));
 struct pw_d3d9_command scalar={.method=57,.args={7,1}};assert(!pw_d3d9_batch_append(&b,&scalar));
 for(unsigned i=0;i<7;i++){
  unsigned k=i%6;struct pw_d3d9_command c={.method=methods[k]};
  if(!nulls){c.args[words[k]]=refs[k].id;c.args[words[k]+1]=refs[k].generation+(stale&&(int)k==bad_index);}
  if(k==3){c.args[0]=15;c.args[3]=c.args[4]=UINT32_MAX;}
  if(ineligible&&i==6)c=(struct pw_d3d9_command){.method=41};
  assert(!pw_d3d9_batch_append(&b,&c));
 }
 assert(!pw_d3d9_batch_encode(wire,PW_D3D9_BATCH_MAX,&bytes,&b));return bytes;
}
static HRESULT execute(struct pw_d3d9_binding_leases *l,unsigned *attempted)
{
 *attempted=0;
 for(unsigned i=0;i<l->batch.count;i++){
  struct pw_d3d9_command c;assert(!pw_d3d9_batch_command(&c,&l->batch,i));
  HRESULT hr=pw_d3d9_native_command_dispatch(&device,&c,pw_d3d9_binding_lease_acquire,l->records+i);++*attempted;if(hr!=S_OK)return hr;
 }
 return S_OK;
}
int main(void)
{
 unsigned char wire[PW_D3D9_BATCH_MAX];struct pw_d3d9_binding_leases leases={0};size_t bytes;unsigned attempted;
 setup();bytes=make(wire,0,-1,0,0);assert(pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects));
 assert(acquires==7&&resources[0].refs==3&&slots[refs[0].id-1].queued_refs==2);
 assert(!pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects)&&acquires==7);
 for(unsigned i=0;i<6;i++){uintptr_t dead;assert(pw_d3d9_object_release(&objects,refs[i]));assert(!pw_d3d9_object_take_destroy(&objects,refs[i],&dead));}
 memset(wire,0,bytes);assert(execute(&leases,&attempted)==S_OK&&attempted==8&&calls==8);
 void *out=(void *)1;assert(pw_d3d9_binding_lease_acquire(leases.records+1,refs[0].id,refs[0].generation,5,&foreign_device,&out)==D3DERR_INVALIDCALL&&!out);
 assert(pw_d3d9_binding_leases_release(&leases));assert(!pw_d3d9_binding_leases_release(&leases));finish();
 /* Every binding kind: stale generation, wrong kind and cross-device target. */
 for(unsigned kind=0;kind<6;kind++)for(unsigned mode=0;mode<3;mode++){
  setup();bytes=make(wire,0,(int)kind,mode==0,0);
  if(mode==1)slots[refs[kind].id-1].kind=1;
  if(mode==2)resources[kind].parent=&foreign_device;
  assert(pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects));
  assert(execute(&leases,&attempted)==D3DERR_INVALIDCALL&&attempted==kind+2&&calls==kind+1);
  assert(pw_d3d9_binding_leases_release(&leases));finish();
 }
 for(unsigned failure=1;failure<=8;failure++){
  setup();native_fail=failure;bytes=make(wire,0,-1,0,0);assert(pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects));
  assert(execute(&leases,&attempted)==D3DERR_DEVICELOST&&attempted==failure&&calls==failure);
  assert(pw_d3d9_binding_leases_release(&leases));finish();
 }
 for(unsigned mode=0;mode<2;mode++){
  setup();special_id=refs[1].id;special_hr=mode?S_FALSE:E_OUTOFMEMORY;bytes=make(wire,0,-1,0,0);
  assert(pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects));
  assert(execute(&leases,&attempted)==(mode?E_FAIL:E_OUTOFMEMORY)&&attempted==3&&calls==2);
  assert(leases.records[2].unexpected_result==(mode?(uint32_t)S_FALSE:0));assert(pw_d3d9_binding_leases_release(&leases));finish();
 }
 setup();drop_during_acquire=1;bytes=make(wire,0,-1,0,0);assert(pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects));
 assert(execute(&leases,&attempted)==S_OK&&calls==8);assert(pw_d3d9_binding_leases_release(&leases));finish();
 setup();bytes=make(wire,1,-1,0,0);assert(pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects)&&!acquires);
 assert(execute(&leases,&attempted)==S_OK&&calls==8);assert(pw_d3d9_binding_leases_release(&leases));finish();
 setup();bytes=make(wire,0,-1,0,1);assert(!pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects)&&!calls&&!acquires&&!leases.prepared);finish();
 setup();bytes=make(wire,0,-1,0,0);wire[bytes-1]=1;assert(!pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects)&&!acquires);finish();
 setup();bytes=make(wire,0,-1,0,0);assert(pw_d3d9_binding_leases_prepare(&leases,&objects,&device,wire,bytes,acquire,&objects));pw_d3d9_objects_cancel(&objects);
 assert(pw_d3d9_binding_leases_release(&leases)&&!calls);finish();
 puts("BINDING_LEASES PASS owned=1 typed=1 prefix=1 release_zero=1 duplicate=1 cancellation=1 all_native_refs_zero=1");return 0;
}
