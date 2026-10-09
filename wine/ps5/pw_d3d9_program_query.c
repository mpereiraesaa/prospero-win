/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_program_query.h"
#include <string.h>
static uint32_t get(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static int valid(const struct pw_d3d9_program_query_request *q)
{
 if(!q||q->kind<PW_D3D9_PROGRAM_DECL||q->kind>PW_D3D9_PROGRAM_PS)return 1;
 if(q->operation==PW_D3D9_PROGRAM_SIZE)return q->offset||q->count;
 if(q->operation!=PW_D3D9_PROGRAM_READ||q->count>PW_D3D9_PROGRAM_CHUNK||q->offset>PW_D3D9_PROGRAM_LIMIT||q->count>PW_D3D9_PROGRAM_LIMIT-q->offset)return 1;
 return 0;
}
int pw_d3d9_program_query_encode(void *out,size_t cap,size_t *n,const struct pw_d3d9_program_query_request *q)
{
 unsigned char *p=out;if(!p||!n||valid(q))return 1;if(cap<24)return 2;
 put(p,1);put(p+4,q->operation);put(p+8,q->kind);put(p+12,q->capacity);put(p+16,q->offset);put(p+20,q->count);*n=24;return 0;
}
int pw_d3d9_program_query_decode(struct pw_d3d9_program_query_request *out,const void *data,size_t n)
{
 const unsigned char *p=data;struct pw_d3d9_program_query_request q;
 if(!p||!out||n!=24||get(p)!=1)return 1;
 q=(struct pw_d3d9_program_query_request){get(p+4),get(p+8),get(p+12),get(p+16),get(p+20)};
 if(valid(&q))return 1;
 *out=q;return 0;
}
static int reply_valid(const struct pw_d3d9_program_query_request *q,const struct pw_d3d9_program_query_reply *r)
{
 uint32_t available,expected;
 if(valid(q)||!r||r->operation!=q->operation||r->kind!=q->kind)return 1;
 if(r->hresult&0x80000000u)return r->total||r->offset||r->count;
 if(!r->total||r->total>PW_D3D9_PROGRAM_LIMIT||r->total%(q->kind==PW_D3D9_PROGRAM_DECL?8:4))return 1;
 if(q->kind==PW_D3D9_PROGRAM_DECL&&r->total>65*8)return 1;
 if(q->operation==PW_D3D9_PROGRAM_SIZE)return r->offset||r->count||r->size!=r->total/(q->kind==PW_D3D9_PROGRAM_DECL?8:1);
 available=q->kind==PW_D3D9_PROGRAM_DECL?r->total:(q->capacity<r->total?q->capacity:r->total);
 if(q->offset>available)return 1;
 expected=available-q->offset;if(expected>q->count)expected=q->count;
 return r->offset!=q->offset||r->count!=expected;
}
int pw_d3d9_program_query_reply_encode(void *out,size_t cap,size_t *n,const struct pw_d3d9_program_query_request *q,const struct pw_d3d9_program_query_reply *r)
{
 unsigned char *p=out;if(!p||!n||reply_valid(q,r))return 1;if(cap<32+r->count)return 2;
 put(p,1);put(p+4,r->operation);put(p+8,r->kind);put(p+12,r->hresult);put(p+16,r->size);put(p+20,r->total);put(p+24,r->offset);put(p+28,r->count);
 memcpy(p+32,r->data,r->count);*n=32+r->count;return 0;
}
int pw_d3d9_program_query_reply_decode(struct pw_d3d9_program_query_reply *out,const struct pw_d3d9_program_query_request *q,const void *data,size_t n)
{
 const unsigned char *p=data;struct pw_d3d9_program_query_reply r={0};
 if(!p||!out||n<32||n>32+PW_D3D9_PROGRAM_CHUNK||get(p)!=1)return 1;
 r.operation=get(p+4);r.kind=get(p+8);r.hresult=get(p+12);r.size=get(p+16);r.total=get(p+20);r.offset=get(p+24);r.count=get(p+28);
 if(r.count>PW_D3D9_PROGRAM_CHUNK||n!=32+r.count||reply_valid(q,&r))return 1;
 memcpy(r.data,p+32,r.count);*out=r;return 0;
}
