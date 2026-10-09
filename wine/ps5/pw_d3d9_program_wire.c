/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_program_wire.h"
#include <string.h>
static uint32_t get(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static int kind_ok(uint32_t k){return k>=PW_D3D9_PROGRAM_DECL&&k<=PW_D3D9_PROGRAM_PS;}
/* SM1 parameter counts, before the instruction-length field existed. Semantic
 * legality stays with the real D3D9 backend; unknown opcodes fail closed. */
static int sm1_count(uint32_t op,unsigned pixel,unsigned minor)
{
 switch(op){
 case 0: case 0xfffd:return 0;
 case 1:case 6:case 7:case 14:case 15:case 16:case 19:case 78:case 79:return 2;
 case 2:case 3:case 5:case 8:case 9:case 10:case 11:case 12:case 13:case 17:
 case 20:case 21:case 22:case 23:case 24:return 3;
 case 4:case 18:case 80:case 88:return 4;
 case 81:return 5;
 case 64:case 66:return pixel?(minor==4?2:1):-1;
 case 65:case 87:return pixel?1:-1;
 case 67:case 68:case 69:case 70:case 71:case 72:case 73:case 74:
 case 77:case 82:case 83:case 84:case 85:case 86:return pixel?2:-1;
 case 76:case 89:return pixel?3:-1;
 default:return -1;
 }
}
int pw_d3d9_program_measure(uint32_t kind,pw_d3d9_program_reader read,void *ctx,size_t available,size_t *words)
{
 uint32_t v,t,op;size_t i=1,n;unsigned major,minor,pixel;int count;
 if(!read||!words||(kind!=PW_D3D9_PROGRAM_VS&&kind!=PW_D3D9_PROGRAM_PS)||!available||available>PW_D3D9_PROGRAM_LIMIT/4||read(ctx,0,&v))return PW_D3D9_PROGRAM_INVALID;
 pixel=kind==PW_D3D9_PROGRAM_PS;major=(v>>8)&255;minor=v&255;
 if((v>>16)!=(pixel?0xffffu:0xfffeu)||major<1||major>3||(major==1&&(minor>4||(!pixel&&minor!=1))))return PW_D3D9_PROGRAM_INVALID;
 while(i<available){
  if(read(ctx,i,&t))return PW_D3D9_PROGRAM_INVALID;
  if(t==0x0000ffff){*words=i+1;return 0;}
  op=t&0xffff;
  if(op==0xffff)return PW_D3D9_PROGRAM_INVALID;
  if(op==0xfffe)n=(t>>16)&0x7fff;
  else if(major>=2)n=(t>>24)&15;
  else {count=sm1_count(op,pixel,minor);if(count<0)return PW_D3D9_PROGRAM_INVALID;n=(unsigned)count;}
  if(n>=available-i)return PW_D3D9_PROGRAM_INVALID;
  i+=1+n;
 }
 return PW_D3D9_PROGRAM_INVALID;
}
struct span {const unsigned char *p;size_t n;};
static int read_span(void *opaque,size_t i,uint32_t *v){struct span *s=opaque;if(i>=s->n)return 1;*v=get(s->p+4*i);return 0;}
int pw_d3d9_program_validate(uint32_t kind,const void *data,size_t size)
{
 const unsigned char *p=data;size_t i,n;struct span s={p,size/4};
 if(!p||!size||size>PW_D3D9_PROGRAM_LIMIT||!kind_ok(kind))return PW_D3D9_PROGRAM_INVALID;
 if(kind!=PW_D3D9_PROGRAM_DECL){if(size%4||pw_d3d9_program_measure(kind,read_span,&s,s.n,&n)||n!=s.n)return PW_D3D9_PROGRAM_INVALID;return 0;}
 if(size%8||size>65*8)return PW_D3D9_PROGRAM_INVALID;
 for(i=0;i<size;i+=8){
  unsigned stream=p[i]|(unsigned)p[i+1]<<8;
  if(stream==255){static const unsigned char end[8]={255,0,0,0,17,0,0,0};return i+8==size&&!memcmp(p+i,end,8)?0:PW_D3D9_PROGRAM_INVALID;}
  if(stream>=16||p[i+4]>=17||p[i+5]>6||p[i+6]>13||p[i+7]>15)return PW_D3D9_PROGRAM_INVALID;
 }
 return PW_D3D9_PROGRAM_INVALID;
}
static int valid(const struct pw_d3d9_program_request *q)
{
 if(!q||!kind_ok(q->kind))return 0;
 switch(q->operation){
 case PW_D3D9_PROGRAM_BEGIN:return !q->transfer&&!q->offset&&!q->count&&q->total&&q->total<=PW_D3D9_PROGRAM_LIMIT&&!(q->total%(q->kind==PW_D3D9_PROGRAM_DECL?8:4))&&(q->kind!=PW_D3D9_PROGRAM_DECL||q->total<=65*8);
 case PW_D3D9_PROGRAM_WRITE:return q->transfer&&!q->total&&q->count&&q->count<=PW_D3D9_PROGRAM_CHUNK&&q->offset<=PW_D3D9_PROGRAM_LIMIT-q->count;
 case PW_D3D9_PROGRAM_COMMIT:case PW_D3D9_PROGRAM_ABORT:return q->transfer&&!q->total&&!q->offset&&!q->count;
 default:return 0;
 }
}
int pw_d3d9_program_encode(void *out,size_t cap,size_t *written,const struct pw_d3d9_program_request *q)
{
 unsigned char *p=out;size_t n;if(!p||!written||!valid(q))return PW_D3D9_PROGRAM_INVALID;n=32+q->count;if(cap<n)return PW_D3D9_PROGRAM_SMALL;
 put(p,1);put(p+4,q->operation);put(p+8,q->kind);put(p+12,q->total);put(p+16,q->offset);put(p+20,q->count);put(p+24,q->transfer);put(p+28,q->transfer>>32);memcpy(p+32,q->data,q->count);*written=n;return 0;
}
int pw_d3d9_program_decode(struct pw_d3d9_program_request *out,const void *data,size_t n)
{
 const unsigned char *p=data;struct pw_d3d9_program_request q={0};if(!out||!p||n<32||get(p)!=1)return PW_D3D9_PROGRAM_INVALID;
 q.operation=get(p+4);q.kind=get(p+8);q.total=get(p+12);q.offset=get(p+16);q.count=get(p+20);q.transfer=get(p+24)|(uint64_t)get(p+28)<<32;
 if(!valid(&q)||n!=32+(size_t)q.count)return PW_D3D9_PROGRAM_INVALID;
 memcpy(q.data,p+32,q.count);*out=q;return 0;
}
static int reply_valid(const struct pw_d3d9_program_reply *q)
{
 if(!q||q->operation<1||q->operation>4)return 0;
 if(q->hresult&0x80000000u)return !q->transfer&&!q->id&&!q->generation;
 if(q->operation==PW_D3D9_PROGRAM_COMMIT)return q->id&&q->generation&&q->transfer;
 return q->transfer&&!q->id&&!q->generation;
}
int pw_d3d9_program_reply_encode(void *out,size_t cap,size_t *written,const struct pw_d3d9_program_reply *q)
{
 unsigned char *p=out;if(!p||!written||!reply_valid(q))return PW_D3D9_PROGRAM_INVALID;if(cap<32)return PW_D3D9_PROGRAM_SMALL;
 put(p,1);put(p+4,q->operation);put(p+8,q->hresult);put(p+12,q->id);put(p+16,q->generation);put(p+20,0);put(p+24,q->transfer);put(p+28,q->transfer>>32);*written=32;return 0;
}
int pw_d3d9_program_reply_decode(struct pw_d3d9_program_reply *out,const void *data,size_t n)
{
 const unsigned char *p=data;struct pw_d3d9_program_reply q;if(!out||!p||n!=32||get(p)!=1||get(p+20))return PW_D3D9_PROGRAM_INVALID;
 q.operation=get(p+4);q.hresult=get(p+8);q.id=get(p+12);q.generation=get(p+16);q.transfer=get(p+24)|(uint64_t)get(p+28)<<32;
 if(!reply_valid(&q))return PW_D3D9_PROGRAM_INVALID;
 *out=q;return 0;
}
void pw_d3d9_program_upload_init(struct pw_d3d9_program_upload *u,void *storage,size_t capacity){memset(u,0,sizeof(*u));u->storage=storage;u->capacity=capacity;}
void pw_d3d9_program_upload_finish(struct pw_d3d9_program_upload *u){u->transfer=0;u->kind=u->total=u->received=u->ready=0;}
int pw_d3d9_program_upload_apply(struct pw_d3d9_program_upload *u,const struct pw_d3d9_program_request *q,uint64_t *transfer)
{
 if(!u||!transfer||!valid(q))return PW_D3D9_PROGRAM_INVALID;
 if(q->operation==PW_D3D9_PROGRAM_BEGIN){
  if(u->transfer)return PW_D3D9_PROGRAM_BUSY;
  if(u->next==UINT64_MAX)return PW_D3D9_PROGRAM_EXHAUSTED;
  if(!u->storage||u->capacity<q->total)return PW_D3D9_PROGRAM_SMALL;
  u->transfer=++u->next;u->kind=q->kind;u->total=q->total;u->received=u->ready=0;*transfer=u->transfer;return 0;
 }
 if(!u->transfer||q->transfer!=u->transfer||q->kind!=u->kind)return PW_D3D9_PROGRAM_STALE;
 if(q->operation==PW_D3D9_PROGRAM_ABORT){*transfer=u->transfer;pw_d3d9_program_upload_finish(u);return 0;}
 if(u->ready)return PW_D3D9_PROGRAM_BUSY;
 if(q->operation==PW_D3D9_PROGRAM_WRITE){
  if(q->offset!=u->received||q->count>u->total-u->received)return PW_D3D9_PROGRAM_INVALID;
  memcpy(u->storage+u->received,q->data,q->count);u->received+=q->count;
 }else {
  if(u->received!=u->total||pw_d3d9_program_validate(u->kind,u->storage,u->total))return PW_D3D9_PROGRAM_INVALID;
  u->ready=1;
 }
 *transfer=u->transfer;return 0;
}
