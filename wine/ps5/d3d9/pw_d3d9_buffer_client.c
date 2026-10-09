/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_buffer_client.h"
#include "pw_d3d9_staging.h"
#include <windows.h>
#include <d3d9.h>
#include <string.h>
uint32_t pw_d3d9_buffer_client_staging_bytes(void)
{return pw_d3d9_staging_bytes();}
static uint32_t invoke(struct pw_d3d9_buffer_client *s,const struct pw_d3d9_resource_request *q,struct pw_d3d9_resource_reply *r)
{
 uint32_t hr;memset(r,0,sizeof(*r));hr=s->call(s->context,s->object,q,r);
 if(!(hr&0x80000000u)&&(r->operation!=q->operation||r->hresult!=hr)){
  s->fail(s->context,E_FAIL);return E_FAIL;
 }return hr;
}
static void drop(struct pw_d3d9_buffer_client *s)
{
 if(s->data){
  uint32_t hr=pw_d3d9_staging_free(s->data,s->length);
  if(hr&0x80000000u)s->fail(s->context,hr);
 }
 s->data=NULL;s->length=s->flags=0;s->generation=0;
}
uint32_t pw_d3d9_buffer_client_cancel(struct pw_d3d9_buffer_client *s)
{
 struct pw_d3d9_resource_request q={0};struct pw_d3d9_resource_reply r;uint32_t hr=S_OK;
 if(!s||!s->call||!s->fail)return D3DERR_INVALIDCALL;
 if(s->generation){
  q.operation=PW_D3D9_RESOURCE_CANCEL_LOCK;q.lock_generation=s->generation;hr=invoke(s,&q,&r);
  if(hr&0x80000000u)s->fail(s->context,hr);
 }
 s->last_cleanup_result=hr;drop(s);return hr;
}
uint32_t pw_d3d9_buffer_client_lock(struct pw_d3d9_buffer_client *s,uint32_t offset,uint32_t length,uint32_t flags,void **data)
{
 struct pw_d3d9_resource_request q={0};struct pw_d3d9_resource_reply r;uint32_t hr,lock_hr,pos;
 if(data)*data=NULL;
 if(!s||!data||!s->call||!s->fail||!s->object.id||!s->object.generation)return D3DERR_INVALIDCALL;
 if(s->generation)return E_NOTIMPL;
 q.operation=PW_D3D9_RESOURCE_LOCK;q.offset=offset;q.length=length;q.flags=flags;
 hr=invoke(s,&q,&r);if(hr&0x80000000u)return hr;
 if(!r.lock_generation||!r.length||r.length>PW_D3D9_RESOURCE_MAX_LOCK){s->fail(s->context,E_FAIL);return E_FAIL;}
 lock_hr=hr;s->last_cleanup_result=S_OK;
 s->generation=r.lock_generation;s->length=r.length;s->flags=flags;
 hr=pw_d3d9_staging_alloc(s->length,&s->data);if(hr&0x80000000u)goto fail;
 /* Prefill even DISCARD. The backend, not this client, decides when DISCARD is
  * ignored (pool, NOOVERWRITE, lost-device state). This preserves partial edits. */
 q.operation=PW_D3D9_RESOURCE_READ;q.lock_generation=s->generation;
 for(pos=0;pos<s->length;pos+=q.count){
  q.offset=pos;q.count=s->length-pos<PW_D3D9_RESOURCE_CHUNK?s->length-pos:PW_D3D9_RESOURCE_CHUNK;
  hr=invoke(s,&q,&r);if(hr&0x80000000u)goto fail;
  if(r.lock_generation!=s->generation||r.offset!=q.offset||r.count!=q.count){s->fail(s->context,E_FAIL);hr=E_FAIL;goto fail;}
  memcpy((unsigned char *)s->data+pos,r.data,q.count);
 }
 *data=s->data;return lock_hr;
 fail:pw_d3d9_buffer_client_cancel(s);return hr;
}
uint32_t pw_d3d9_buffer_client_unlock(struct pw_d3d9_buffer_client *s)
{
 struct pw_d3d9_resource_request q={0};struct pw_d3d9_resource_reply r;uint32_t hr,pos;
 if(!s||!s->data||!s->generation||!s->call||!s->fail)return D3DERR_INVALIDCALL;
 q.lock_generation=s->generation;
 if(!(s->flags&D3DLOCK_READONLY)){
  q.operation=PW_D3D9_RESOURCE_WRITE;
  for(pos=0;pos<s->length;pos+=q.count){
   q.offset=pos;q.count=s->length-pos<PW_D3D9_RESOURCE_CHUNK?s->length-pos:PW_D3D9_RESOURCE_CHUNK;
   memcpy(q.data,(unsigned char *)s->data+pos,q.count);
   hr=invoke(s,&q,&r);if(hr&0x80000000u){pw_d3d9_buffer_client_cancel(s);return hr;}
  }
 }
 q.operation=PW_D3D9_RESOURCE_UNLOCK;hr=invoke(s,&q,&r);
 if(hr&0x80000000u){pw_d3d9_buffer_client_cancel(s);return hr;}
 drop(s);return hr;
}
