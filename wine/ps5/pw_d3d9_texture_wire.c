/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_texture_wire.h"
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
{return op>=PW_D3D9_TEXTURE_CREATE&&op<=PW_D3D9_TEXTURE_RT_DATA;}
static int chunk(uint64_t gen,uint32_t off,uint32_t count)
{return gen&&count&&count<=PW_D3D9_RESOURCE_CHUNK&&off<=UINT32_MAX-count;}
int pw_d3d9_texture_layout_valid(int32_t pitch,uint32_t rows,uint32_t row_bytes,uint32_t length)
{
 uint64_t stride=pitch<0?-(int64_t)pitch:pitch;
 return pitch&&rows&&row_bytes&&stride>=row_bytes&&length<=PW_D3D9_RESOURCE_MAX_LOCK&&stride*(rows-1)+row_bytes==length;
}
#define O(f) offsetof(struct pw_d3d9_texture_request,f)
struct schema {unsigned n;size_t fields[15];};
static const struct schema schema[18]={
 [PW_D3D9_TEXTURE_CREATE]={6,{O(width),O(height),O(levels),O(usage),O(format),O(pool)}},
 [PW_D3D9_TEXTURE_CREATE_SURFACE]={4,{O(width),O(height),O(format),O(pool)}},
 [PW_D3D9_TEXTURE_DESC]={1,{O(level)}},[PW_D3D9_TEXTURE_SURFACE_LEVEL]={1,{O(level)}},
 [PW_D3D9_TEXTURE_LOCK]={7,{O(level),O(flags),O(has_rect),O(left),O(top),O(right),O(bottom)}},
 [PW_D3D9_TEXTURE_DIRTY]={5,{O(has_rect),O(left),O(top),O(right),O(bottom)}},
 [PW_D3D9_TEXTURE_UPDATE]={4,{O(source.id),O(source.generation),O(destination.id),O(destination.generation)}},
 [PW_D3D9_TEXTURE_UPDATE_SURFACE]={12,{O(source.id),O(source.generation),O(destination.id),O(destination.generation),O(has_rect),O(left),O(top),O(right),O(bottom),O(has_point),O(x),O(y)}},
 [PW_D3D9_TEXTURE_CREATE_RT]={6,{O(width),O(height),O(format),O(multisample_type),O(multisample_quality),O(lockable)}},
 [PW_D3D9_TEXTURE_CREATE_DEPTH]={6,{O(width),O(height),O(format),O(multisample_type),O(multisample_quality),O(discard)}},
 [PW_D3D9_TEXTURE_STRETCH]={15,{O(source.id),O(source.generation),O(destination.id),O(destination.generation),O(has_rect),O(left),O(top),O(right),O(bottom),O(has_destination_rect),O(destination_left),O(destination_top),O(destination_right),O(destination_bottom),O(filter)}},
 [PW_D3D9_TEXTURE_COLOR_FILL]={6,{O(has_rect),O(left),O(top),O(right),O(bottom),O(color)}},
 [PW_D3D9_TEXTURE_RT_DATA]={4,{O(source.id),O(source.generation),O(destination.id),O(destination.generation)}}
};
#undef O
static int request_size(const struct pw_d3d9_texture_request *q,size_t *n)
{
 if(!valid_op(q->operation))return 0;
 if(q->operation==PW_D3D9_TEXTURE_READ||q->operation==PW_D3D9_TEXTURE_WRITE){
  if(!chunk(q->lock_generation,q->offset,q->count))return 0;
  *n=16+(q->operation==PW_D3D9_TEXTURE_WRITE?q->count:0);return 1;
 }
 if(q->operation==PW_D3D9_TEXTURE_UNLOCK||q->operation==PW_D3D9_TEXTURE_CANCEL_LOCK){
  *n=8;return !!q->lock_generation;
 }
 if(q->operation==PW_D3D9_TEXTURE_LOCK||q->operation==PW_D3D9_TEXTURE_DIRTY||q->operation==PW_D3D9_TEXTURE_UPDATE_SURFACE||q->operation==PW_D3D9_TEXTURE_STRETCH||q->operation==PW_D3D9_TEXTURE_COLOR_FILL){
  if(q->has_rect>1||(!q->has_rect&&(q->left||q->top||q->right||q->bottom)))return 0;
 }
 if(q->operation==PW_D3D9_TEXTURE_UPDATE_SURFACE&&(q->has_point>1||(!q->has_point&&(q->x||q->y))))return 0;
 if((q->operation==PW_D3D9_TEXTURE_UPDATE||q->operation==PW_D3D9_TEXTURE_UPDATE_SURFACE||q->operation==PW_D3D9_TEXTURE_STRETCH||q->operation==PW_D3D9_TEXTURE_RT_DATA)&&(!q->source.id||!q->source.generation||!q->destination.id||!q->destination.generation))return 0;
 if(q->operation==PW_D3D9_TEXTURE_STRETCH&&(q->has_destination_rect>1||(!q->has_destination_rect&&(q->destination_left||q->destination_top||q->destination_right||q->destination_bottom))))return 0;
 *n=4*schema[q->operation].n;return 1;
}
int pw_d3d9_texture_request_encode(void *wire,size_t cap,size_t *written,const struct pw_d3d9_texture_request *q)
{
 unsigned char tmp[PW_D3D9_TEXTURE_MAX_WIRE]={0},*p=tmp+16;size_t n;unsigned i;
 if(!q||!written)return PW_D3D9_RESOURCE_INVALID;
 *written=0;if(!valid_op(q->operation))return PW_D3D9_RESOURCE_UNSUPPORTED;
 if(!request_size(q,&n))return PW_D3D9_RESOURCE_INVALID;
 if(cap<n+16)return PW_D3D9_RESOURCE_SMALL;
 if(!wire)return PW_D3D9_RESOURCE_INVALID;
 put32(tmp,PW_D3D9_TEXTURE_VERSION);put32(tmp+4,q->operation);put32(tmp+8,(uint32_t)n);
 if(q->operation>=PW_D3D9_TEXTURE_READ&&q->operation<=PW_D3D9_TEXTURE_CANCEL_LOCK){
  put64(p,q->lock_generation);
  if(q->operation<=PW_D3D9_TEXTURE_WRITE){put32(p+8,q->offset);put32(p+12,q->count);}
  if(q->operation==PW_D3D9_TEXTURE_WRITE)memcpy(p+16,q->data,q->count);
 }else for(i=0;i<schema[q->operation].n;i++){
  uint32_t value;memcpy(&value,(const unsigned char *)q+schema[q->operation].fields[i],4);put32(p+4*i,value);
 }
 memcpy(wire,tmp,n+16);*written=n+16;return PW_D3D9_RESOURCE_OK;
}
int pw_d3d9_texture_request_decode(struct pw_d3d9_texture_request *out,const void *wire,size_t bytes)
{
 struct pw_d3d9_texture_request q={0};const unsigned char *p=wire;size_t n;unsigned i;
 if(!out||!p||bytes<16||bytes>PW_D3D9_TEXTURE_MAX_WIRE||get32(p)!=PW_D3D9_TEXTURE_VERSION||get32(p+12)||get32(p+8)!=bytes-16)return PW_D3D9_RESOURCE_INVALID;
 q.operation=get32(p+4);p+=16;if(!valid_op(q.operation))return PW_D3D9_RESOURCE_UNSUPPORTED;
 if(q.operation>=PW_D3D9_TEXTURE_READ&&q.operation<=PW_D3D9_TEXTURE_CANCEL_LOCK){
  if(bytes<24)return PW_D3D9_RESOURCE_INVALID;
  q.lock_generation=get64(p);
  if(q.operation<=PW_D3D9_TEXTURE_WRITE){
   if(bytes<32)return PW_D3D9_RESOURCE_INVALID;
   q.offset=get32(p+8);q.count=get32(p+12);
  }
 }else{
  if(bytes!=16+4*schema[q.operation].n)return PW_D3D9_RESOURCE_INVALID;
  for(i=0;i<schema[q.operation].n;i++){uint32_t value=get32(p+4*i);memcpy((unsigned char *)&q+schema[q.operation].fields[i],&value,4);}
 }
 if(!request_size(&q,&n)||bytes!=n+16)return PW_D3D9_RESOURCE_INVALID;
 if(q.operation==PW_D3D9_TEXTURE_WRITE)memcpy(q.data,p+16,q.count);
 *out=q;return PW_D3D9_RESOURCE_OK;
}
static void desc_put(unsigned char *p,const struct pw_d3d9_surface_desc *d)
{
 put32(p,d->format);put32(p+4,d->type);put32(p+8,d->usage);put32(p+12,d->pool);
 put32(p+16,d->multisample_type);put32(p+20,d->multisample_quality);put32(p+24,d->width);put32(p+28,d->height);
}
static void desc_get(struct pw_d3d9_surface_desc *d,const unsigned char *p)
{
 d->format=get32(p);d->type=get32(p+4);d->usage=get32(p+8);d->pool=get32(p+12);
 d->multisample_type=get32(p+16);d->multisample_quality=get32(p+20);d->width=get32(p+24);d->height=get32(p+28);
}
static int reply_size(const struct pw_d3d9_texture_reply *r,size_t *n)
{
 if(!valid_op(r->operation))return 0;
 *n=0;if(r->hresult&0x80000000u)return 1;
 switch(r->operation){
 case PW_D3D9_TEXTURE_CREATE:case PW_D3D9_TEXTURE_CREATE_SURFACE:case PW_D3D9_TEXTURE_CREATE_RT:case PW_D3D9_TEXTURE_CREATE_DEPTH:case PW_D3D9_TEXTURE_SURFACE_LEVEL:
  if(!r->object.id||!r->object.generation||!r->levels)return 0;
  *n=44;break;
 case PW_D3D9_TEXTURE_DESC:if(!r->levels)return 0;*n=36;break;
 case PW_D3D9_TEXTURE_LOCK:
  if(!r->lock_generation||!pw_d3d9_texture_layout_valid(r->pitch,r->rows,r->row_bytes,r->length))return 0;
  *n=24;break;
 case PW_D3D9_TEXTURE_READ:
  if(!chunk(r->lock_generation,r->offset,r->count))return 0;
  *n=16+r->count;break;
 default:break;
 }return 1;
}
int pw_d3d9_texture_reply_encode(void *wire,size_t cap,size_t *written,const struct pw_d3d9_texture_reply *r)
{
 unsigned char tmp[PW_D3D9_TEXTURE_MAX_WIRE]={0},*p=tmp+16;size_t n;
 if(!r||!written)return PW_D3D9_RESOURCE_INVALID;
 *written=0;if(!valid_op(r->operation))return PW_D3D9_RESOURCE_UNSUPPORTED;
 if(!reply_size(r,&n))return PW_D3D9_RESOURCE_INVALID;
 if(cap<n+16)return PW_D3D9_RESOURCE_SMALL;
 if(!wire)return PW_D3D9_RESOURCE_INVALID;
 put32(tmp,PW_D3D9_TEXTURE_VERSION);put32(tmp+4,r->operation);put32(tmp+8,r->hresult);put32(tmp+12,(uint32_t)n);
 if(n)switch(r->operation){
 case PW_D3D9_TEXTURE_CREATE:case PW_D3D9_TEXTURE_CREATE_SURFACE:case PW_D3D9_TEXTURE_CREATE_RT:case PW_D3D9_TEXTURE_CREATE_DEPTH:case PW_D3D9_TEXTURE_SURFACE_LEVEL:
  put32(p,r->object.id);put32(p+4,r->object.generation);put32(p+8,r->levels);desc_put(p+12,&r->desc);break;
 case PW_D3D9_TEXTURE_DESC:desc_put(p,&r->desc);put32(p+32,r->levels);break;
 case PW_D3D9_TEXTURE_LOCK:put64(p,r->lock_generation);put32(p+8,(uint32_t)r->pitch);put32(p+12,r->rows);put32(p+16,r->row_bytes);put32(p+20,r->length);break;
 case PW_D3D9_TEXTURE_READ:put64(p,r->lock_generation);put32(p+8,r->offset);put32(p+12,r->count);memcpy(p+16,r->data,r->count);break;
 default:break;
 }
 memcpy(wire,tmp,n+16);*written=n+16;return PW_D3D9_RESOURCE_OK;
}
int pw_d3d9_texture_reply_decode(struct pw_d3d9_texture_reply *out,const void *wire,size_t bytes)
{
 struct pw_d3d9_texture_reply r={0};const unsigned char *p=wire;size_t n;
 if(!out||!p||bytes<16||bytes>PW_D3D9_TEXTURE_MAX_WIRE||get32(p)!=PW_D3D9_TEXTURE_VERSION||get32(p+12)!=bytes-16)return PW_D3D9_RESOURCE_INVALID;
 r.operation=get32(p+4);r.hresult=get32(p+8);p+=16;if(!valid_op(r.operation))return PW_D3D9_RESOURCE_UNSUPPORTED;
 if(!(r.hresult&0x80000000u))switch(r.operation){
 case PW_D3D9_TEXTURE_CREATE:case PW_D3D9_TEXTURE_CREATE_SURFACE:case PW_D3D9_TEXTURE_CREATE_RT:case PW_D3D9_TEXTURE_CREATE_DEPTH:case PW_D3D9_TEXTURE_SURFACE_LEVEL:
  if(bytes!=60)return PW_D3D9_RESOURCE_INVALID;
  r.object.id=get32(p);r.object.generation=get32(p+4);r.levels=get32(p+8);desc_get(&r.desc,p+12);break;
 case PW_D3D9_TEXTURE_DESC:
  if(bytes!=52)return PW_D3D9_RESOURCE_INVALID;
  desc_get(&r.desc,p);r.levels=get32(p+32);break;
 case PW_D3D9_TEXTURE_LOCK:
  if(bytes!=40)return PW_D3D9_RESOURCE_INVALID;
  r.lock_generation=get64(p);r.pitch=(int32_t)get32(p+8);r.rows=get32(p+12);r.row_bytes=get32(p+16);r.length=get32(p+20);break;
 case PW_D3D9_TEXTURE_READ:
  if(bytes<32)return PW_D3D9_RESOURCE_INVALID;
  r.lock_generation=get64(p);r.offset=get32(p+8);r.count=get32(p+12);break;
 default:break;
 }
 if(!reply_size(&r,&n)||bytes!=n+16)return PW_D3D9_RESOURCE_INVALID;
 if(r.operation==PW_D3D9_TEXTURE_READ&&n)memcpy(r.data,p+16,r.count);
 *out=r;return PW_D3D9_RESOURCE_OK;
}
