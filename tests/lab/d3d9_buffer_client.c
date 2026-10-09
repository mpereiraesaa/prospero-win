/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Controlled transport faults, actual PE32 VirtualAlloc staging. */
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_buffer_client.h"
struct transport {
 unsigned char bytes[16384];uint32_t size,offset,span,written,fail_op,corrupt;
 uint64_t generation;int active,cancels,failures;
};
static uint32_t exchange(void *context,struct pw_d3d9_object_ref ref,const struct pw_d3d9_resource_request *q,struct pw_d3d9_resource_reply *r)
{
 struct transport *t=context;uint32_t hr=S_OK;
 assert(ref.id==7&&ref.generation==9);memset(r,0,sizeof(*r));r->operation=q->operation;
 if(t->fail_op==q->operation)hr=D3DERR_WASSTILLDRAWING;
 else switch(q->operation){
 case PW_D3D9_RESOURCE_LOCK:
  assert(!t->active);t->active=1;t->generation++;t->offset=q->offset;t->span=q->length?q->length:t->size-q->offset;t->written=0;
  r->lock_generation=t->generation;r->length=t->span;break;
 case PW_D3D9_RESOURCE_READ:
  assert(t->active&&q->lock_generation==t->generation&&q->offset+q->count<=t->span);
  r->lock_generation=t->generation+t->corrupt;r->count=q->count;r->offset=q->offset;
  if(t->size<=sizeof(t->bytes))memcpy(r->data,t->bytes+t->offset+q->offset,q->count);
  else memset(r->data,0,q->count);
  break;
 case PW_D3D9_RESOURCE_WRITE:
  assert(t->active&&q->lock_generation==t->generation&&q->offset==t->written);
  assert(t->size<=sizeof(t->bytes));memcpy(t->bytes+t->offset+q->offset,q->data,q->count);t->written+=q->count;break;
 case PW_D3D9_RESOURCE_UNLOCK:assert(t->active);t->active=0;break;
 case PW_D3D9_RESOURCE_CANCEL_LOCK:assert(t->active);t->active=0;t->cancels++;break;
 default:assert(0);
 }
 r->hresult=hr;return hr;
}
static void fail(void *context,uint32_t hr)
{struct transport *t=context;assert(hr&0x80000000u);t->failures++;}
static struct pw_d3d9_buffer_client client(struct transport *t)
{
 struct pw_d3d9_buffer_client c={0};c.context=t;c.call=exchange;c.fail=fail;c.object=(struct pw_d3d9_object_ref){7,9};return c;
}
int main(void)
{
 struct transport t={.size=16384};struct pw_d3d9_buffer_client c=client(&t);void *data=NULL;unsigned i;
 assert(sizeof(void *)==4);
 for(i=0;i<sizeof(t.bytes);i++)t.bytes[i]=(unsigned char)i;
 assert(pw_d3d9_buffer_client_lock(&c,8,8193,0,&data)==S_OK);
 assert(data&&(uintptr_t)data<=UINT32_MAX-8192&&pw_d3d9_buffer_client_staging_bytes()==8193);
 assert(!memcmp(data,t.bytes+8,8193));((unsigned char *)data)[4097]=0x77;
 assert(pw_d3d9_buffer_client_unlock(&c)==S_OK&&t.bytes[8+4097]==0x77&&!t.active&&!c.data);
 assert(pw_d3d9_buffer_client_staging_bytes()==0);
 assert(pw_d3d9_buffer_client_unlock(&c)==(uint32_t)D3DERR_INVALIDCALL);
 assert(pw_d3d9_buffer_client_lock(&c,8,8193,D3DLOCK_READONLY,&data)==S_OK);
 assert(pw_d3d9_buffer_client_unlock(&c)==S_OK&&t.written==0);
 t.fail_op=PW_D3D9_RESOURCE_READ;
 assert(pw_d3d9_buffer_client_lock(&c,0,8193,0,&data)==(uint32_t)D3DERR_WASSTILLDRAWING);
 assert(!data&&!c.data&&!t.active&&t.cancels==1&&!t.failures&&!pw_d3d9_buffer_client_staging_bytes());
 t.fail_op=0;t.corrupt=1;
 assert(pw_d3d9_buffer_client_lock(&c,0,8193,0,&data)==(uint32_t)E_FAIL);
 assert(!data&&!t.active&&t.cancels==2&&t.failures==1&&!pw_d3d9_buffer_client_staging_bytes());
 t.corrupt=0;assert(pw_d3d9_buffer_client_lock(&c,0,8193,0,&data)==S_OK);
 t.fail_op=PW_D3D9_RESOURCE_WRITE;
 assert(pw_d3d9_buffer_client_unlock(&c)==(uint32_t)D3DERR_WASSTILLDRAWING&&!t.active&&!pw_d3d9_buffer_client_staging_bytes());
 t.fail_op=0;
 {struct transport large={.size=PW_D3D9_RESOURCE_MAX_LOCK},second={.size=16};
 struct pw_d3d9_buffer_client a=client(&large),b=client(&second);
 assert(pw_d3d9_buffer_client_lock(&a,0,0,0,&data)==S_OK);
 assert(pw_d3d9_buffer_client_staging_bytes()==PW_D3D9_RESOURCE_MAX_LOCK);
 assert(pw_d3d9_buffer_client_lock(&b,0,0,0,&data)==(uint32_t)E_OUTOFMEMORY);
 assert(!data&&!second.active&&second.cancels==1&&!second.failures);
 assert(pw_d3d9_buffer_client_cancel(&a)==S_OK&&!large.active&&!pw_d3d9_buffer_client_staging_bytes());}
 puts("PW_BUFFER_CLIENT PASS low32=1 copied=1 readonly=1 failure_cleanup=1 budget=1");return 0;
}
