/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_texture_client.h"
#include "../../wine/ps5/d3d9/pw_d3d9_staging.h"
struct transport {unsigned char bytes[17156];int32_t pitch;uint64_t generation;unsigned active,cancels,failures,written,fail_op,corrupt;};
static uint32_t exchange(void *context,struct pw_d3d9_object_ref ref,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *r)
{
 struct transport *t=context;uint32_t hr=S_OK;assert(ref.id==7&&ref.generation==9);memset(r,0,sizeof(*r));r->operation=q->operation;
 if(t->fail_op==q->operation)hr=D3DERR_WASSTILLDRAWING;
 else switch(q->operation){
 case PW_D3D9_TEXTURE_LOCK:
  assert(!t->active);t->active=1;t->written=0;r->lock_generation=++t->generation;
  r->pitch=t->corrupt?1:t->pitch;r->rows=33;r->row_bytes=516;r->length=sizeof(t->bytes);break;
 case PW_D3D9_TEXTURE_READ:
  assert(t->active&&q->lock_generation==t->generation&&q->offset+q->count<=sizeof(t->bytes));
  r->lock_generation=t->generation;r->offset=q->offset;r->count=q->count;memcpy(r->data,t->bytes+q->offset,q->count);break;
 case PW_D3D9_TEXTURE_WRITE:
  assert(t->active&&q->lock_generation==t->generation&&q->offset==t->written&&q->offset+q->count<=sizeof(t->bytes));
  memcpy(t->bytes+q->offset,q->data,q->count);t->written+=q->count;break;
 case PW_D3D9_TEXTURE_UNLOCK:assert(t->active);t->active=0;break;
 case PW_D3D9_TEXTURE_CANCEL_LOCK:assert(t->active);t->active=0;t->cancels++;break;
 default:assert(0);
 }
 r->hresult=hr;return hr;
}
static void fail(void *context,uint32_t hr)
{struct transport *t=context;assert(hr&0x80000000u);t->failures++;}
static void scenario(int32_t pitch)
{
 struct transport t={.pitch=pitch};struct pw_d3d9_texture_client c={.context=&t,.call=exchange,.fail=fail,.object={7,9}};
 struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_LOCK};int32_t returned_pitch;void *bits=NULL,*held=NULL;
 for(unsigned i=0;i<sizeof(t.bytes);i++)t.bytes[i]=(unsigned char)i;
 assert(pw_d3d9_texture_client_lock(&c,&q,&returned_pitch,&bits)==S_OK&&returned_pitch==pitch&&bits);
 assert((uintptr_t)c.data<=UINT32_MAX-sizeof(t.bytes)+1&&pw_d3d9_staging_bytes()==sizeof(t.bytes));
 unsigned first=pitch<0?520*32:0;assert(!memcmp(bits,t.bytes+first,516));((unsigned char *)bits)[13]=0x77;
 assert(pw_d3d9_texture_client_unlock(&c)==S_OK&&t.bytes[first+13]==0x77&&!pw_d3d9_staging_bytes());
 q.flags=D3DLOCK_READONLY;assert(pw_d3d9_texture_client_lock(&c,&q,&returned_pitch,&bits)==S_OK);
 assert(pw_d3d9_texture_client_unlock(&c)==S_OK&&!t.written);q.flags=0;
 t.fail_op=PW_D3D9_TEXTURE_READ;assert(pw_d3d9_texture_client_lock(&c,&q,&returned_pitch,&bits)==(uint32_t)D3DERR_WASSTILLDRAWING);
 assert(!bits&&!t.active&&t.cancels==1&&!pw_d3d9_staging_bytes());t.fail_op=0;
 t.corrupt=1;assert(pw_d3d9_texture_client_lock(&c,&q,&returned_pitch,&bits)==(uint32_t)E_FAIL);
 assert(!bits&&!t.active&&t.cancels==2&&t.failures==1);t.corrupt=0;
 assert(pw_d3d9_texture_client_lock(&c,&q,&returned_pitch,&bits)==S_OK);t.fail_op=PW_D3D9_TEXTURE_WRITE;
 assert(pw_d3d9_texture_client_unlock(&c)==(uint32_t)D3DERR_WASSTILLDRAWING&&!t.active&&!pw_d3d9_staging_bytes());t.fail_op=0;
 assert(pw_d3d9_staging_alloc(PW_D3D9_RESOURCE_MAX_LOCK,&held)==S_OK);
 assert(pw_d3d9_texture_client_lock(&c,&q,&returned_pitch,&bits)==(uint32_t)E_OUTOFMEMORY&&!bits&&!t.active);
 assert(pw_d3d9_staging_free(held,PW_D3D9_RESOURCE_MAX_LOCK)==S_OK&&!pw_d3d9_staging_bytes());
 printf("PW_TEXTURE_CLIENT pitch=%ld low32=1 copied=1 readonly=1 failure_cleanup=1 budget=1\n",(long)pitch);
}
int main(void)
{assert(sizeof(void *)==4);scenario(520);scenario(-520);puts("PW_TEXTURE_CLIENT PASS");return 0;}
