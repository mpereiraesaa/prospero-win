/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_texture_client.h"
#include "pw_d3d9_staging.h"
#include <windows.h>
#include <d3d9.h>
#include <string.h>
static uint32_t invoke(struct pw_d3d9_texture_client *s,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *r)
{
 uint32_t hr;memset(r,0,sizeof(*r));hr=s->call(s->context,s->object,q,r);
 if(!(hr&0x80000000u)&&(r->operation!=q->operation||r->hresult!=hr)){s->fail(s->context,E_FAIL);return E_FAIL;}
 return hr;
}
static void drop(struct pw_d3d9_texture_client *s)
{
 if(s->data){uint32_t hr=pw_d3d9_staging_free(s->data,s->length);if(hr&0x80000000u)s->fail(s->context,hr);}
 s->data=NULL;s->generation=0;s->pitch=0;s->length=s->rows=s->row_bytes=s->flags=0;
}
uint32_t pw_d3d9_texture_client_cancel(struct pw_d3d9_texture_client *s)
{
 struct pw_d3d9_texture_request q={0};struct pw_d3d9_texture_reply r;uint32_t hr=S_OK;
 if(!s||!s->call||!s->fail)return D3DERR_INVALIDCALL;
 if(s->generation){q.operation=PW_D3D9_TEXTURE_CANCEL_LOCK;q.lock_generation=s->generation;hr=invoke(s,&q,&r);if(hr&0x80000000u)s->fail(s->context,hr);}
 s->last_cleanup_result=hr;drop(s);return hr;
}
uint32_t pw_d3d9_texture_client_lock(struct pw_d3d9_texture_client *s,const struct pw_d3d9_texture_request *request,int32_t *pitch,void **bits)
{
 struct pw_d3d9_texture_request q;struct pw_d3d9_texture_reply r;uint32_t hr,lock_hr,pos;
 if(bits)*bits=NULL;
 if(pitch)*pitch=0;
 if(!s||!request||!pitch||!bits||!s->call||!s->fail||!s->object.id||!s->object.generation||request->operation!=PW_D3D9_TEXTURE_LOCK)return D3DERR_INVALIDCALL;
 if(s->generation)return E_NOTIMPL;
 q=*request;hr=invoke(s,&q,&r);if(hr&0x80000000u)return hr;
 if(!r.lock_generation){s->fail(s->context,E_FAIL);return E_FAIL;}
 s->generation=r.lock_generation;s->last_cleanup_result=S_OK;
 if(!pw_d3d9_texture_layout_valid(r.pitch,r.rows,r.row_bytes,r.length)){s->fail(s->context,E_FAIL);hr=E_FAIL;goto failed;}
 lock_hr=hr;s->pitch=r.pitch;s->rows=r.rows;s->row_bytes=r.row_bytes;s->length=r.length;s->flags=q.flags;
 hr=pw_d3d9_staging_alloc(s->length,&s->data);if(hr&0x80000000u)goto failed;
 q.operation=PW_D3D9_TEXTURE_READ;q.lock_generation=s->generation;
 for(pos=0;pos<s->length;pos+=q.count){
  q.offset=pos;q.count=s->length-pos<PW_D3D9_RESOURCE_CHUNK?s->length-pos:PW_D3D9_RESOURCE_CHUNK;
  hr=invoke(s,&q,&r);if(hr&0x80000000u)goto failed;
  if(r.lock_generation!=s->generation||r.offset!=q.offset||r.count!=q.count){s->fail(s->context,E_FAIL);hr=E_FAIL;goto failed;}
  memcpy((unsigned char *)s->data+pos,r.data,q.count);
 }
 *pitch=s->pitch;*bits=(unsigned char *)s->data+(s->pitch<0?(uint64_t)-(int64_t)s->pitch*(s->rows-1):0);return lock_hr;
 failed:pw_d3d9_texture_client_cancel(s);return hr;
}
uint32_t pw_d3d9_texture_client_unlock(struct pw_d3d9_texture_client *s)
{
 struct pw_d3d9_texture_request q={0};struct pw_d3d9_texture_reply r;uint32_t hr,pos;
 if(!s||!s->data||!s->generation||!s->call||!s->fail)return D3DERR_INVALIDCALL;
 q.lock_generation=s->generation;
 if(!(s->flags&D3DLOCK_READONLY)){
  q.operation=PW_D3D9_TEXTURE_WRITE;
  for(pos=0;pos<s->length;pos+=q.count){
   q.offset=pos;q.count=s->length-pos<PW_D3D9_RESOURCE_CHUNK?s->length-pos:PW_D3D9_RESOURCE_CHUNK;
   memcpy(q.data,(unsigned char *)s->data+pos,q.count);hr=invoke(s,&q,&r);
   if(hr&0x80000000u){pw_d3d9_texture_client_cancel(s);return hr;}
  }
 }
 q.operation=PW_D3D9_TEXTURE_UNLOCK;hr=invoke(s,&q,&r);
 if(hr&0x80000000u){pw_d3d9_texture_client_cancel(s);return hr;}
 drop(s);return hr;
}
