/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_up_wire.h"
#include <string.h>
static uint32_t get(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int pw_d3d9_up_measure(struct pw_d3d9_up_draw *d)
{
 uint64_t count,vertices,vb,ib=0;
 if(!d||(d->method!=83&&d->method!=84))return PW_D3D9_UP_INVALID;
 if(d->method==83&&(d->min_vertex||d->num_vertices||d->index_format))return PW_D3D9_UP_INVALID;
 /* The backend checks declaration existence before its zero-primitive return. */
 if(!d->primitive_count){d->vertex_bytes=d->index_bytes=0;return 0;}
 switch(d->primitive_type){
 case 1:count=d->primitive_count;break;
 case 2:count=(uint64_t)d->primitive_count*2;break;
 case 3:count=(uint64_t)d->primitive_count+1;break;
 case 4:count=(uint64_t)d->primitive_count*3;break;
 case 5:case 6:count=(uint64_t)d->primitive_count+2;break;
 default:return PW_D3D9_UP_INVALID;
 }
 if(count>UINT32_MAX)return PW_D3D9_UP_INVALID;
 vertices=count;
 if(d->method==84){
  if(d->index_format!=101&&d->index_format!=102)return PW_D3D9_UP_INVALID;
  vertices=(uint64_t)d->min_vertex+d->num_vertices;
  if(!vertices||vertices>UINT32_MAX)return PW_D3D9_UP_INVALID;
  ib=count*(d->index_format==101?2:4);
 }
 vb=vertices*d->stride;
 if(vb>PW_D3D9_UP_LIMIT||ib>PW_D3D9_UP_LIMIT||vb+ib>PW_D3D9_UP_LIMIT)return PW_D3D9_UP_INVALID;
 d->vertex_bytes=(uint32_t)vb;d->index_bytes=(uint32_t)ib;return 0;
}
static int valid(const struct pw_d3d9_up_request *q)
{
 struct pw_d3d9_up_draw d,zero={0};
 if(!q)return 0;
 if(q->operation==PW_D3D9_UP_BEGIN){
  d=q->draw;
  return !q->transfer&&!q->offset&&!q->count&&!pw_d3d9_up_measure(&d)&&d.vertex_bytes==q->draw.vertex_bytes&&d.index_bytes==q->draw.index_bytes;
 }
 if(!q->transfer||memcmp(&q->draw,&zero,sizeof(zero)))return 0;
 if(q->operation==PW_D3D9_UP_WRITE)return q->count&&q->count<=PW_D3D9_UP_CHUNK&&q->offset<=PW_D3D9_UP_LIMIT-q->count;
 return (q->operation==PW_D3D9_UP_COMMIT||q->operation==PW_D3D9_UP_ABORT)&&!q->offset&&!q->count;
}
int pw_d3d9_up_encode(void *out,size_t capacity,size_t *written,const struct pw_d3d9_up_request *q)
{
 unsigned char *p=out;size_t n;
 if(!p||!written||!valid(q))return PW_D3D9_UP_INVALID;
 n=64+q->count;if(capacity<n)return PW_D3D9_UP_SMALL;
 put(p,1);put(p+4,q->operation);put(p+8,q->draw.method);put(p+12,q->draw.primitive_type);
 put(p+16,q->draw.min_vertex);put(p+20,q->draw.num_vertices);put(p+24,q->draw.primitive_count);put(p+28,q->draw.stride);
 put(p+32,q->draw.index_format);put(p+36,q->draw.vertex_bytes);put(p+40,q->draw.index_bytes);put(p+44,q->offset);put(p+48,q->count);
 put(p+52,q->transfer);put(p+56,q->transfer>>32);put(p+60,0);memcpy(p+64,q->data,q->count);*written=n;return 0;
}
int pw_d3d9_up_decode(struct pw_d3d9_up_request *out,const void *data,size_t size)
{
 const unsigned char *p=data;struct pw_d3d9_up_request q={0};
 if(!out||!p||size<64||get(p)!=1||get(p+60))return PW_D3D9_UP_INVALID;
 q.operation=get(p+4);q.draw.method=get(p+8);q.draw.primitive_type=get(p+12);q.draw.min_vertex=get(p+16);q.draw.num_vertices=get(p+20);
 q.draw.primitive_count=get(p+24);q.draw.stride=get(p+28);q.draw.index_format=get(p+32);q.draw.vertex_bytes=get(p+36);q.draw.index_bytes=get(p+40);
 q.offset=get(p+44);q.count=get(p+48);q.transfer=get(p+52)|(uint64_t)get(p+56)<<32;
 if(!valid(&q)||size!=64+(size_t)q.count)return PW_D3D9_UP_INVALID;
 memcpy(q.data,p+64,q.count);*out=q;return 0;
}
static int reply_valid(const struct pw_d3d9_up_reply *r)
{return r&&r->operation>=1&&r->operation<=4&&((r->hresult&0x80000000u)?!r->transfer:!!r->transfer);}
int pw_d3d9_up_reply_encode(void *out,size_t capacity,size_t *written,const struct pw_d3d9_up_reply *r)
{
 unsigned char *p=out;if(!p||!written||!reply_valid(r))return PW_D3D9_UP_INVALID;if(capacity<24)return PW_D3D9_UP_SMALL;
 put(p,1);put(p+4,r->operation);put(p+8,r->hresult);put(p+12,0);put(p+16,r->transfer);put(p+20,r->transfer>>32);*written=24;return 0;
}
int pw_d3d9_up_reply_decode(struct pw_d3d9_up_reply *out,const void *data,size_t size)
{
 const unsigned char *p=data;struct pw_d3d9_up_reply r;
 if(!out||!p||size!=24||get(p)!=1||get(p+12))return PW_D3D9_UP_INVALID;
 r.operation=get(p+4);r.hresult=get(p+8);r.transfer=get(p+16)|(uint64_t)get(p+20)<<32;
 if(!reply_valid(&r))return PW_D3D9_UP_INVALID;
 *out=r;return 0;
}
void pw_d3d9_up_upload_init(struct pw_d3d9_up_upload *u,void *storage,size_t capacity)
{memset(u,0,sizeof(*u));u->storage=storage;u->capacity=capacity;}
void pw_d3d9_up_upload_finish(struct pw_d3d9_up_upload *u)
{u->transfer=0;memset(&u->draw,0,sizeof(u->draw));u->total=u->received=u->ready=0;}
int pw_d3d9_up_upload_apply(struct pw_d3d9_up_upload *u,const struct pw_d3d9_up_request *q,uint64_t *transfer)
{
 uint32_t total;
 if(!u||!transfer||!valid(q))return PW_D3D9_UP_INVALID;
 if(q->operation==PW_D3D9_UP_BEGIN){
  if(u->transfer)return PW_D3D9_UP_BUSY;
  if(u->next==UINT64_MAX)return PW_D3D9_UP_EXHAUSTED;
  total=q->draw.vertex_bytes+q->draw.index_bytes;
  if(total&&(!u->storage||u->capacity<total))return PW_D3D9_UP_SMALL;
  u->transfer=++u->next;u->draw=q->draw;u->total=total;u->received=u->ready=0;*transfer=u->transfer;return 0;
 }
 if(!u->transfer||q->transfer!=u->transfer)return PW_D3D9_UP_STALE;
 if(q->operation==PW_D3D9_UP_ABORT){*transfer=u->transfer;pw_d3d9_up_upload_finish(u);return 0;}
 if(u->ready)return PW_D3D9_UP_BUSY;
 if(q->operation==PW_D3D9_UP_WRITE){
  if(q->offset!=u->received||q->count>u->total-u->received)return PW_D3D9_UP_INVALID;
  memcpy(u->storage+u->received,q->data,q->count);u->received+=q->count;
 }else{
  if(u->received!=u->total)return PW_D3D9_UP_INVALID;
  u->ready=1;
 }
 *transfer=u->transfer;return 0;
}
