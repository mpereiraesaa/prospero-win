/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_resource_wire.h"
#include <string.h>
static uint32_t get32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const unsigned char *p)
{return get32(p)|(uint64_t)get32(p+4)<<32;}
static void put32(unsigned char *p,uint32_t n)
{unsigned i;for(i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
static void put64(unsigned char *p,uint64_t n)
{put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static int valid_op(uint32_t op)
{return op>=PW_D3D9_RESOURCE_CREATE_VB&&op<=PW_D3D9_RESOURCE_UNLOCK;}
static int chunk_valid(uint64_t generation,uint32_t offset,uint32_t count)
{return generation&&count&&count<=PW_D3D9_RESOURCE_CHUNK&&offset<=UINT32_MAX-count;}
static int req_size(const struct pw_d3d9_resource_request *q,size_t *n)
{
 switch(q->operation){
 case PW_D3D9_RESOURCE_CREATE_VB:case PW_D3D9_RESOURCE_CREATE_IB:*n=16;break;
 case PW_D3D9_RESOURCE_DESC:*n=0;break;
 case PW_D3D9_RESOURCE_LOCK:*n=12;break;
 case PW_D3D9_RESOURCE_READ:case PW_D3D9_RESOURCE_WRITE:
  if(!chunk_valid(q->lock_generation,q->offset,q->count))return 0;
  *n=16+(q->operation==PW_D3D9_RESOURCE_WRITE?q->count:0);break;
 case PW_D3D9_RESOURCE_UNLOCK:if(!q->lock_generation)return 0;*n=8;break;
 default:return 0;
 }return 1;
}
int pw_d3d9_resource_request_encode(void *wire,size_t cap,size_t *written,const struct pw_d3d9_resource_request *q)
{
 unsigned char tmp[PW_D3D9_RESOURCE_MAX_WIRE]={0},*p=tmp+16;size_t n;
 if(!written||!q)return PW_D3D9_RESOURCE_INVALID;
 *written=0;if(!valid_op(q->operation))return PW_D3D9_RESOURCE_UNSUPPORTED;
 if(!req_size(q,&n))return PW_D3D9_RESOURCE_INVALID;
 if(cap<n+16)return PW_D3D9_RESOURCE_SMALL;
 if(!wire)return PW_D3D9_RESOURCE_INVALID;
 put32(tmp,PW_D3D9_RESOURCE_VERSION);put32(tmp+4,q->operation);put32(tmp+8,(uint32_t)n);
 switch(q->operation){
 case PW_D3D9_RESOURCE_CREATE_VB:case PW_D3D9_RESOURCE_CREATE_IB:
  put32(p,q->length);put32(p+4,q->usage);put32(p+8,q->format_fvf);put32(p+12,q->pool);break;
 case PW_D3D9_RESOURCE_LOCK:put32(p,q->offset);put32(p+4,q->length);put32(p+8,q->flags);break;
 case PW_D3D9_RESOURCE_READ:case PW_D3D9_RESOURCE_WRITE:
  put64(p,q->lock_generation);put32(p+8,q->offset);put32(p+12,q->count);
  if(q->operation==PW_D3D9_RESOURCE_WRITE)memcpy(p+16,q->data,q->count);
  break;
 case PW_D3D9_RESOURCE_UNLOCK:put64(p,q->lock_generation);break;
 default:break;
 }
 memcpy(wire,tmp,n+16);*written=n+16;return PW_D3D9_RESOURCE_OK;
}
int pw_d3d9_resource_request_decode(struct pw_d3d9_resource_request *out,const void *wire,size_t bytes)
{
 const unsigned char *p=wire;struct pw_d3d9_resource_request q={0};size_t n;
 if(!out||!p||bytes<16||bytes>PW_D3D9_RESOURCE_MAX_WIRE||get32(p)!=PW_D3D9_RESOURCE_VERSION||get32(p+12)||get32(p+8)!=bytes-16)return PW_D3D9_RESOURCE_INVALID;
 q.operation=get32(p+4);p+=16;
 if(!valid_op(q.operation))return PW_D3D9_RESOURCE_UNSUPPORTED;
 switch(q.operation){
 case PW_D3D9_RESOURCE_CREATE_VB:case PW_D3D9_RESOURCE_CREATE_IB:
  if(bytes!=32)return PW_D3D9_RESOURCE_INVALID;
  q.length=get32(p);q.usage=get32(p+4);q.format_fvf=get32(p+8);q.pool=get32(p+12);break;
 case PW_D3D9_RESOURCE_DESC:if(bytes!=16)return PW_D3D9_RESOURCE_INVALID;break;
 case PW_D3D9_RESOURCE_LOCK:
  if(bytes!=28)return PW_D3D9_RESOURCE_INVALID;
  q.offset=get32(p);q.length=get32(p+4);q.flags=get32(p+8);break;
 case PW_D3D9_RESOURCE_READ:case PW_D3D9_RESOURCE_WRITE:
  if(bytes<32)return PW_D3D9_RESOURCE_INVALID;
  q.lock_generation=get64(p);q.offset=get32(p+8);q.count=get32(p+12);break;
 case PW_D3D9_RESOURCE_UNLOCK:
  if(bytes!=24)return PW_D3D9_RESOURCE_INVALID;
  q.lock_generation=get64(p);break;
 default:return PW_D3D9_RESOURCE_INVALID;
 }
 if(!req_size(&q,&n)||bytes!=n+16)return PW_D3D9_RESOURCE_INVALID;
 if(q.operation==PW_D3D9_RESOURCE_WRITE)memcpy(q.data,p+16,q.count);
 *out=q;return PW_D3D9_RESOURCE_OK;
}
static void desc_put(unsigned char *p,const struct pw_d3d9_buffer_desc *d)
{put32(p,d->format);put32(p+4,d->type);put32(p+8,d->usage);put32(p+12,d->pool);put32(p+16,d->size);put32(p+20,d->fvf);}
static void desc_get(struct pw_d3d9_buffer_desc *d,const unsigned char *p)
{d->format=get32(p);d->type=get32(p+4);d->usage=get32(p+8);d->pool=get32(p+12);d->size=get32(p+16);d->fvf=get32(p+20);}
static int reply_size(const struct pw_d3d9_resource_reply *r,size_t *n)
{
 if(!valid_op(r->operation))return 0;
 *n=0;if(r->hresult&UINT32_C(0x80000000))return 1;
 switch(r->operation){
 case PW_D3D9_RESOURCE_CREATE_VB:case PW_D3D9_RESOURCE_CREATE_IB:
  if(!r->object.id||!r->object.generation)return 0;
  *n=32;break;
 case PW_D3D9_RESOURCE_DESC:*n=24;break;
 case PW_D3D9_RESOURCE_LOCK:
  if(!r->lock_generation||!r->length||r->length>PW_D3D9_RESOURCE_MAX_LOCK)return 0;
  *n=12;break;
 case PW_D3D9_RESOURCE_READ:
  if(!chunk_valid(r->lock_generation,r->offset,r->count))return 0;
  *n=16+r->count;break;
 default:break;
 }return 1;
}
int pw_d3d9_resource_reply_encode(void *wire,size_t cap,size_t *written,const struct pw_d3d9_resource_reply *r)
{
 unsigned char tmp[PW_D3D9_RESOURCE_MAX_WIRE]={0},*p=tmp+16;size_t n;
 if(!written||!r)return PW_D3D9_RESOURCE_INVALID;
 *written=0;if(!valid_op(r->operation))return PW_D3D9_RESOURCE_UNSUPPORTED;
 if(!reply_size(r,&n))return PW_D3D9_RESOURCE_INVALID;
 if(cap<n+16)return PW_D3D9_RESOURCE_SMALL;
 if(!wire)return PW_D3D9_RESOURCE_INVALID;
 put32(tmp,PW_D3D9_RESOURCE_VERSION);put32(tmp+4,r->operation);put32(tmp+8,r->hresult);put32(tmp+12,(uint32_t)n);
 if(n)switch(r->operation){
 case PW_D3D9_RESOURCE_CREATE_VB:case PW_D3D9_RESOURCE_CREATE_IB:
  put32(p,r->object.id);put32(p+4,r->object.generation);desc_put(p+8,&r->desc);break;
 case PW_D3D9_RESOURCE_DESC:desc_put(p,&r->desc);break;
 case PW_D3D9_RESOURCE_LOCK:put64(p,r->lock_generation);put32(p+8,r->length);break;
 case PW_D3D9_RESOURCE_READ:
  put64(p,r->lock_generation);put32(p+8,r->offset);put32(p+12,r->count);memcpy(p+16,r->data,r->count);break;
 default:break;
 }
 memcpy(wire,tmp,n+16);*written=n+16;return PW_D3D9_RESOURCE_OK;
}
int pw_d3d9_resource_reply_decode(struct pw_d3d9_resource_reply *out,const void *wire,size_t bytes)
{
 const unsigned char *p=wire;struct pw_d3d9_resource_reply r={0};size_t n;
 if(!out||!p||bytes<16||bytes>PW_D3D9_RESOURCE_MAX_WIRE||get32(p)!=PW_D3D9_RESOURCE_VERSION||get32(p+12)!=bytes-16)return PW_D3D9_RESOURCE_INVALID;
 r.operation=get32(p+4);r.hresult=get32(p+8);p+=16;
 if(!valid_op(r.operation))return PW_D3D9_RESOURCE_UNSUPPORTED;
 if(!(r.hresult&UINT32_C(0x80000000)))switch(r.operation){
 case PW_D3D9_RESOURCE_CREATE_VB:case PW_D3D9_RESOURCE_CREATE_IB:
  if(bytes!=48)return PW_D3D9_RESOURCE_INVALID;
  r.object.id=get32(p);r.object.generation=get32(p+4);desc_get(&r.desc,p+8);break;
 case PW_D3D9_RESOURCE_DESC:
  if(bytes!=40)return PW_D3D9_RESOURCE_INVALID;
  desc_get(&r.desc,p);break;
 case PW_D3D9_RESOURCE_LOCK:
  if(bytes!=28)return PW_D3D9_RESOURCE_INVALID;
  r.lock_generation=get64(p);r.length=get32(p+8);break;
 case PW_D3D9_RESOURCE_READ:
  if(bytes<32)return PW_D3D9_RESOURCE_INVALID;
  r.lock_generation=get64(p);r.offset=get32(p+8);r.count=get32(p+12);break;
 default:break;
 }
 if(!reply_size(&r,&n)||bytes!=n+16)return PW_D3D9_RESOURCE_INVALID;
 if(r.operation==PW_D3D9_RESOURCE_READ&&n)memcpy(r.data,p+16,r.count);
 *out=r;return PW_D3D9_RESOURCE_OK;
}
